#include "pap/job_queue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HTTP_BUFFER_SIZE 4096
#define HTTP_PATH_SIZE 512
#define HTTP_UUID_SIZE 128

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
    return 1;
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
        return 0;
    }
    return write_all(client_fd, response, (size_t)written);
}

static int parse_first_line(const char *request, char *method, size_t method_len, char *path, size_t path_len) {
    if (!request || !method || !path) {
        return 0;
    }

    const char *space = strchr(request, ' ');
    if (!space) {
        return 0;
    }
    size_t method_size = (size_t)(space - request);
    if (method_size + 1 > method_len) {
        return 0;
    }
    memcpy(method, request, method_size);
    method[method_size] = '\0';

    const char *path_start = space + 1;
    const char *path_end = strchr(path_start, ' ');
    if (!path_end) {
        return 0;
    }
    size_t path_size = (size_t)(path_end - path_start);
    if (path_size + 1 > path_len) {
        return 0;
    }
    memcpy(path, path_start, path_size);
    path[path_size] = '\0';

    return 1;
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

static int get_query_param(const char *query, const char *key, char *out, size_t out_len) {
    if (!query || !key || !out) {
        return 0;
    }

    size_t key_len = strlen(key);
    const char *cursor = query;
    while (*cursor) {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            size_t value_len = value_end ? (size_t)(value_end - value_start) : strlen(value_start);
            if (value_len + 1 > out_len) {
                return 0;
            }
            memcpy(out, value_start, value_len);
            out[value_len] = '\0';
            return 1;
        }
        const char *next = strchr(cursor, '&');
        if (!next) {
            break;
        }
        cursor = next + 1;
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

    int priority = 0;
    if (get_query_param(query, "priority", priority_value, sizeof(priority_value))) {
        priority = strcmp(priority_value, "1") == 0;
    }

    jq_result_t result = jq_submit(root, uuid, pdf_path, metadata_path, priority);
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

static int route_request(const char *root, const char *method, const char *path, int client_fd) {
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

    if (strcmp(path_buffer, "/health") == 0) {
        return send_response(client_fd, 200, "OK", "ok\n");
    }

    if (strcmp(path_buffer, "/submit") == 0) {
        return handle_submit(root, query, client_fd);
    }

    if (strcmp(path_buffer, "/claim") == 0) {
        return handle_claim(root, query, client_fd);
    }

    if (strcmp(path_buffer, "/release") == 0) {
        return handle_release(root, query, client_fd);
    }

    if (strcmp(path_buffer, "/finalize") == 0) {
        return handle_finalize(root, query, client_fd);
    }

    if (strcmp(path_buffer, "/move") == 0) {
        return handle_move(root, query, client_fd);
    }

    return send_response(client_fd, 404, "Not Found", "unknown endpoint\n");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: job_queue_http <root> <port>\n");
        return 1;
    }

    const char *root = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 1;
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
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

    printf("listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        char buffer[HTTP_BUFFER_SIZE];
        ssize_t bytes = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes <= 0) {
            close(client_fd);
            continue;
        }
        buffer[bytes] = '\0';

        char method[16];
        char path[HTTP_PATH_SIZE];
        if (!parse_first_line(buffer, method, sizeof(method), path, sizeof(path))) {
            send_response(client_fd, 400, "Bad Request", "invalid request\n");
            close(client_fd);
            continue;
        }

        route_request(root, method, path, client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 1;
}
