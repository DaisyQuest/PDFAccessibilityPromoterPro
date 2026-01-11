#include "pap/job_queue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#define HTTP_BUFFER_SIZE 8192
#define HTTP_PATH_SIZE 512
#define HTTP_UUID_SIZE 128
#define HTTP_METHOD_SIZE 16
#define HTTP_MAX_REQUEST_BYTES 8192
#define HTTP_MAX_HEADER_LINES 50
#define HTTP_REQUEST_LINE_TIMEOUT_MS 2000
#define HTTP_HEADERS_TIMEOUT_MS 5000
#define HTTP_SOCKET_TIMEOUT_MS 1000
#define HTTP_MAX_CHILDREN 32

typedef enum {
    READ_OK = 0,
    READ_TIMEOUT = 1,
    READ_TOO_LARGE = 2,
    READ_TOO_MANY_HEADERS = 3,
    READ_ERROR = 4
} read_result_t;

static int url_decode(const char *src, char *dst, size_t dst_len);

static volatile sig_atomic_t active_children = 0;

static void handle_sigchld(int signal_id) {
    (void)signal_id;
    int status = 0;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        if (active_children > 0) {
            active_children--;
        }
    }
}

static int write_all(int fd, const char *buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        ssize_t written = write(fd, buffer + total, length - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        total += (size_t)written;
    }
    return 200;
}

static int send_response(int client_fd, int status, const char *status_text, const char *body) {
    char response[HTTP_BUFFER_SIZE];
    int body_len = body ? (int)strlen(body) : 0;
    int written = snprintf(response, sizeof(response),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n\r\n"
                           "%s",
                           status, status_text, body_len, body ? body : "");
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return status;
    }
    write_all(client_fd, response, (size_t)written);
    return status;
}

static long long elapsed_ms(const struct timespec *start, const struct timespec *now) {
    long long seconds = now->tv_sec - start->tv_sec;
    long long nsec = now->tv_nsec - start->tv_nsec;
    return (seconds * 1000LL) + (nsec / 1000000LL);
}

static int count_header_lines(const char *buffer, size_t len) {
    int lines = 0;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            lines++;
        }
    }
    return lines;
}

static read_result_t read_request(int client_fd, char *buffer, size_t cap, size_t *out_len) {
    size_t total = 0;
    int saw_request_line = 0;
    struct timespec start_time;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        return READ_ERROR;
    }

    while (total + 1 < cap) {
        ssize_t bytes = recv(client_fd, buffer + total, cap - total - 1, 0);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                    return READ_ERROR;
                }
                long long elapsed = elapsed_ms(&start_time, &now);
                if (!saw_request_line && elapsed >= HTTP_REQUEST_LINE_TIMEOUT_MS) {
                    return READ_TIMEOUT;
                }
                if (elapsed >= HTTP_HEADERS_TIMEOUT_MS) {
                    return READ_TIMEOUT;
                }
                continue;
            }
            return READ_ERROR;
        }
        if (bytes == 0) {
            break;
        }
        total += (size_t)bytes;
        buffer[total] = '\0';

        if (total >= HTTP_MAX_REQUEST_BYTES) {
            return READ_TOO_LARGE;
        }

        if (!saw_request_line && strstr(buffer, "\r\n") != NULL) {
            saw_request_line = 1;
        }

        if (strstr(buffer, "\r\n\r\n") != NULL) {
            int lines = count_header_lines(buffer, total);
            if (lines > HTTP_MAX_HEADER_LINES) {
                return READ_TOO_MANY_HEADERS;
            }
            *out_len = total;
            return READ_OK;
        }
    }

    return READ_TOO_LARGE;
}

typedef enum {
    SEND_OK = 0,
    SEND_NOT_FOUND = 1,
    SEND_IO_ERROR = 2
} send_result_t;

static send_result_t send_file(int client_fd, const char *content_type, const char *path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        if (errno == ENOENT) {
            return SEND_NOT_FOUND;
        }
        return SEND_IO_ERROR;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        return SEND_IO_ERROR;
    }

    char header[HTTP_BUFFER_SIZE];
    int written = snprintf(header, sizeof(header),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %lld\r\n"
                           "Connection: close\r\n\r\n",
                           content_type, (long long)st.st_size);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        close(file_fd);
        return SEND_IO_ERROR;
    }

    if (!write_all(client_fd, header, (size_t)written)) {
        close(file_fd);
        return SEND_IO_ERROR;
    }

    off_t offset = 0;
    while (offset < st.st_size) {
        ssize_t sent = sendfile(client_fd, file_fd, &offset, (size_t)(st.st_size - offset));
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (sent == 0) {
            break;
        }
    }

    if (offset < st.st_size) {
        if (lseek(file_fd, offset, SEEK_SET) == (off_t)-1) {
            close(file_fd);
            return SEND_IO_ERROR;
        }
        char buffer[HTTP_BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
            if (!write_all(client_fd, buffer, (size_t)bytes_read)) {
                close(file_fd);
                return SEND_IO_ERROR;
            }
        }
        if (bytes_read < 0) {
            close(file_fd);
            return SEND_IO_ERROR;
        }
    }

    close(file_fd);
    return SEND_OK;
}

static int constant_time_equals(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a != len_b) {
        return 0;
    }
    unsigned char result = 0;
    for (size_t i = 0; i < len_a; ++i) {
        result |= (unsigned char)(a[i] ^ b[i]);
    }
    return result == 0;
}

static void sanitize_for_log(const char *input, char *output, size_t output_len) {
    if (!input || !output || output_len == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; input[i] != '\0' && out + 1 < output_len; ++i) {
        unsigned char ch = (unsigned char)input[i];
        if (isprint(ch)) {
            output[out++] = (char)ch;
        } else {
            output[out++] = '?';
        }
    }
    output[out] = '\0';
}

static void build_log_path(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len == 0) {
        return;
    }
    char raw_path[HTTP_PATH_SIZE];
    size_t i = 0;
    while (path[i] != '\0' && path[i] != '?' && i + 1 < sizeof(raw_path)) {
        raw_path[i] = path[i];
        i++;
    }
    raw_path[i] = '\0';
    char decoded[HTTP_PATH_SIZE];
    if (url_decode(raw_path, decoded, sizeof(decoded))) {
        sanitize_for_log(decoded, out, out_len);
    } else {
        sanitize_for_log(raw_path, out, out_len);
    }
}

static int get_header_value(const char *request, const char *header_name, char *out, size_t out_len) {
    if (!request || !header_name || !out || out_len == 0) {
        return 0;
    }
    const char *line = strstr(request, "\r\n");
    if (!line) {
        return 0;
    }
    line += 2;
    while (*line != '\0') {
        if (line[0] == '\r' && line[1] == '\n') {
            break;
        }
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) {
            break;
        }
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon) {
            size_t name_len = (size_t)(colon - line);
            if (strlen(header_name) == name_len && strncasecmp(line, header_name, name_len) == 0) {
                const char *value_start = colon + 1;
                while (value_start < line_end && (*value_start == ' ' || *value_start == '\t')) {
                    value_start++;
                }
                const char *value_end = line_end;
                while (value_end > value_start && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
                    value_end--;
                }
                size_t value_len = (size_t)(value_end - value_start);
                if (value_len + 1 > out_len) {
                    return 0;
                }
                memcpy(out, value_start, value_len);
                out[value_len] = '\0';
                return 1;
            }
        }
        line = line_end + 2;
    }
    return 0;
}

static int extract_bearer_token(const char *value, char *out, size_t out_len) {
    if (!value || !out || out_len == 0) {
        return 0;
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    const char *prefix = "Bearer";
    size_t prefix_len = strlen(prefix);
    if (strncasecmp(value, prefix, prefix_len) != 0) {
        return 0;
    }
    value += prefix_len;
    if (*value != ' ' && *value != '\t') {
        return 0;
    }
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    size_t value_len = strlen(value);
    while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) {
        value_len--;
    }
    if (value_len == 0 || value_len + 1 > out_len) {
        return 0;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    return 200;
}

static int has_control_chars(const char *value, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (iscntrl(ch)) {
            return 1;
        }
    }
    return 0;
}

static int parse_request_line(const char *request,
                              size_t request_len,
                              char *method,
                              size_t method_len,
                              char *path,
                              size_t path_len,
                              char *version,
                              size_t version_len) {
    if (!request || !method || !path || !version) {
        return 0;
    }

    const char *line_end = strstr(request, "\r\n");
    if (!line_end) {
        return 0;
    }
    size_t line_len = (size_t)(line_end - request);
    if (line_len == 0 || line_len >= request_len) {
        return 0;
    }

    const char *first_space = memchr(request, ' ', line_len);
    if (!first_space) {
        return 0;
    }
    const char *second_space = memchr(first_space + 1, ' ', (size_t)(line_end - (first_space + 1)));
    if (!second_space) {
        return 0;
    }

    size_t method_size = (size_t)(first_space - request);
    size_t path_size = (size_t)(second_space - first_space - 1);
    size_t version_size = (size_t)(line_end - second_space - 1);

    if (method_size == 0 || method_size >= method_len || method_size > HTTP_METHOD_SIZE) {
        return 0;
    }
    if (path_size == 0 || path_size >= path_len || path_size > HTTP_PATH_SIZE) {
        return 0;
    }
    if (version_size == 0 || version_size >= version_len) {
        return 0;
    }

    if (has_control_chars(request, line_len)) {
        return 0;
    }

    memcpy(method, request, method_size);
    method[method_size] = '\0';
    memcpy(path, first_space + 1, path_size);
    path[path_size] = '\0';
    memcpy(version, second_space + 1, version_size);
    version[version_size] = '\0';

    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
        return 0;
    }

    return 200;
}

static int parse_state(const char *value, jq_state_t *state_out) {
    if (!value || !state_out) {
        return 0;
    }
    if (strcmp(value, "jobs") == 0) {
        *state_out = JQ_STATE_JOBS;
        return 1;
    }
    if (strcmp(value, "priority") == 0) {
        *state_out = JQ_STATE_PRIORITY;
        return 1;
    }
    if (strcmp(value, "complete") == 0) {
        *state_out = JQ_STATE_COMPLETE;
        return 1;
    }
    if (strcmp(value, "error") == 0) {
        *state_out = JQ_STATE_ERROR;
        return 1;
    }
    return 0;
}

static int is_valid_uuid(const char *value) {
    if (!value) {
        return 0;
    }
    size_t len = strlen(value);
    if (len == 0 || len >= HTTP_UUID_SIZE) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') {
            continue;
        }
        return 0;
    }
    return 200;
}

static int is_safe_relpath(const char *value) {
    if (!value || value[0] == '\0') {
        return 0;
    }
    if (value[0] == '/' || value[0] == '\\') {
        return 0;
    }
    const char *segment_start = value;
    for (const char *cursor = value; ; ++cursor) {
        char ch = *cursor;
        if (ch == '\0' || ch == '/') {
            size_t segment_len = (size_t)(cursor - segment_start);
            if (segment_len == 0) {
                return 0;
            }
            if (segment_len == 1 && segment_start[0] == '.') {
                return 0;
            }
            if (segment_len == 2 && segment_start[0] == '.' && segment_start[1] == '.') {
                return 0;
            }
            if (ch == '\0') {
                break;
            }
            segment_start = cursor + 1;
            continue;
        }
        if (iscntrl((unsigned char)ch) || ch == ':' || ch == '\\') {
            return 0;
        }
    }
    return 1;
}

static int is_path_under_root(const char *root_real, const char *path_real) {
    size_t root_len = strlen(root_real);
    if (strncmp(root_real, path_real, root_len) != 0) {
        return 0;
    }
    if (path_real[root_len] == '\0') {
        return 1;
    }
    return path_real[root_len] == '/';
}

static int build_rooted_path(const char *root_real, const char *relpath, char *out, size_t out_len) {
    if (!root_real || !relpath || !out) {
        return 0;
    }
    int written = snprintf(out, out_len, "%s/%s", root_real, relpath);
    if (written < 0 || (size_t)written >= out_len) {
        return 0;
    }
    return 1;
}

static int resolve_existing_under_root(const char *root_real,
                                       const char *path,
                                       char *resolved,
                                       size_t resolved_len,
                                       int *status_out) {
    if (!root_real || !path || !resolved || !status_out) {
        return 0;
    }
    (void)resolved_len;
    if (!realpath(path, resolved)) {
        if (errno == ENOENT) {
            *status_out = 404;
        } else {
            *status_out = 500;
        }
        return 0;
    }
    if (!is_path_under_root(root_real, resolved)) {
        *status_out = 403;
        return 0;
    }
    *status_out = 200;
    return 1;
}

static const char *state_to_string(jq_state_t state) {
    switch (state) {
        case JQ_STATE_JOBS:
            return "jobs";
        case JQ_STATE_PRIORITY:
            return "priority";
        case JQ_STATE_COMPLETE:
            return "complete";
        case JQ_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static int hex_to_int(char ch, int *value_out) {
    if (ch >= '0' && ch <= '9') {
        *value_out = ch - '0';
        return 1;
    }
    if (ch >= 'a' && ch <= 'f') {
        *value_out = 10 + (ch - 'a');
        return 1;
    }
    if (ch >= 'A' && ch <= 'F') {
        *value_out = 10 + (ch - 'A');
        return 1;
    }
    return 0;
}

static int url_decode(const char *src, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) {
        return 0;
    }
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; ++i) {
        char ch = src[i];
        if (ch == '%') {
            int high = 0;
            int low = 0;
            if (src[i + 1] == '\0' || src[i + 2] == '\0') {
                return 0;
            }
            if (!hex_to_int(src[i + 1], &high) || !hex_to_int(src[i + 2], &low)) {
                return 0;
            }
            unsigned char decoded = (unsigned char)((high << 4) | low);
            if (out + 1 >= dst_len) {
                return 0;
            }
            dst[out++] = (char)decoded;
            i += 2;
            continue;
        }
        if (ch == '+') {
            ch = ' ';
        }
        if (out + 1 >= dst_len) {
            return 0;
        }
        dst[out++] = ch;
    }
    dst[out] = '\0';
    return 1;
}

static int get_query_param(const char *query, const char *key, char *out, size_t out_len) {
    if (!query || !key || !out) {
        return 0;
    }

    size_t key_len = strlen(key);
    const char *cursor = query;
    while (*cursor) {
        const char *pair_end = strchr(cursor, '&');
        size_t pair_len = pair_end ? (size_t)(pair_end - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', pair_len);
        if (equals) {
            size_t current_key_len = (size_t)(equals - cursor);
            if (current_key_len == key_len && strncmp(cursor, key, key_len) == 0) {
                const char *value_start = equals + 1;
                size_t value_len = pair_len - (size_t)(value_start - cursor);
                char encoded_value[HTTP_PATH_SIZE];
                if (value_len + 1 > sizeof(encoded_value)) {
                    return 0;
                }
                memcpy(encoded_value, value_start, value_len);
                encoded_value[value_len] = '\0';
                if (!url_decode(encoded_value, out, out_len)) {
                    return 0;
                }
                return 1;
            }
        }
        if (!pair_end) {
            break;
        }
        cursor = pair_end + 1;
    }
    return 0;
}

static int handle_claim(const char *root, const char *query, int client_fd) {
    char prefer_value[8];
    int prefer_priority = 0;
    if (get_query_param(query, "prefer_priority", prefer_value, sizeof(prefer_value))) {
        prefer_priority = strcmp(prefer_value, "1") == 0;
    }

    char uuid[HTTP_UUID_SIZE];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t result = jq_claim_next(root, prefer_priority, uuid, sizeof(uuid), &state);

    if (result == JQ_OK) {
        char body[HTTP_BUFFER_SIZE];
        int written = snprintf(body, sizeof(body), "%s %s\n", uuid, state_to_string(state));
        if (written < 0 || (size_t)written >= sizeof(body)) {
            return send_response(client_fd, 500, "Internal Server Error", "response too large");
        }
        return send_response(client_fd, 200, "OK", body);
    }

    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "no jobs\n");
    }

    if (result == JQ_ERR_INVALID_ARGUMENT) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    return send_response(client_fd, 500, "Internal Server Error", "io error\n");
}

static int handle_release(const char *root, const char *query, int client_fd) {
    char uuid[HTTP_UUID_SIZE];
    char state_value[32];
    if (!get_query_param(query, "uuid", uuid, sizeof(uuid)) ||
        !get_query_param(query, "state", state_value, sizeof(state_value))) {
        return send_response(client_fd, 400, "Bad Request", "missing parameters\n");
    }
    if (!is_valid_uuid(uuid)) {
        return send_response(client_fd, 400, "Bad Request", "invalid uuid\n");
    }

    jq_state_t state;
    if (!parse_state(state_value, &state)) {
        return send_response(client_fd, 400, "Bad Request", "invalid state\n");
    }

    jq_result_t result = jq_release(root, uuid, state);
    if (result == JQ_OK) {
        return send_response(client_fd, 200, "OK", "released\n");
    }

    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job not found\n");
    }

    if (result == JQ_ERR_INVALID_ARGUMENT) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    return send_response(client_fd, 500, "Internal Server Error", "io error\n");
}

static int handle_finalize(const char *root, const char *query, int client_fd) {
    char uuid[HTTP_UUID_SIZE];
    char from_value[32];
    char to_value[32];
    if (!get_query_param(query, "uuid", uuid, sizeof(uuid)) ||
        !get_query_param(query, "from", from_value, sizeof(from_value)) ||
        !get_query_param(query, "to", to_value, sizeof(to_value))) {
        return send_response(client_fd, 400, "Bad Request", "missing parameters\n");
    }
    if (!is_valid_uuid(uuid)) {
        return send_response(client_fd, 400, "Bad Request", "invalid uuid\n");
    }

    jq_state_t from_state;
    jq_state_t to_state;
    if (!parse_state(from_value, &from_state) || !parse_state(to_value, &to_state)) {
        return send_response(client_fd, 400, "Bad Request", "invalid state\n");
    }

    jq_result_t result = jq_finalize(root, uuid, from_state, to_state);
    if (result == JQ_OK) {
        return send_response(client_fd, 200, "OK", "finalized\n");
    }

    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job not found\n");
    }

    if (result == JQ_ERR_INVALID_ARGUMENT) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    return send_response(client_fd, 500, "Internal Server Error", "io error\n");
}

static int handle_submit(const char *root, const char *query, int client_fd) {
    char uuid[HTTP_UUID_SIZE];
    char pdf_path[HTTP_PATH_SIZE];
    char metadata_path[HTTP_PATH_SIZE];
    char priority_value[8];

    if (!get_query_param(query, "uuid", uuid, sizeof(uuid)) ||
        !get_query_param(query, "pdf", pdf_path, sizeof(pdf_path)) ||
        !get_query_param(query, "metadata", metadata_path, sizeof(metadata_path))) {
        return send_response(client_fd, 400, "Bad Request", "missing parameters\n");
    }
    if (!is_valid_uuid(uuid)) {
        return send_response(client_fd, 400, "Bad Request", "invalid uuid\n");
    }
    if (!is_safe_relpath(pdf_path) || !is_safe_relpath(metadata_path)) {
        return send_response(client_fd, 400, "Bad Request", "invalid path\n");
    }

    char pdf_full[PATH_MAX];
    char metadata_full[PATH_MAX];
    if (!build_rooted_path(root, pdf_path, pdf_full, sizeof(pdf_full)) ||
        !build_rooted_path(root, metadata_path, metadata_full, sizeof(metadata_full))) {
        return send_response(client_fd, 400, "Bad Request", "path too long\n");
    }

    char pdf_resolved[PATH_MAX];
    char metadata_resolved[PATH_MAX];
    int status = 0;
    if (!resolve_existing_under_root(root, pdf_full, pdf_resolved, sizeof(pdf_resolved), &status) ||
        !resolve_existing_under_root(root, metadata_full, metadata_resolved, sizeof(metadata_resolved), &status)) {
        if (status == 403) {
            return send_response(client_fd, 403, "Forbidden", "path outside root\n");
        }
        if (status == 404) {
            return send_response(client_fd, 404, "Not Found", "file not found\n");
        }
        return send_response(client_fd, 500, "Internal Server Error", "io error\n");
    }

    int priority = 0;
    if (get_query_param(query, "priority", priority_value, sizeof(priority_value))) {
        priority = strcmp(priority_value, "1") == 0;
    }

    jq_result_t result = jq_submit(root, uuid, pdf_resolved, metadata_resolved, priority);
    if (result == JQ_OK) {
        return send_response(client_fd, 200, "OK", "submitted\n");
    }

    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "file not found\n");
    }

    if (result == JQ_ERR_INVALID_ARGUMENT) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    return send_response(client_fd, 500, "Internal Server Error", "io error\n");
}

static int handle_move(const char *root, const char *query, int client_fd) {
    char uuid[HTTP_UUID_SIZE];
    char from_value[32];
    char to_value[32];
    if (!get_query_param(query, "uuid", uuid, sizeof(uuid)) ||
        !get_query_param(query, "from", from_value, sizeof(from_value)) ||
        !get_query_param(query, "to", to_value, sizeof(to_value))) {
        return send_response(client_fd, 400, "Bad Request", "missing parameters\n");
    }
    if (!is_valid_uuid(uuid)) {
        return send_response(client_fd, 400, "Bad Request", "invalid uuid\n");
    }

    jq_state_t from_state;
    jq_state_t to_state;
    if (!parse_state(from_value, &from_state) || !parse_state(to_value, &to_state)) {
        return send_response(client_fd, 400, "Bad Request", "invalid state\n");
    }

    jq_result_t result = jq_move(root, uuid, from_state, to_state);
    if (result == JQ_OK) {
        return send_response(client_fd, 200, "OK", "moved\n");
    }

    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job not found\n");
    }

    if (result == JQ_ERR_INVALID_ARGUMENT) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    return send_response(client_fd, 500, "Internal Server Error", "io error\n");
}

static int handle_status(const char *root, const char *query, int client_fd) {
    char uuid[HTTP_UUID_SIZE];
    if (!get_query_param(query, "uuid", uuid, sizeof(uuid))) {
        return send_response(client_fd, 400, "Bad Request", "missing parameters\n");
    }
    if (!is_valid_uuid(uuid)) {
        return send_response(client_fd, 400, "Bad Request", "invalid uuid\n");
    }

    jq_state_t state = JQ_STATE_JOBS;
    int locked = 0;
    jq_result_t result = jq_status(root, uuid, &state, &locked);
    if (result == JQ_OK) {
        char body[HTTP_BUFFER_SIZE];
        int written = snprintf(body, sizeof(body), "state=%s locked=%d\n", state_to_string(state), locked);
        if (written < 0 || (size_t)written >= sizeof(body)) {
            return send_response(client_fd, 500, "Internal Server Error", "response too large");
        }
        return send_response(client_fd, 200, "OK", body);
    }

    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job not found\n");
    }

    if (result == JQ_ERR_INVALID_ARGUMENT) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    return send_response(client_fd, 500, "Internal Server Error", "io error\n");
}

static int handle_retrieve(const char *root, const char *query, int client_fd) {
    char uuid[HTTP_UUID_SIZE];
    char state_value[32];
    char kind_value[32];
    if (!get_query_param(query, "uuid", uuid, sizeof(uuid)) ||
        !get_query_param(query, "state", state_value, sizeof(state_value)) ||
        !get_query_param(query, "kind", kind_value, sizeof(kind_value))) {
        return send_response(client_fd, 400, "Bad Request", "missing parameters\n");
    }
    if (!is_valid_uuid(uuid)) {
        return send_response(client_fd, 400, "Bad Request", "invalid uuid\n");
    }

    jq_state_t state;
    if (!parse_state(state_value, &state)) {
        return send_response(client_fd, 400, "Bad Request", "invalid state\n");
    }

    int send_pdf = strcmp(kind_value, "pdf") == 0;
    int send_metadata = strcmp(kind_value, "metadata") == 0;
    if (!send_pdf && !send_metadata) {
        return send_response(client_fd, 400, "Bad Request", "invalid kind\n");
    }

    char pdf_path[HTTP_PATH_SIZE];
    char metadata_path[HTTP_PATH_SIZE];
    jq_result_t path_result =
        jq_job_paths(root, uuid, state, pdf_path, sizeof(pdf_path), metadata_path, sizeof(metadata_path));
    if (path_result != JQ_OK) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    const char *path = send_pdf ? pdf_path : metadata_path;
    char resolved[PATH_MAX];
    int status = 0;
    if (!resolve_existing_under_root(root, path, resolved, sizeof(resolved), &status)) {
        if (status == 403) {
            return send_response(client_fd, 403, "Forbidden", "path outside root\n");
        }
        if (status == 404) {
            return send_response(client_fd, 404, "Not Found", "job not found\n");
        }
        return send_response(client_fd, 500, "Internal Server Error", "io error\n");
    }

    const char *content_type = send_pdf ? "application/pdf" : "application/json";
    send_result_t send_result = send_file(client_fd, content_type, resolved);
    if (send_result == SEND_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job not found\n");
    }
    if (send_result != SEND_OK) {
        return send_response(client_fd, 500, "Internal Server Error", "io error\n");
    }

    return 200;
}

static int is_authorized(const char *token_config, const char *auth_header, const char *query) {
    if (!token_config || token_config[0] == '\0') {
        return 1;
    }

    char header_token[HTTP_UUID_SIZE];
    if (auth_header && extract_bearer_token(auth_header, header_token, sizeof(header_token))) {
        if (constant_time_equals(header_token, token_config)) {
            return 1;
        }
    }

    char query_token[HTTP_UUID_SIZE];
    if (get_query_param(query, "token", query_token, sizeof(query_token))) {
        if (constant_time_equals(query_token, token_config)) {
            return 1;
        }
    }

    return 0;
}

static int route_request(const char *root,
                         const char *method,
                         const char *path,
                         const char *auth_header,
                         const char *token_config,
                         int client_fd);

static void handle_client_connection(int client_fd,
                                     const struct sockaddr_in *client_addr,
                                     const char *root_real,
                                     const char *token_config) {
    struct timespec start_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        memset(&start_time, 0, sizeof(start_time));
    }
    struct timeval timeout;
    timeout.tv_sec = HTTP_SOCKET_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HTTP_SOCKET_TIMEOUT_MS % 1000) * 1000;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        return;
    }
    if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        return;
    }

    char buffer[HTTP_BUFFER_SIZE];
    size_t bytes = 0;
    read_result_t read_result = read_request(client_fd, buffer, sizeof(buffer), &bytes);
    int status = 0;
    if (read_result != READ_OK) {
        if (read_result == READ_TIMEOUT) {
            status = send_response(client_fd, 408, "Request Timeout", "request timeout\n");
        } else if (read_result == READ_TOO_LARGE) {
            status = send_response(client_fd, 413, "Payload Too Large", "request too large\n");
        } else if (read_result == READ_TOO_MANY_HEADERS) {
            status = send_response(client_fd, 400, "Bad Request", "too many headers\n");
        } else {
            status = send_response(client_fd, 500, "Internal Server Error", "io error\n");
        }
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        long long latency = elapsed_ms(&start_time, &end_time);
        char addr_buffer[INET_ADDRSTRLEN];
        const char *addr = "unknown";
        if (client_addr && inet_ntop(AF_INET, &client_addr->sin_addr, addr_buffer, sizeof(addr_buffer))) {
            addr = addr_buffer;
        }
        fprintf(stdout, "%s %s %s %d %lld\n", addr, "-", "-", status, latency);
        fflush(stdout);
        return;
    }

    char method[HTTP_METHOD_SIZE];
    char path[HTTP_PATH_SIZE];
    char version[16];
    if (!parse_request_line(buffer, bytes, method, sizeof(method), path, sizeof(path), version, sizeof(version))) {
        status = send_response(client_fd, 400, "Bad Request", "invalid request\n");
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        long long latency = elapsed_ms(&start_time, &end_time);
        char addr_buffer[INET_ADDRSTRLEN];
        const char *addr = "unknown";
        if (client_addr && inet_ntop(AF_INET, &client_addr->sin_addr, addr_buffer, sizeof(addr_buffer))) {
            addr = addr_buffer;
        }
        fprintf(stdout, "%s %s %s %d %lld\n", addr, "-", "-", status, latency);
        fflush(stdout);
        return;
    }
    (void)version;

    char auth_header[256];
    const char *auth_value = NULL;
    if (get_header_value(buffer, "Authorization", auth_header, sizeof(auth_header))) {
        auth_value = auth_header;
    }

    status = route_request(root_real, method, path, auth_value, token_config, client_fd);

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long long latency = elapsed_ms(&start_time, &end_time);
    char addr_buffer[INET_ADDRSTRLEN];
    const char *addr = "unknown";
    if (client_addr && inet_ntop(AF_INET, &client_addr->sin_addr, addr_buffer, sizeof(addr_buffer))) {
        addr = addr_buffer;
    }
    char log_path[HTTP_PATH_SIZE];
    build_log_path(path, log_path, sizeof(log_path));
    fprintf(stdout, "%s %s %s %d %lld\n", addr, method, log_path, status, latency);
    fflush(stdout);
}

static int route_request(const char *root,
                         const char *method,
                         const char *path,
                         const char *auth_header,
                         const char *token_config,
                         int client_fd) {
    if (strcmp(method, "GET") != 0) {
        return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
    }

    char path_buffer[HTTP_PATH_SIZE];
    int written = snprintf(path_buffer, sizeof(path_buffer), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(path_buffer)) {
        return send_response(client_fd, 400, "Bad Request", "path too long\n");
    }
    char *query = strchr(path_buffer, '?');
    if (query) {
        *query = '\0';
        query += 1;
    } else {
        query = "";
    }

    char decoded_path[HTTP_PATH_SIZE];
    if (!url_decode(path_buffer, decoded_path, sizeof(decoded_path))) {
        return send_response(client_fd, 400, "Bad Request", "invalid path encoding\n");
    }

    if (strcmp(decoded_path, "/health") == 0) {
        return send_response(client_fd, 200, "OK", "ok\n");
    }

    if (!is_authorized(token_config, auth_header, query)) {
        return send_response(client_fd, 401, "Unauthorized", "unauthorized\n");
    }

    if (strcmp(decoded_path, "/submit") == 0) {
        return handle_submit(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/claim") == 0) {
        return handle_claim(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/release") == 0) {
        return handle_release(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/finalize") == 0) {
        return handle_finalize(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/move") == 0) {
        return handle_move(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/status") == 0) {
        return handle_status(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/retrieve") == 0) {
        return handle_retrieve(root, query, client_fd);
    }

    return send_response(client_fd, 404, "Not Found", "unknown endpoint\n");
}

#ifndef JQ_HTTP_TEST
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: job_queue_http <root> <port> [--bind <addr>] [--token <token>]\n");
        return 1;
    }

    const char *root = argv[1];
    char root_real[PATH_MAX];
    if (!realpath(root, root_real)) {
        perror("realpath");
        return 1;
    }
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }

    const char *bind_addr = "127.0.0.1";
    const char *token_config = getenv("JOB_QUEUE_TOKEN");
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--bind") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --bind\n");
                return 1;
            }
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "--token") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --token\n");
                return 1;
            }
            token_config = argv[++i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (token_config && token_config[0] == '\0') {
        token_config = NULL;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address\n");
        close(server_fd);
        return 1;
    }
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) != 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        perror("sigaction");
        close(server_fd);
        return 1;
    }

    printf("listening on %s:%d\n", bind_addr, port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        if (active_children >= HTTP_MAX_CHILDREN) {
            send_response(client_fd, 503, "Service Unavailable", "server busy\n");
            close(client_fd);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            close(server_fd);
            handle_client_connection(client_fd, &client_addr, root_real, token_config);
            close(client_fd);
            _exit(0);
        }
        active_children++;
        close(client_fd);
    }

    close(server_fd);
    return 1;
}
#else
static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int test_url_decode(void) {
    char decoded[32];
    if (!assert_true(url_decode("hello%20world", decoded, sizeof(decoded)), "decode space")) {
        return 0;
    }
    if (!assert_true(strcmp(decoded, "hello world") == 0, "decoded value")) {
        return 0;
    }
    if (!assert_true(url_decode("a+b", decoded, sizeof(decoded)), "decode plus")) {
        return 0;
    }
    if (!assert_true(strcmp(decoded, "a b") == 0, "plus to space")) {
        return 0;
    }
    if (!assert_true(!url_decode("%ZZ", decoded, sizeof(decoded)), "invalid percent")) {
        return 0;
    }
    if (!assert_true(!url_decode("%1", decoded, sizeof(decoded)), "short percent")) {
        return 0;
    }
    return 1;
}

static int test_parse_request_line(void) {
    char method[HTTP_METHOD_SIZE];
    char path[HTTP_PATH_SIZE];
    char version[16];
    const char *request = "GET /health HTTP/1.1\r\nHost: test\r\n\r\n";
    if (!assert_true(parse_request_line(request, strlen(request), method, sizeof(method), path, sizeof(path),
                                        version, sizeof(version)),
                     "parse request line")) {
        return 0;
    }
    if (!assert_true(strcmp(method, "GET") == 0, "method parsed")) {
        return 0;
    }
    if (!assert_true(strcmp(path, "/health") == 0, "path parsed")) {
        return 0;
    }
    if (!assert_true(strcmp(version, "HTTP/1.1") == 0, "version parsed")) {
        return 0;
    }
    const char *bad_request = "GET /health HTTP/2\r\n\r\n";
    if (!assert_true(!parse_request_line(bad_request, strlen(bad_request), method, sizeof(method), path, sizeof(path),
                                         version, sizeof(version)),
                     "reject bad version")) {
        return 0;
    }
    const char *control_request = "GE\tT / HTTP/1.1\r\n\r\n";
    if (!assert_true(!parse_request_line(control_request, strlen(control_request), method, sizeof(method),
                                         path, sizeof(path), version, sizeof(version)),
                     "reject control chars")) {
        return 0;
    }
    return 1;
}

static int test_is_safe_relpath(void) {
    if (!assert_true(is_safe_relpath("docs/file.pdf"), "safe path")) {
        return 0;
    }
    if (!assert_true(!is_safe_relpath("/absolute"), "reject absolute")) {
        return 0;
    }
    if (!assert_true(!is_safe_relpath("../escape"), "reject parent")) {
        return 0;
    }
    if (!assert_true(!is_safe_relpath("dir/../file"), "reject parent segment")) {
        return 0;
    }
    if (!assert_true(!is_safe_relpath("dir//file"), "reject empty segment")) {
        return 0;
    }
    if (!assert_true(!is_safe_relpath("dir/./file"), "reject dot segment")) {
        return 0;
    }
    return 1;
}

static int test_token_compare(void) {
    if (!assert_true(constant_time_equals("token", "token"), "token match")) {
        return 0;
    }
    if (!assert_true(!constant_time_equals("token", "other"), "token mismatch")) {
        return 0;
    }
    return 1;
}

static int test_extract_bearer(void) {
    char token[32];
    if (!assert_true(extract_bearer_token("Bearer abc123", token, sizeof(token)), "extract bearer")) {
        return 0;
    }
    if (!assert_true(strcmp(token, "abc123") == 0, "bearer value")) {
        return 0;
    }
    if (!assert_true(extract_bearer_token("bearer\t token", token, sizeof(token)), "extract lowercase")) {
        return 0;
    }
    if (!assert_true(strcmp(token, "token") == 0, "bearer lowercase value")) {
        return 0;
    }
    if (!assert_true(!extract_bearer_token("Basic abc", token, sizeof(token)), "reject non-bearer")) {
        return 0;
    }
    return 1;
}

static int test_get_header_value(void) {
    const char *request =
        "GET /health HTTP/1.1\r\n"
        "Host: example\r\n"
        "Authorization: Bearer token123  \r\n"
        "X-Empty: \r\n"
        "\r\n";
    char value[64];
    if (!assert_true(get_header_value(request, "Authorization", value, sizeof(value)), "header value found")) {
        return 0;
    }
    if (!assert_true(strcmp(value, "Bearer token123") == 0, "header value trimmed")) {
        return 0;
    }
    if (!assert_true(!get_header_value(request, "Missing", value, sizeof(value)), "missing header")) {
        return 0;
    }
    char small[4];
    if (!assert_true(!get_header_value(request, "Authorization", small, sizeof(small)), "header value overflow")) {
        return 0;
    }
    if (!assert_true(get_header_value(request, "X-Empty", value, sizeof(value)), "empty header value")) {
        return 0;
    }
    if (!assert_true(strcmp(value, "") == 0, "empty header value string")) {
        return 0;
    }
    return 1;
}

static int test_parse_state_and_uuid(void) {
    jq_state_t state = JQ_STATE_JOBS;
    if (!assert_true(parse_state("jobs", &state) && state == JQ_STATE_JOBS, "parse jobs")) {
        return 0;
    }
    if (!assert_true(parse_state("priority", &state) && state == JQ_STATE_PRIORITY, "parse priority")) {
        return 0;
    }
    if (!assert_true(parse_state("complete", &state) && state == JQ_STATE_COMPLETE, "parse complete")) {
        return 0;
    }
    if (!assert_true(parse_state("error", &state) && state == JQ_STATE_ERROR, "parse error")) {
        return 0;
    }
    if (!assert_true(!parse_state("unknown", &state), "reject unknown state")) {
        return 0;
    }

    if (!assert_true(is_valid_uuid("job-1_ok") == 200, "valid uuid")) {
        return 0;
    }
    if (!assert_true(!is_valid_uuid("bad uuid"), "reject space")) {
        return 0;
    }
    if (!assert_true(!is_valid_uuid("bad/uuid"), "reject slash")) {
        return 0;
    }
    char long_uuid[HTTP_UUID_SIZE + 1];
    memset(long_uuid, 'a', sizeof(long_uuid));
    long_uuid[sizeof(long_uuid) - 1] = '\0';
    if (!assert_true(!is_valid_uuid(long_uuid), "reject too long uuid")) {
        return 0;
    }
    return 1;
}

static int test_query_param_and_root_paths(void) {
    char output[HTTP_PATH_SIZE];
    if (!assert_true(get_query_param("uuid=test&pdf=file+name.pdf", "pdf", output, sizeof(output)),
                     "get query param")) {
        return 0;
    }
    if (!assert_true(strcmp(output, "file name.pdf") == 0, "query param decoded plus")) {
        return 0;
    }
    if (!assert_true(get_query_param("a=1&b=two%20words", "b", output, sizeof(output)),
                     "get query param encoded")) {
        return 0;
    }
    if (!assert_true(strcmp(output, "two words") == 0, "query param decoded percent")) {
        return 0;
    }
    if (!assert_true(!get_query_param("a=1&b=2", "missing", output, sizeof(output)),
                     "missing query param")) {
        return 0;
    }

    char template[] = "/tmp/pap_http_root_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char root_real[PATH_MAX];
    if (!realpath(root, root_real)) {
        perror("realpath");
        return 0;
    }

    char rooted[PATH_MAX];
    if (!assert_true(build_rooted_path(root_real, "file.pdf", rooted, sizeof(rooted)), "build rooted path")) {
        return 0;
    }
    if (!assert_true(strstr(rooted, "file.pdf") != NULL, "rooted path includes file")) {
        return 0;
    }
    size_t tiny_len = strlen(root_real) + 2;
    char *tiny = malloc(tiny_len);
    if (!tiny) {
        fprintf(stderr, "malloc failed\n");
        return 0;
    }
    int overflow_ok = !build_rooted_path(root_real, "file.pdf", tiny, tiny_len);
    free(tiny);
    if (!assert_true(overflow_ok, "rooted path overflow")) {
        return 0;
    }
    return 1;
}

static int test_resolve_existing_under_root(void) {
    char root_template[] = "/tmp/pap_http_root_real_XXXXXX";
    char *root = mkdtemp(root_template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char root_real[PATH_MAX];
    if (!realpath(root, root_real)) {
        perror("realpath root");
        return 0;
    }

    char file_path[PATH_MAX];
    size_t root_len = strlen(root_real);
    const char *file_suffix = "/file.txt";
    if (root_len + strlen(file_suffix) + 1 > sizeof(file_path)) {
        fprintf(stderr, "root path too long\n");
        return 0;
    }
    memcpy(file_path, root_real, root_len);
    memcpy(file_path + root_len, file_suffix, strlen(file_suffix) + 1);
    FILE *fp = fopen(file_path, "w");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fputs("data", fp);
    fclose(fp);

    char resolved[PATH_MAX];
    int status = 0;
    if (!assert_true(resolve_existing_under_root(root_real, file_path, resolved, sizeof(resolved), &status),
                     "resolve existing under root")) {
        return 0;
    }
    if (!assert_true(status == 200, "resolve existing status")) {
        return 0;
    }
    if (!assert_true(strcmp(resolved, file_path) == 0, "resolved path matches")) {
        return 0;
    }

    char missing_path[PATH_MAX];
    const char *missing_suffix = "/missing.txt";
    if (root_len + strlen(missing_suffix) + 1 > sizeof(missing_path)) {
        fprintf(stderr, "root path too long for missing\n");
        return 0;
    }
    memcpy(missing_path, root_real, root_len);
    memcpy(missing_path + root_len, missing_suffix, strlen(missing_suffix) + 1);
    status = 0;
    if (!assert_true(!resolve_existing_under_root(root_real, missing_path, resolved, sizeof(resolved), &status),
                     "resolve missing under root")) {
        return 0;
    }
    if (!assert_true(status == 404, "missing file status")) {
        return 0;
    }

    char outside_template[] = "/tmp/pap_http_outside_XXXXXX";
    char *outside = mkdtemp(outside_template);
    if (!outside) {
        perror("mkdtemp outside failed");
        return 0;
    }
    char outside_real[PATH_MAX];
    if (!realpath(outside, outside_real)) {
        perror("realpath outside");
        return 0;
    }
    char outside_file[PATH_MAX];
    size_t outside_len = strlen(outside_real);
    const char *outside_suffix = "/outside.txt";
    if (outside_len + strlen(outside_suffix) + 1 > sizeof(outside_file)) {
        fprintf(stderr, "outside path too long\n");
        return 0;
    }
    memcpy(outside_file, outside_real, outside_len);
    memcpy(outside_file + outside_len, outside_suffix, strlen(outside_suffix) + 1);
    fp = fopen(outside_file, "w");
    if (!fp) {
        perror("fopen outside");
        return 0;
    }
    fputs("data", fp);
    fclose(fp);

    status = 0;
    if (!assert_true(!resolve_existing_under_root(root_real, outside_file, resolved, sizeof(resolved), &status),
                     "resolve outside root")) {
        return 0;
    }
    if (!assert_true(status == 403, "outside root status")) {
        return 0;
    }

    return 1;
}

static int test_build_log_path(void) {
    char out[HTTP_PATH_SIZE];
    build_log_path("/submit?uuid=job", out, sizeof(out));
    if (!assert_true(strcmp(out, "/submit") == 0, "log path strips query")) {
        return 0;
    }
    build_log_path("/bad%0Apath", out, sizeof(out));
    if (!assert_true(strcmp(out, "/bad?path") == 0, "log path sanitizes control char")) {
        return 0;
    }
    return 1;
}

int main(void) {
    int passed = 1;
    passed &= test_url_decode();
    passed &= test_parse_request_line();
    passed &= test_is_safe_relpath();
    passed &= test_token_compare();
    passed &= test_extract_bearer();
    passed &= test_get_header_value();
    passed &= test_parse_state_and_uuid();
    passed &= test_query_param_and_root_paths();
    passed &= test_resolve_existing_under_root();
    passed &= test_build_log_path();

    if (!passed) {
        fprintf(stderr, "HTTP helper tests failed.\n");
        return 1;
    }
    printf("HTTP helper tests passed.\n");
    return 0;
}
#endif
