#include "pap/job_queue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdarg.h>
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
#define HTTP_MAX_BODY_BYTES (10 * 1024 * 1024)
#define HTTP_MAX_HEADER_LINES 50
#define HTTP_REQUEST_LINE_TIMEOUT_MS 2000
#define HTTP_HEADERS_TIMEOUT_MS 5000
#define HTTP_SOCKET_TIMEOUT_MS 1000
#define HTTP_MAX_CHILDREN 32
#define HTTP_METRICS_BUFFER 16384
#define HTTP_PANEL_BUFFER 65536

typedef enum {
    READ_OK = 0,
    READ_TIMEOUT = 1,
    READ_TOO_LARGE = 2,
    READ_TOO_MANY_HEADERS = 3,
    READ_ERROR = 4
} read_result_t;

static int url_decode(const char *src, char *dst, size_t dst_len);
static void trim_token(char *value);

static volatile sig_atomic_t active_children = 0;
static struct timespec server_start_time;
static int server_start_set = 0;

static void record_server_start(void) {
    if (!server_start_set) {
        if (clock_gettime(CLOCK_MONOTONIC, &server_start_time) == 0) {
            server_start_set = 1;
        }
    }
}

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

static int send_response_with_type(int client_fd,
                                   int status,
                                   const char *status_text,
                                   const char *content_type,
                                   const char *body) {
    char header[HTTP_BUFFER_SIZE];
    size_t body_len = body ? strlen(body) : 0;
    int written = snprintf(header, sizeof(header),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n\r\n",
                           status, status_text, content_type, body_len);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        return status;
    }
    write_all(client_fd, header, (size_t)written);
    if (body_len > 0) {
        write_all(client_fd, body, body_len);
    }
    return status;
}

static int send_response(int client_fd, int status, const char *status_text, const char *body) {
    return send_response_with_type(client_fd, status, status_text, "text/plain", body);
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

static int parse_content_length(const char *value, size_t *length_out) {
    if (!value || !length_out || value[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long length = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return 0;
    }
    *length_out = (size_t)length;
    return 1;
}

static int read_request_body(int client_fd,
                             const char *header_buffer,
                             size_t header_len,
                             size_t content_length,
                             char **body_out) {
    if (!header_buffer || !body_out) {
        return 0;
    }
    if (content_length == 0) {
        *body_out = NULL;
        return 1;
    }
    if (content_length > HTTP_MAX_BODY_BYTES) {
        return 0;
    }

    char *body = malloc(content_length + 1);
    if (!body) {
        return 0;
    }

    size_t header_end = 0;
    const char *header_marker = strstr(header_buffer, "\r\n\r\n");
    if (header_marker) {
        header_end = (size_t)(header_marker - header_buffer) + 4;
    }

    size_t available = header_len > header_end ? header_len - header_end : 0;
    size_t to_copy = available > content_length ? content_length : available;
    if (to_copy > 0) {
        memcpy(body, header_buffer + header_end, to_copy);
    }

    size_t total = to_copy;
    while (total < content_length) {
        ssize_t bytes = recv(client_fd, body + total, content_length - total, 0);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(body);
            return 0;
        }
        if (bytes == 0) {
            break;
        }
        total += (size_t)bytes;
    }

    if (total < content_length) {
        free(body);
        return 0;
    }

    body[content_length] = '\0';
    *body_out = body;
    return 1;
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

static long long uptime_seconds(void) {
    struct timespec now;
    if (!server_start_set) {
        return 0;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    long long seconds = now.tv_sec - server_start_time.tv_sec;
    if (seconds < 0) {
        return 0;
    }
    return seconds;
}

static int json_escape(const char *input, char *output, size_t output_len) {
    if (!input || !output || output_len == 0) {
        return 0;
    }
    size_t out = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)input[i];
        const char *escape = NULL;
        char scratch[7];
        if (ch == '\\') {
            escape = "\\\\";
        } else if (ch == '"') {
            escape = "\\\"";
        } else if (ch == '\n') {
            escape = "\\n";
        } else if (ch == '\r') {
            escape = "\\r";
        } else if (ch == '\t') {
            escape = "\\t";
        } else if (ch < 0x20) {
            snprintf(scratch, sizeof(scratch), "\\u%04x", ch);
            escape = scratch;
        }

        if (escape) {
            size_t escape_len = strlen(escape);
            if (out + escape_len + 1 > output_len) {
                return 0;
            }
            memcpy(output + out, escape, escape_len);
            out += escape_len;
            continue;
        }

        if (out + 2 > output_len) {
            return 0;
        }
        output[out++] = (char)ch;
    }
    output[out] = '\0';
    return 1;
}

static const char *find_bytes(const char *haystack,
                              size_t haystack_len,
                              const char *needle,
                              size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static int parse_multipart_boundary(const char *content_type, char *boundary, size_t boundary_len) {
    if (!content_type || !boundary || boundary_len == 0) {
        return 0;
    }
    const char *marker = "boundary=";
    const char *start = strstr(content_type, marker);
    if (!start) {
        return 0;
    }
    start += strlen(marker);
    if (*start == '\0') {
        return 0;
    }
    const char *end = start;
    while (*end != '\0' && *end != ';' && *end != ' ' && *end != '\r' && *end != '\n') {
        end++;
    }
    size_t len = (size_t)(end - start);
    if (len == 0 || len + 1 > boundary_len) {
        return 0;
    }
    memcpy(boundary, start, len);
    boundary[len] = '\0';
    return 1;
}

static int parse_multipart_part(const char *body,
                                size_t body_len,
                                const char *boundary,
                                const char *field_name,
                                const char **data_out,
                                size_t *data_len_out,
                                char *filename_out,
                                size_t filename_len) {
    if (!body || !boundary || !field_name || !data_out || !data_len_out) {
        return 0;
    }
    *data_out = NULL;
    *data_len_out = 0;
    if (filename_out && filename_len > 0) {
        filename_out[0] = '\0';
    }

    char boundary_marker[128];
    int marker_written = snprintf(boundary_marker, sizeof(boundary_marker), "--%s", boundary);
    if (marker_written < 0 || (size_t)marker_written >= sizeof(boundary_marker)) {
        return 0;
    }
    size_t marker_len = (size_t)marker_written;

    const char *cursor = body;
    size_t remaining = body_len;
    while (remaining >= marker_len) {
        const char *part_start = find_bytes(cursor, remaining, boundary_marker, marker_len);
        if (!part_start) {
            return 0;
        }
        part_start += marker_len;
        remaining = body_len - (size_t)(part_start - body);

        if (remaining >= 2 && part_start[0] == '-' && part_start[1] == '-') {
            return 0;
        }
        if (remaining >= 2 && part_start[0] == '\r' && part_start[1] == '\n') {
            part_start += 2;
            remaining -= 2;
        }

        const char *headers_end = find_bytes(part_start, remaining, "\r\n\r\n", 4);
        if (!headers_end) {
            return 0;
        }
        size_t header_len = (size_t)(headers_end - part_start);
        char header_buffer[1024];
        size_t copy_len = header_len < sizeof(header_buffer) - 1 ? header_len : sizeof(header_buffer) - 1;
        memcpy(header_buffer, part_start, copy_len);
        header_buffer[copy_len] = '\0';

        char name_buffer[64];
        name_buffer[0] = '\0';
        char filename_buffer[256];
        filename_buffer[0] = '\0';

        const char *name_pos = strstr(header_buffer, "name=\"");
        if (name_pos) {
            name_pos += strlen("name=\"");
            const char *name_end = strchr(name_pos, '"');
            if (name_end) {
                size_t name_len = (size_t)(name_end - name_pos);
                if (name_len < sizeof(name_buffer)) {
                    memcpy(name_buffer, name_pos, name_len);
                    name_buffer[name_len] = '\0';
                }
            }
        }

        const char *file_pos = strstr(header_buffer, "filename=\"");
        if (file_pos) {
            file_pos += strlen("filename=\"");
            const char *file_end = strchr(file_pos, '"');
            if (file_end) {
                size_t file_len = (size_t)(file_end - file_pos);
                if (file_len < sizeof(filename_buffer)) {
                    memcpy(filename_buffer, file_pos, file_len);
                    filename_buffer[file_len] = '\0';
                }
            }
        }

        const char *data_start = headers_end + 4;
        size_t data_remaining = body_len - (size_t)(data_start - body);
        const char *next_boundary = find_bytes(data_start, data_remaining, boundary_marker, marker_len);
        if (!next_boundary) {
            return 0;
        }
        size_t data_len = (size_t)(next_boundary - data_start);
        if (data_len >= 2 && data_start[data_len - 2] == '\r' && data_start[data_len - 1] == '\n') {
            data_len -= 2;
        }

        if (name_buffer[0] != '\0' && strcmp(name_buffer, field_name) == 0) {
            *data_out = data_start;
            *data_len_out = data_len;
            if (filename_out && filename_len > 0) {
                snprintf(filename_out, filename_len, "%s", filename_buffer);
            }
            return 1;
        }

        cursor = next_boundary;
        remaining = body_len - (size_t)(cursor - body);
    }
    return 0;
}

static int read_multipart_text(const char *body,
                               size_t body_len,
                               const char *boundary,
                               const char *field_name,
                               char *output,
                               size_t output_len) {
    if (!output || output_len == 0) {
        return 0;
    }
    output[0] = '\0';
    const char *data = NULL;
    size_t data_len = 0;
    if (!parse_multipart_part(body, body_len, boundary, field_name, &data, &data_len, NULL, 0)) {
        return 0;
    }
    if (data_len + 1 > output_len) {
        return 0;
    }
    memcpy(output, data, data_len);
    output[data_len] = '\0';
    trim_token(output);
    return 1;
}

static int json_append(char *buffer, size_t buffer_len, size_t *offset, const char *fmt, ...) {
    if (!buffer || !offset || *offset >= buffer_len) {
        return 0;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer + *offset, buffer_len - *offset, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= buffer_len - *offset) {
        return 0;
    }
    *offset += (size_t)written;
    return 1;
}

static int build_panel_html(const char *token, char *output, size_t output_len) {
    if (!output || output_len == 0) {
        return 0;
    }
    const char *safe_token = token ? token : "";
    char escaped_token[HTTP_UUID_SIZE * 2];
    if (!json_escape(safe_token, escaped_token, sizeof(escaped_token))) {
        return 0;
    }
    size_t offset = 0;
    const char *chunks[] = {
        "<!doctype html><html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Job Queue Monitor</title>"
        "<style>",
        ":root{color-scheme:light;background:#0f172a;font-family:'Segoe UI',system-ui,sans-serif;color:#0f172a;}"
        "body{margin:0;background:linear-gradient(135deg,#0f172a,#1e293b);} "
        ".wrap{max-width:1100px;margin:0 auto;padding:32px;}"
        ".hero{display:flex;flex-wrap:wrap;gap:20px;align-items:center;justify-content:space-between;color:#f8fafc;}"
        ".hero h1{margin:0;font-size:32px;letter-spacing:.5px;}"
        ".hero p{margin:6px 0 0;color:#cbd5f5;}",
        ".panel{margin-top:24px;background:#f8fafc;border-radius:18px;padding:24px;box-shadow:0 20px 50px rgba(15,23,42,.35);}"
        ".meta{display:flex;flex-wrap:wrap;gap:16px;align-items:center;justify-content:space-between;}"
        ".meta .status{font-weight:600;color:#1e293b;}"
        ".meta .timestamp{color:#64748b;font-size:14px;}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:16px;margin-top:20px;}"
        ".card{background:white;border-radius:14px;padding:16px;border:1px solid #e2e8f0;}",
        ".card h3{margin:0 0 8px;font-size:16px;color:#0f172a;}"
        ".card .value{font-size:28px;font-weight:700;color:#2563eb;}"
        ".card small{color:#64748b;display:block;margin-top:4px;}"
        ".state-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px;margin-top:16px;}"
        ".state{background:#f1f5f9;border-radius:12px;padding:12px;}"
        ".state h4{margin:0 0 6px;font-size:14px;color:#1e293b;text-transform:uppercase;letter-spacing:.08em;}"
        ".state .row{display:flex;justify-content:space-between;font-size:13px;color:#475569;}",
        ".actions{display:flex;gap:12px;align-items:center;}"
        "button{border:0;background:#2563eb;color:white;padding:10px 16px;border-radius:999px;font-weight:600;cursor:pointer;}"
        "button:disabled{background:#94a3b8;cursor:not-allowed;}"
        "a{color:#2563eb;text-decoration:none;font-weight:600;}"
        ".pill{background:#e2e8f0;color:#1e293b;padding:4px 10px;border-radius:999px;font-size:12px;}"
        ".error{color:#dc2626;font-weight:600;}"
        ".upload-panel{margin-top:24px;background:#f8fafc;border-radius:18px;padding:24px;box-shadow:0 20px 50px rgba(15,23,42,.25);}"
        ".upload-panel h2{margin:0 0 8px;font-size:20px;color:#0f172a;}"
        ".upload-panel p{margin:0 0 16px;color:#64748b;}"
        ".form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:16px;}"
        ".form-field{display:flex;flex-direction:column;gap:6px;font-size:14px;color:#1e293b;}"
        ".form-field input[type=\"text\"],.form-field textarea{border:1px solid #cbd5f5;border-radius:10px;padding:10px;font-size:14px;}"
        ".form-field textarea{min-height:96px;resize:vertical;}"
        ".form-footer{margin-top:16px;display:flex;flex-wrap:wrap;gap:12px;align-items:center;}"
        ".result{margin-top:16px;padding:12px;border-radius:12px;background:#eef2ff;color:#1e293b;font-size:13px;}"
        ".result code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;}"
        "</style></head><body><div class=\"wrap\">"
        "<section class=\"hero\"><div>"
        "<h1>Job Queue Monitor</h1>"
        "<p>Live visibility into queued, locked, and completed jobs.</p>"
        "</div><div class=\"actions\">"
        "<button id=\"refreshBtn\">Refresh now</button>"
        "<a id=\"metricsLink\" href=\"#\" target=\"_blank\" rel=\"noreferrer\">Open metrics JSON</a>"
        "</div></section>",
        "<section class=\"panel\"><div class=\"meta\">"
        "<div class=\"status\">Status: <span id=\"statusText\" class=\"pill\">Loading...</span></div>"
        "<div class=\"timestamp\">Last updated: <span id=\"updatedAt\">--</span></div>"
        "</div><div id=\"errorText\" class=\"error\" aria-live=\"polite\"></div>"
        "<div class=\"grid\">"
        "<div class=\"card\"><h3>Total files</h3><div id=\"totalFiles\" class=\"value\">0</div><small>Across all states</small></div>"
        "<div class=\"card\"><h3>Locked jobs</h3><div id=\"totalLocked\" class=\"value\">0</div><small>Currently locked</small></div>"
        "<div class=\"card\"><h3>Orphaned files</h3><div id=\"totalOrphans\" class=\"value\">0</div><small>Need attention</small></div>",
        "<div class=\"card\"><h3>Stored bytes</h3><div id=\"totalBytes\" class=\"value\">0</div><small>On disk</small></div>"
        "<div class=\"card\"><h3>Uptime</h3><div id=\"uptime\" class=\"value\">0s</div><small>Server runtime</small></div>"
        "<div class=\"card\"><h3>Root path</h3><div id=\"rootPath\" class=\"value\" "
        "style=\"font-size:14px;word-break:break-all;\">--</div><small>Job queue root</small></div>"
        "</div><div class=\"state-grid\">",
        "<div class=\"state\"><h4>Jobs</h4>"
        "<div class=\"row\"><span>PDF</span><span id=\"jobsPdf\">0</span></div>"
        "<div class=\"row\"><span>Metadata</span><span id=\"jobsMetadata\">0</span></div>"
        "<div class=\"row\"><span>Reports</span><span id=\"jobsReport\">0</span></div>"
        "<div class=\"row\"><span>Locked</span><span id=\"jobsLocked\">0</span></div>"
        "<div class=\"row\"><span>Orphans</span><span id=\"jobsOrphans\">0</span></div>"
        "</div>",
        "<div class=\"state\"><h4>Priority</h4>"
        "<div class=\"row\"><span>PDF</span><span id=\"priorityPdf\">0</span></div>"
        "<div class=\"row\"><span>Metadata</span><span id=\"priorityMetadata\">0</span></div>"
        "<div class=\"row\"><span>Reports</span><span id=\"priorityReport\">0</span></div>"
        "<div class=\"row\"><span>Locked</span><span id=\"priorityLocked\">0</span></div>"
        "<div class=\"row\"><span>Orphans</span><span id=\"priorityOrphans\">0</span></div>"
        "</div>",
        "<div class=\"state\"><h4>Complete</h4>"
        "<div class=\"row\"><span>PDF</span><span id=\"completePdf\">0</span></div>"
        "<div class=\"row\"><span>Metadata</span><span id=\"completeMetadata\">0</span></div>"
        "<div class=\"row\"><span>Reports</span><span id=\"completeReport\">0</span></div>"
        "<div class=\"row\"><span>Locked</span><span id=\"completeLocked\">0</span></div>"
        "<div class=\"row\"><span>Orphans</span><span id=\"completeOrphans\">0</span></div>"
        "</div>",
        "<div class=\"state\"><h4>Error</h4>"
        "<div class=\"row\"><span>PDF</span><span id=\"errorPdf\">0</span></div>"
        "<div class=\"row\"><span>Metadata</span><span id=\"errorMetadata\">0</span></div>"
        "<div class=\"row\"><span>Reports</span><span id=\"errorReport\">0</span></div>"
        "<div class=\"row\"><span>Locked</span><span id=\"errorLocked\">0</span></div>"
        "<div class=\"row\"><span>Orphans</span><span id=\"errorOrphans\">0</span></div>"
        "</div></div></section>"
        "<section class=\"upload-panel\">"
        "<h2>Submit OCR &amp; Redaction</h2>"
        "<p>Upload a PDF for OCR and optionally request redaction. Jobs are queued immediately and written under the job root.</p>"
        "<form id=\"uploadForm\">"
        "<div class=\"form-grid\">"
        "<label class=\"form-field\">PDF file"
        "<input id=\"pdfInput\" name=\"pdf\" type=\"file\" accept=\"application/pdf\" required>"
        "</label>"
        "<label class=\"form-field\">Output folder"
        "<input id=\"outputDir\" name=\"output_dir\" type=\"text\" value=\"uploads\">"
        "</label>"
        "<label class=\"form-field\">Job label"
        "<input id=\"labelInput\" name=\"label\" type=\"text\" value=\"ocr\">"
        "</label>"
        "<label class=\"form-field\">Priority"
        "<input id=\"priorityInput\" name=\"priority\" type=\"checkbox\" value=\"1\">"
        "</label>"
        "<label class=\"form-field\">Enable redaction"
        "<input id=\"redactToggle\" name=\"redact\" type=\"checkbox\" value=\"1\">"
        "</label>"
        "<label class=\"form-field\">Redaction terms (comma or newline separated)"
        "<textarea id=\"redactionsInput\" name=\"redactions\" placeholder=\"SECRET&#10;CONFIDENTIAL\"></textarea>"
        "</label>"
        "</div>"
        "<div class=\"form-footer\">"
        "<button id=\"submitBtn\" type=\"submit\">Submit job</button>"
        "<span id=\"submitStatus\" class=\"pill\">Waiting for input</span>"
        "</div>"
        "</form>"
        "<div id=\"resultBox\" class=\"result\" hidden></div>"
        "</section>"
        "</div><script>"
    };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        if (!json_append(output, output_len, &offset, "%s", chunks[i])) {
            return 0;
        }
    }
    if (!json_append(output, output_len, &offset,
                     "const token = \"%s\";"
                     "const tokenQuery = token ? `?token=${encodeURIComponent(token)}` : \"\";"
                     "const metricsUrl = `/metrics${tokenQuery}`;"
                     "const uploadUrl = `/upload${tokenQuery}`;"
                     "const refreshBtn = document.getElementById('refreshBtn');"
                     "const metricsLink = document.getElementById('metricsLink');"
                     "metricsLink.href = metricsUrl;"
                     "const errorText = document.getElementById('errorText');"
                     "const uploadForm = document.getElementById('uploadForm');"
                     "const submitBtn = document.getElementById('submitBtn');"
                     "const submitStatus = document.getElementById('submitStatus');"
                     "const resultBox = document.getElementById('resultBox');"
                     "const redactionToggle = document.getElementById('redactToggle');"
                     "const redactionsInput = document.getElementById('redactionsInput');",
                     escaped_token)) {
        return 0;
    }
    if (!json_append(output, output_len, &offset,
                     "function formatBytes(bytes){"
                     "if(bytes < 1024){return `${bytes} B`;}const units=['KB','MB','GB','TB'];"
                     "let value=bytes;let idx=-1;while(value>=1024 && idx<units.length-1){value/=1024;idx++;}"
                     "return `${value.toFixed(1)} ${units[idx]}`;}"
                     "function setText(id, value){const el=document.getElementById(id);if(el){el.textContent=value;}}"
                     "function setState(prefix, state){"
                     "setText(`${prefix}Pdf`, state.pdf);"
                     "setText(`${prefix}Metadata`, state.metadata);"
                     "setText(`${prefix}Report`, state.report);"
                     "setText(`${prefix}Locked`, state.locked_pdf + state.locked_metadata + state.locked_report);"
                     "setText(`${prefix}Orphans`, state.orphan_pdf + state.orphan_metadata + state.orphan_report);"
                     "}")) {
        return 0;
    }
    if (!json_append(output, output_len, &offset,
                     "function updatePanel(data){"
                     "setText('statusText', data.status || 'unknown');"
                     "setText('updatedAt', new Date().toLocaleString());"
                     "setText('totalFiles', data.totals.files);"
                     "setText('totalLocked', data.totals.locked);"
                     "setText('totalOrphans', data.totals.orphans);"
                     "setText('totalBytes', formatBytes(data.totals.bytes));"
                     "setText('uptime', `${data.uptime_seconds}s`);"
                     "setText('rootPath', data.root);"
                     "setState('jobs', data.states.jobs);"
                     "setState('priority', data.states.priority);"
                     "setState('complete', data.states.complete);"
                     "setState('error', data.states.error);"
                     "}")) {
        return 0;
    }
    if (!json_append(output, output_len, &offset,
                     "function setSubmitStatus(text, isError){"
                     "submitStatus.textContent=text;"
                     "submitStatus.style.background=isError ? '#fee2e2' : '#e2e8f0';"
                     "submitStatus.style.color=isError ? '#991b1b' : '#1e293b';"
                     "}"
                     "function showResult(html){"
                     "resultBox.innerHTML=html;"
                     "resultBox.hidden=false;"
                     "}")) {
        return 0;
    }
    if (!json_append(output, output_len, &offset,
                     "if(redactionToggle){"
                     "redactionsInput.disabled=!redactionToggle.checked;"
                     "redactionToggle.addEventListener('change',()=>{"
                     "redactionsInput.disabled=!redactionToggle.checked;});"
                     "}")) {
        return 0;
    }
    if (!json_append(output, output_len, &offset,
                     "if(uploadForm){"
                     "uploadForm.addEventListener('submit',async (event)=>{"
                     "event.preventDefault();"
                     "if(!uploadForm.reportValidity()){return;}"
                     "const fileInput=document.getElementById('pdfInput');"
                     "if(!fileInput.files.length){"
                     "setSubmitStatus('Please choose a PDF.', true);return;}"
                     "submitBtn.disabled=true;"
                     "setSubmitStatus('Uploading...', false);"
                     "resultBox.hidden=true;"
                     "const formData=new FormData(uploadForm);"
                     "if(!redactionToggle.checked){"
                     "formData.delete('redact');"
                     "formData.delete('redactions');"
                     "}"
                     "if(!document.getElementById('priorityInput').checked){"
                     "formData.delete('priority');"
                     "}"
                     "try{"
                     "const res=await fetch(uploadUrl,{method:'POST',body:formData});"
                     "if(!res.ok){const text=await res.text();"
                     "throw new Error(text || `HTTP ${res.status}`);}"
                     "const data=await res.json();"
                     "let html=`<strong>Queued OCR job:</strong> <code>${data.ocr_uuid}</code><br>`;"
                     "html+=`<strong>Upload folder:</strong> <code>${data.upload_dir}</code><br>`;"
                     "html+=`<strong>Expected OCR output:</strong> <code>${data.expected.ocr.pdf}</code><br>`;"
                     "if(data.expected.redact){"
                     "html+=`<strong>Queued redaction job:</strong> <code>${data.expected.redact.uuid}</code><br>`;"
                     "html+=`<strong>Expected redaction output:</strong> <code>${data.expected.redact.pdf}</code><br>`;"
                     "}"
                     "showResult(html);"
                     "setSubmitStatus('Submitted successfully', false);"
                     "uploadForm.reset();"
                     "redactionsInput.disabled=true;"
                     "}catch(err){"
                     "setSubmitStatus('Upload failed', true);"
                     "showResult(`<strong>Error:</strong> ${err.message}`);"
                     "}finally{submitBtn.disabled=false;}"
                     "});"
                     "}")) {
        return 0;
    }
    if (!json_append(output, output_len, &offset,
                     "async function fetchMetrics(){"
                     "refreshBtn.disabled=true;errorText.textContent='';"
                     "try{const res=await fetch(metricsUrl,{cache:'no-store'});"
                     "if(!res.ok){throw new Error(`HTTP ${res.status}`);}const data=await res.json();"
                     "updatePanel(data);}catch(err){errorText.textContent=`Unable to load metrics: ${err.message}`;}"
                     "finally{refreshBtn.disabled=false;}}"
                     "refreshBtn.addEventListener('click', fetchMetrics);"
                     "fetchMetrics();"
                     "setInterval(fetchMetrics, 5000);"
                     "</script>"
                     "</body>"
                     "</html>")) {
        return 0;
    }
    return 1;
}

static int append_state_metrics(char *buffer,
                                size_t buffer_len,
                                size_t *offset,
                                const char *label,
                                const jq_state_stats_t *stats) {
    return json_append(buffer, buffer_len, offset,
                       "\"%s\":{"
                       "\"pdf\":%zu,"
                       "\"metadata\":%zu,"
                       "\"report\":%zu,"
                       "\"locked_pdf\":%zu,"
                       "\"locked_metadata\":%zu,"
                       "\"locked_report\":%zu,"
                       "\"orphan_pdf\":%zu,"
                       "\"orphan_metadata\":%zu,"
                       "\"orphan_report\":%zu,"
                       "\"bytes_pdf\":%llu,"
                       "\"bytes_metadata\":%llu,"
                       "\"bytes_report\":%llu"
                       "}",
                       label,
                       stats->pdf_jobs,
                       stats->metadata_jobs,
                       stats->report_jobs,
                       stats->pdf_locked,
                       stats->metadata_locked,
                       stats->report_locked,
                       stats->orphan_pdf,
                       stats->orphan_metadata,
                       stats->orphan_report,
                       stats->pdf_bytes,
                       stats->metadata_bytes,
                       stats->report_bytes);
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

static int ensure_directory_recursive(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    char buffer[PATH_MAX];
    if (strlen(path) >= sizeof(buffer)) {
        return 0;
    }
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
                return 0;
            }
            *cursor = '/';
        }
    }
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
        return 0;
    }
    return 1;
}

static int write_file_binary(const char *path, const char *data, size_t length) {
    if (!path || (!data && length > 0)) {
        return 0;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    if (length > 0 && fwrite(data, 1, length, fp) != length) {
        fclose(fp);
        return 0;
    }
    if (fclose(fp) != 0) {
        return 0;
    }
    return 1;
}

static void trim_token(char *value) {
    if (!value) {
        return;
    }
    size_t len = strlen(value);
    size_t start = 0;
    while (start < len && isspace((unsigned char)value[start])) {
        start++;
    }
    size_t end = len;
    while (end > start && isspace((unsigned char)value[end - 1])) {
        end--;
    }
    if (start > 0) {
        memmove(value, value + start, end - start);
    }
    value[end - start] = '\0';
}

static int generate_uuid(const char *label, char *output, size_t output_len) {
    static unsigned int counter = 0;
    if (!output || output_len == 0) {
        return 0;
    }
    const char *prefix = (label && is_valid_uuid(label) == 200) ? label : "upload";
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        now.tv_sec = time(NULL);
        now.tv_nsec = 0;
    }
    counter++;
    int written = snprintf(output, output_len, "%s-%lld-%d-%u",
                           prefix, (long long)now.tv_sec, (int)getpid(), counter);
    if (written < 0 || (size_t)written >= output_len) {
        return 0;
    }
    return is_valid_uuid(output) == 200;
}

static char *build_metadata_json(const char *output_dir,
                                 const char *redactions,
                                 int include_redactions,
                                 size_t *length_out) {
    if (length_out) {
        *length_out = 0;
    }
    const char *safe_output = output_dir ? output_dir : "";
    char escaped_output[PATH_MAX * 2];
    if (!json_escape(safe_output, escaped_output, sizeof(escaped_output))) {
        return NULL;
    }

    size_t buffer_len = 512 + (redactions ? strlen(redactions) * 3 : 0);
    char *buffer = malloc(buffer_len);
    if (!buffer) {
        return NULL;
    }
    size_t offset = 0;
    if (!json_append(buffer, buffer_len, &offset, "{")) {
        free(buffer);
        return NULL;
    }

    if (!json_append(buffer, buffer_len, &offset, "\"output_dir\":\"%s\"", escaped_output)) {
        free(buffer);
        return NULL;
    }

    if (include_redactions) {
        if (!json_append(buffer, buffer_len, &offset, ",\"redactions\":[")) {
            free(buffer);
            return NULL;
        }
        int first = 1;
        if (redactions) {
            char *copy = strdup(redactions);
            if (!copy) {
                free(buffer);
                return NULL;
            }
            char *cursor = copy;
            while (cursor && *cursor != '\0') {
                char *next = strpbrk(cursor, ",\n\r");
                if (next) {
                    *next = '\0';
                }
                trim_token(cursor);
                if (cursor[0] != '\0') {
                    char escaped_token[256];
                    if (!json_escape(cursor, escaped_token, sizeof(escaped_token))) {
                        free(copy);
                        free(buffer);
                        return NULL;
                    }
                    if (!first) {
                        if (!json_append(buffer, buffer_len, &offset, ",")) {
                            free(copy);
                            free(buffer);
                            return NULL;
                        }
                    }
                    if (!json_append(buffer, buffer_len, &offset, "\"%s\"", escaped_token)) {
                        free(copy);
                        free(buffer);
                        return NULL;
                    }
                    first = 0;
                }
                if (!next) {
                    break;
                }
                cursor = next + 1;
            }
            free(copy);
        }
        if (!json_append(buffer, buffer_len, &offset, "]")) {
            free(buffer);
            return NULL;
        }
    }

    if (!json_append(buffer, buffer_len, &offset, "}")) {
        free(buffer);
        return NULL;
    }

    if (length_out) {
        *length_out = offset;
    }
    return buffer;
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

static int handle_upload(const char *root,
                         const char *query,
                         const char *content_type,
                         const char *body,
                         size_t body_len,
                         int client_fd) {
    (void)query;
    if (!root || !content_type || !body) {
        return send_response(client_fd, 400, "Bad Request", "missing upload data\n");
    }

    char boundary[128];
    if (!parse_multipart_boundary(content_type, boundary, sizeof(boundary))) {
        return send_response(client_fd, 400, "Bad Request", "missing boundary\n");
    }

    const char *pdf_data = NULL;
    size_t pdf_len = 0;
    char filename[256];
    if (!parse_multipart_part(body, body_len, boundary, "pdf", &pdf_data, &pdf_len,
                              filename, sizeof(filename))) {
        return send_response(client_fd, 400, "Bad Request", "missing pdf file\n");
    }
    if (pdf_len == 0) {
        return send_response(client_fd, 400, "Bad Request", "empty pdf file\n");
    }

    char output_dir[128] = "uploads";
    char redactions[2048] = "";
    char redact_flag[16] = "";
    char priority_flag[16] = "";
    char label[64] = "";

    (void)read_multipart_text(body, body_len, boundary, "output_dir", output_dir, sizeof(output_dir));
    (void)read_multipart_text(body, body_len, boundary, "redactions", redactions, sizeof(redactions));
    (void)read_multipart_text(body, body_len, boundary, "redact", redact_flag, sizeof(redact_flag));
    (void)read_multipart_text(body, body_len, boundary, "priority", priority_flag, sizeof(priority_flag));
    (void)read_multipart_text(body, body_len, boundary, "label", label, sizeof(label));

    if (!is_safe_relpath(output_dir)) {
        return send_response(client_fd, 400, "Bad Request", "invalid output directory\n");
    }

    int redact_enabled = redact_flag[0] != '\0' && strcmp(redact_flag, "0") != 0 &&
                         strcasecmp(redact_flag, "false") != 0;
    if (redact_enabled && redactions[0] == '\0') {
        return send_response(client_fd, 400, "Bad Request", "redactions required\n");
    }

    char output_full[PATH_MAX];
    if (!build_rooted_path(root, output_dir, output_full, sizeof(output_full))) {
        return send_response(client_fd, 400, "Bad Request", "output path too long\n");
    }
    if (!ensure_directory_recursive(output_full)) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to create output directory\n");
    }

    char ocr_uuid[HTTP_UUID_SIZE];
    if (!generate_uuid(label[0] != '\0' ? label : "ocr", ocr_uuid, sizeof(ocr_uuid))) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to generate uuid\n");
    }

    char pdf_path[PATH_MAX];
    char ocr_metadata_path[PATH_MAX];
    if (snprintf(pdf_path, sizeof(pdf_path), "%s/%s.pdf", output_full, ocr_uuid) < 0 ||
        snprintf(ocr_metadata_path, sizeof(ocr_metadata_path), "%s/%s.metadata.json",
                 output_full, ocr_uuid) < 0) {
        return send_response(client_fd, 500, "Internal Server Error", "path too long\n");
    }

    if (!write_file_binary(pdf_path, pdf_data, pdf_len)) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to write pdf\n");
    }

    size_t ocr_meta_len = 0;
    char *ocr_meta_json = build_metadata_json(output_dir, NULL, 0, &ocr_meta_len);
    if (!ocr_meta_json) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to build metadata\n");
    }
    int ocr_meta_written = write_file_binary(ocr_metadata_path, ocr_meta_json, ocr_meta_len);
    free(ocr_meta_json);
    if (!ocr_meta_written) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to write metadata\n");
    }

    int priority = priority_flag[0] != '\0' && strcmp(priority_flag, "0") != 0 &&
                   strcasecmp(priority_flag, "false") != 0;
    jq_result_t submit_result = jq_submit(root, ocr_uuid, pdf_path, ocr_metadata_path, priority);
    if (submit_result != JQ_OK) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to submit ocr job\n");
    }

    char redact_uuid[HTTP_UUID_SIZE] = "";
    if (redact_enabled) {
        if (!generate_uuid(label[0] != '\0' ? label : "redact", redact_uuid, sizeof(redact_uuid))) {
            return send_response(client_fd, 500, "Internal Server Error", "failed to generate redact uuid\n");
        }
        char redact_metadata_path[PATH_MAX];
        if (snprintf(redact_metadata_path, sizeof(redact_metadata_path), "%s/%s.metadata.json",
                     output_full, redact_uuid) < 0) {
            return send_response(client_fd, 500, "Internal Server Error", "path too long\n");
        }
        size_t redact_meta_len = 0;
        char *redact_meta_json = build_metadata_json(output_dir, redactions, 1, &redact_meta_len);
        if (!redact_meta_json) {
            return send_response(client_fd, 500, "Internal Server Error", "failed to build redact metadata\n");
        }
        int redact_written = write_file_binary(redact_metadata_path, redact_meta_json, redact_meta_len);
        free(redact_meta_json);
        if (!redact_written) {
            return send_response(client_fd, 500, "Internal Server Error", "failed to write redact metadata\n");
        }
        jq_result_t redact_submit = jq_submit(root, redact_uuid, pdf_path, redact_metadata_path, priority);
        if (redact_submit != JQ_OK) {
            return send_response(client_fd, 500, "Internal Server Error", "failed to submit redact job\n");
        }
    }

    char escaped_output[PATH_MAX * 2];
    char escaped_filename[256 * 2];
    if (!json_escape(output_dir, escaped_output, sizeof(escaped_output)) ||
        !json_escape(filename, escaped_filename, sizeof(escaped_filename))) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to encode response\n");
    }

    char body_buffer[HTTP_BUFFER_SIZE * 4];
    size_t offset = 0;
    if (!json_append(body_buffer, sizeof(body_buffer), &offset,
                     "{"
                     "\"status\":\"ok\","
                     "\"ocr_uuid\":\"%s\","
                     "\"upload_dir\":\"%s\","
                     "\"filename\":\"%s\","
                     "\"expected\":{"
                     "\"ocr\":{"
                     "\"metadata\":\"complete/%s.metadata.job\","
                     "\"pdf\":\"complete/%s.pdf.job\""
                     "}",
                     ocr_uuid, escaped_output, escaped_filename, ocr_uuid, ocr_uuid)) {
        return send_response(client_fd, 500, "Internal Server Error", "response too large\n");
    }
    if (redact_enabled) {
        if (!json_append(body_buffer, sizeof(body_buffer), &offset,
                         ",\"redact\":{"
                         "\"uuid\":\"%s\","
                         "\"metadata\":\"complete/%s.metadata.job\","
                         "\"pdf\":\"complete/%s.pdf.job\""
                         "}",
                         redact_uuid, redact_uuid, redact_uuid)) {
            return send_response(client_fd, 500, "Internal Server Error", "response too large\n");
        }
    }
    if (!json_append(body_buffer, sizeof(body_buffer), &offset, "}}")) {
        return send_response(client_fd, 500, "Internal Server Error", "response too large\n");
    }

    return send_response_with_type(client_fd, 200, "OK", "application/json", body_buffer);
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
    int send_report = strcmp(kind_value, "report") == 0;
    if (!send_pdf && !send_metadata && !send_report) {
        return send_response(client_fd, 400, "Bad Request", "invalid kind\n");
    }

    char pdf_path[HTTP_PATH_SIZE];
    char metadata_path[HTTP_PATH_SIZE];
    char report_path[HTTP_PATH_SIZE];
    jq_result_t path_result =
        jq_job_paths(root, uuid, state, pdf_path, sizeof(pdf_path), metadata_path, sizeof(metadata_path));
    if (path_result != JQ_OK) {
        return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
    }

    if (send_report) {
        jq_result_t report_result =
            jq_job_report_paths(root, uuid, state, report_path, sizeof(report_path));
        if (report_result != JQ_OK) {
            return send_response(client_fd, 400, "Bad Request", "invalid arguments\n");
        }
    }

    const char *path = send_pdf ? pdf_path : (send_metadata ? metadata_path : report_path);
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

    const char *content_type = send_pdf ? "application/pdf" :
                               (send_metadata ? "application/json" : "text/html");
    send_result_t send_result = send_file(client_fd, content_type, resolved);
    if (send_result == SEND_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job not found\n");
    }
    if (send_result != SEND_OK) {
        return send_response(client_fd, 500, "Internal Server Error", "io error\n");
    }

    return 200;
}

static int handle_metrics(const char *root, int client_fd) {
    jq_stats_t stats;
    jq_result_t result = jq_collect_stats(root, &stats);
    if (result == JQ_ERR_NOT_FOUND) {
        return send_response(client_fd, 404, "Not Found", "job root not found\n");
    }
    if (result != JQ_OK) {
        return send_response(client_fd, 500, "Internal Server Error", "unable to read stats\n");
    }

    record_server_start();
    char escaped_root[PATH_MAX * 2];
    if (!json_escape(root, escaped_root, sizeof(escaped_root))) {
        return send_response(client_fd, 500, "Internal Server Error", "failed to encode root\n");
    }

    char body[HTTP_METRICS_BUFFER];
    size_t offset = 0;
    time_t now = time(NULL);

    if (!json_append(body, sizeof(body), &offset,
                     "{"
                     "\"status\":\"ok\","
                     "\"timestamp_epoch\":%lld,"
                     "\"uptime_seconds\":%lld,"
                     "\"root\":\"%s\","
                     "\"limits\":{"
                     "\"max_children\":%d,"
                     "\"max_body_bytes\":%d,"
                     "\"max_request_bytes\":%d,"
                     "\"max_header_lines\":%d,"
                     "\"request_line_timeout_ms\":%d,"
                     "\"headers_timeout_ms\":%d,"
                     "\"socket_timeout_ms\":%d"
                     "},"
                     "\"totals\":{"
                     "\"files\":%zu,"
                     "\"locked\":%zu,"
                     "\"orphans\":%zu,"
                     "\"bytes\":%llu,"
                     "\"oldest_mtime\":%lld,"
                     "\"newest_mtime\":%lld"
                     "},"
                     "\"states\":{",
                     (long long)now,
                     uptime_seconds(),
                     escaped_root,
                     HTTP_MAX_CHILDREN,
                     HTTP_MAX_BODY_BYTES,
                     HTTP_MAX_REQUEST_BYTES,
                     HTTP_MAX_HEADER_LINES,
                     HTTP_REQUEST_LINE_TIMEOUT_MS,
                     HTTP_HEADERS_TIMEOUT_MS,
                     HTTP_SOCKET_TIMEOUT_MS,
                     stats.total_jobs,
                     stats.total_locked,
                     stats.total_orphans,
                     stats.total_bytes,
                     (long long)stats.oldest_mtime,
                     (long long)stats.newest_mtime)) {
        return send_response(client_fd, 500, "Internal Server Error", "metrics too large\n");
    }

    if (!append_state_metrics(body, sizeof(body), &offset, "jobs", &stats.states[JQ_STATE_JOBS]) ||
        !json_append(body, sizeof(body), &offset, ",") ||
        !append_state_metrics(body, sizeof(body), &offset, "priority", &stats.states[JQ_STATE_PRIORITY]) ||
        !json_append(body, sizeof(body), &offset, ",") ||
        !append_state_metrics(body, sizeof(body), &offset, "complete", &stats.states[JQ_STATE_COMPLETE]) ||
        !json_append(body, sizeof(body), &offset, ",") ||
        !append_state_metrics(body, sizeof(body), &offset, "error", &stats.states[JQ_STATE_ERROR]) ||
        !json_append(body, sizeof(body), &offset, "}}")) {
        return send_response(client_fd, 500, "Internal Server Error", "metrics too large\n");
    }

    return send_response_with_type(client_fd, 200, "OK", "application/json", body);
}

static int handle_panel(const char *query, int client_fd) {
    char token_value[HTTP_UUID_SIZE];
    const char *token = "";
    if (get_query_param(query, "token", token_value, sizeof(token_value))) {
        token = token_value;
    }

    char body[HTTP_PANEL_BUFFER];
    if (!build_panel_html(token, body, sizeof(body))) {
        return send_response(client_fd, 500, "Internal Server Error", "panel too large\n");
    }
    return send_response_with_type(client_fd, 200, "OK", "text/html; charset=utf-8", body);
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
                         const char *content_type,
                         const char *body,
                         size_t body_len,
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

    char content_type[128];
    const char *content_type_value = NULL;
    if (get_header_value(buffer, "Content-Type", content_type, sizeof(content_type))) {
        content_type_value = content_type;
    }

    char content_length_value[32];
    size_t content_length = 0;
    char *body = NULL;
    int needs_body = strncmp(path, "/upload", 7) == 0;
    if (strcasecmp(method, "POST") == 0 && needs_body) {
        if (!get_header_value(buffer, "Content-Length", content_length_value, sizeof(content_length_value)) ||
            !parse_content_length(content_length_value, &content_length)) {
            status = send_response(client_fd, 411, "Length Required", "missing content length\n");
            goto log_request;
        }
        if (!read_request_body(client_fd, buffer, bytes, content_length, &body)) {
            status = send_response(client_fd, 413, "Payload Too Large", "body too large\n");
            goto log_request;
        }
    }

    status = route_request(root_real,
                           method,
                           path,
                           auth_value,
                           token_config,
                           content_type_value,
                           body,
                           content_length,
                           client_fd);
    free(body);

log_request:
    ;
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
                         const char *content_type,
                         const char *body,
                         size_t body_len,
                         int client_fd) {
    int is_get = strcmp(method, "GET") == 0;
    int is_post = strcmp(method, "POST") == 0;
    if (!is_get && !is_post) {
        return send_response(client_fd, 405, "Method Not Allowed", "unsupported method\n");
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

    if (strcmp(decoded_path, "/metrics") == 0) {
        return handle_metrics(root, client_fd);
    }

    if (strcmp(decoded_path, "/") == 0 || strcmp(decoded_path, "/panel") == 0) {
        return handle_panel(query, client_fd);
    }

    if (strcmp(decoded_path, "/submit") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
        return handle_submit(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/upload") == 0) {
        if (!is_post) {
            return send_response(client_fd, 405, "Method Not Allowed", "only POST supported\n");
        }
        return handle_upload(root, query, content_type, body, body_len, client_fd);
    }

    if (strcmp(decoded_path, "/claim") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
        return handle_claim(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/release") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
        return handle_release(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/finalize") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
        return handle_finalize(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/move") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
        return handle_move(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/status") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
        return handle_status(root, query, client_fd);
    }

    if (strcmp(decoded_path, "/retrieve") == 0) {
        if (!is_get) {
            return send_response(client_fd, 405, "Method Not Allowed", "only GET supported\n");
        }
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

    record_server_start();

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

static int test_json_escape(void) {
    char escaped[64];
    if (!assert_true(json_escape("path\"\\\n", escaped, sizeof(escaped)), "json escape special chars")) {
        return 0;
    }
    if (!assert_true(strcmp(escaped, "path\\\"\\\\\\n") == 0, "json escaped output")) {
        return 0;
    }
    char control_input[3];
    control_input[0] = 'a';
    control_input[1] = 1;
    control_input[2] = '\0';
    if (!assert_true(json_escape(control_input, escaped, sizeof(escaped)), "json escape control")) {
        return 0;
    }
    if (!assert_true(strstr(escaped, "\\u0001") != NULL, "json escape control value")) {
        return 0;
    }
    return 1;
}

static int test_build_panel_html(void) {
    char html[HTTP_PANEL_BUFFER];
    if (!assert_true(build_panel_html(NULL, html, sizeof(html)), "build panel html without token")) {
        return 0;
    }
    if (!assert_true(strstr(html, "Job Queue Monitor") != NULL, "panel title present")) {
        return 0;
    }
    if (!assert_true(strstr(html, "/metrics") != NULL, "panel metrics link present")) {
        return 0;
    }
    if (!assert_true(strstr(html, "Submit OCR") != NULL, "panel upload form present")) {
        return 0;
    }
    if (!assert_true(build_panel_html("tok\"en", html, sizeof(html)), "build panel html with token")) {
        return 0;
    }
    if (!assert_true(strstr(html, "tok\\\"en") != NULL, "token escaped in html")) {
        return 0;
    }
    return 1;
}

static int test_multipart_parsing(void) {
    const char *content_type = "multipart/form-data; boundary=bound";
    char boundary[32];
    if (!assert_true(parse_multipart_boundary(content_type, boundary, sizeof(boundary)),
                     "parse multipart boundary")) {
        return 0;
    }
    if (!assert_true(strcmp(boundary, "bound") == 0, "boundary parsed")) {
        return 0;
    }
    const char *body =
        "--bound\r\n"
        "Content-Disposition: form-data; name=\"output_dir\"\r\n\r\n"
        "uploads\r\n"
        "--bound\r\n"
        "Content-Disposition: form-data; name=\"pdf\"; filename=\"sample.pdf\"\r\n\r\n"
        "PDFDATA\r\n"
        "--bound--\r\n";
    const char *data = NULL;
    size_t data_len = 0;
    char filename[64];
    if (!assert_true(parse_multipart_part(body, strlen(body), boundary, "pdf", &data, &data_len,
                                          filename, sizeof(filename)),
                     "parse multipart pdf")) {
        return 0;
    }
    if (!assert_true(data_len == strlen("PDFDATA"), "pdf data length")) {
        return 0;
    }
    if (!assert_true(strncmp(data, "PDFDATA", data_len) == 0, "pdf data content")) {
        return 0;
    }
    if (!assert_true(strcmp(filename, "sample.pdf") == 0, "pdf filename parsed")) {
        return 0;
    }
    char output_dir[32];
    if (!assert_true(read_multipart_text(body, strlen(body), boundary, "output_dir",
                                         output_dir, sizeof(output_dir)),
                     "read multipart output dir")) {
        return 0;
    }
    if (!assert_true(strcmp(output_dir, "uploads") == 0, "output dir parsed")) {
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
    passed &= test_json_escape();
    passed &= test_build_panel_html();
    passed &= test_multipart_parsing();

    if (!passed) {
        fprintf(stderr, "HTTP helper tests failed.\n");
        return 1;
    }
    printf("HTTP helper tests passed.\n");
    return 0;
}
#endif
