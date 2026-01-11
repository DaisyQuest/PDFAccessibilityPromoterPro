#include "pap/job_queue.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESPONSE_BUFFER 4096
#define COMMAND_BUFFER 16384

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return 0;
    }
    fputs(contents, fp);
    fclose(fp);
    return 1;
}

static int read_command_output(const char *command, char *buffer, size_t buffer_len) {
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        return 0;
    }
    size_t bytes = fread(buffer, 1, buffer_len - 1, pipe);
    buffer[bytes] = '\0';
    int status = pclose(pipe);
    if (status == -1) {
        return 0;
    }
    return 1;
}

static int read_http_status(const char *command, char *buffer, size_t buffer_len) {
    if (!read_command_output(command, buffer, buffer_len)) {
        return 0;
    }
    if (buffer[0] == '\0') {
        return 0;
    }
    return 1;
}

static int wait_for_port_open(int port) {
    char command[COMMAND_BUFFER];
    char output[RESPONSE_BUFFER];
    for (int i = 0; i < 50; ++i) {
        snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/health", port);
        if (read_command_output(command, output, sizeof(output))) {
            if (strstr(output, "ok") != NULL) {
                return 1;
            }
        }
        sleep(1);
    }
    return 0;
}

static int start_server(const char *root, int port, pid_t *pid_out) {
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        char port_arg[16];
        snprintf(port_arg, sizeof(port_arg), "%d", port);
        execl("./job_queue_http", "job_queue_http", root, port_arg, (char *)NULL);
        _exit(1);
    }

    *pid_out = pid;
    return wait_for_port_open(port);
}

static void stop_server(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static int test_http_submit_claim_finalize(void) {
    char template[] = "/tmp/pap_test_http_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9099;
    if (!assert_true(start_server(root, port, &pid), "start server")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=http-job&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_src, metadata_src);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "200") != NULL, "submit status 200")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/claim", port);
    char output[RESPONSE_BUFFER];
    if (!assert_true(read_command_output(command, output, sizeof(output)), "claim request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strncmp(output, "http-job jobs", strlen("http-job jobs")) == 0, "claim output")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/finalize?uuid=http-job&from=jobs&to=complete'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "finalize request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "200") != NULL, "finalize status 200")) {
        stop_server(pid);
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "http-job", JQ_STATE_COMPLETE, pdf_complete, sizeof(pdf_complete),
                                  metadata_complete, sizeof(metadata_complete)) == JQ_OK,
                     "complete paths")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(file_exists(pdf_complete), "pdf complete")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(metadata_complete), "metadata complete")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_release_and_errors(void) {
    char template[] = "/tmp/pap_test_http_release_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "release-job", pdf_src, metadata_src, 0) == JQ_OK, "submit job")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9100;
    if (!assert_true(start_server(root, port, &pid), "start server")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/claim", port);
    char output[RESPONSE_BUFFER];
    if (!assert_true(read_command_output(command, output, sizeof(output)), "claim request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strncmp(output, "release-job jobs", strlen("release-job jobs")) == 0, "claim output")) {
        stop_server(pid);
        return 0;
    }

    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/release?uuid=release-job&state=jobs'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "release request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "200") != NULL, "release status 200")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/release?uuid=release-job&state=bad'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "release bad state")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "400") != NULL, "bad state status 400")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_invalid_method_and_not_found(void) {
    char template[] = "/tmp/pap_test_http_misc_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9101;
    if (!assert_true(start_server(root, port, &pid), "start server")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -X POST -w \"%s\" -o /dev/null http://127.0.0.1:%d/claim",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "post request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "405") != NULL, "method not allowed")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null http://127.0.0.1:%d/unknown",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "unknown request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "404") != NULL, "not found")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_missing_params(void) {
    char template[] = "/tmp/pap_test_http_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9102;
    if (!assert_true(start_server(root, port, &pid), "start server")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null 'http://127.0.0.1:%d/submit?uuid=only'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit missing params")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "400") != NULL, "missing params status")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_submit_missing_file(void) {
    char template[] = "/tmp/pap_test_http_submit_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for submit missing")) {
        return 0;
    }

    char metadata_src[PATH_MAX];
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for missing submit")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9105;
    if (!assert_true(start_server(root, port, &pid), "start server for submit missing")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=missing-file&pdf=%s&metadata=%s'",
             "%{http_code}", port, "/tmp/does-not-exist.pdf", metadata_src);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit missing file")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "404") != NULL, "submit missing file status 404")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_claim_empty(void) {
    char template[] = "/tmp/pap_test_http_empty_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9103;
    if (!assert_true(start_server(root, port, &pid), "start server")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null http://127.0.0.1:%d/claim",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "claim empty")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(status_buffer, "404") != NULL, "claim empty status")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_move_endpoint(void) {
    char template[] = "/tmp/pap_test_http_move_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for move")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for move")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for move")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9106;
    if (!assert_true(start_server(root, port, &pid), "start server for move")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=move-job&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_src, metadata_src);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit move job")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "200") != NULL, "submit move job 200")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/move?uuid=move-job&from=jobs&to=error'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "move request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "200") != NULL, "move status 200")) {
        stop_server(pid);
        return 0;
    }

    char pdf_error[PATH_MAX];
    char metadata_error[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "move-job", JQ_STATE_ERROR, pdf_error, sizeof(pdf_error),
                                  metadata_error, sizeof(metadata_error)) == JQ_OK,
                     "error paths after move")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(pdf_error), "pdf moved to error")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(metadata_error), "metadata moved to error")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_status_missing_uuid(void) {
    char template[] = "/tmp/pap_test_http_status_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for status missing")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9107;
    if (!assert_true(start_server(root, port, &pid), "start server for status missing")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null http://127.0.0.1:%d/status",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "status missing uuid")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "400") != NULL, "status missing uuid 400")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_retrieve_invalid_kind(void) {
    char template[] = "/tmp/pap_test_http_retrieve_invalid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for retrieve invalid")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9108;
    if (!assert_true(start_server(root, port, &pid), "start server for retrieve invalid")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/retrieve?uuid=missing&state=jobs&kind=bad'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "retrieve invalid kind")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "400") != NULL, "retrieve invalid kind 400")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_retrieve_not_found(void) {
    char template[] = "/tmp/pap_test_http_retrieve_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for retrieve missing")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9109;
    if (!assert_true(start_server(root, port, &pid), "start server for retrieve missing")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/retrieve?uuid=missing&state=jobs&kind=pdf'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "retrieve missing job")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "404") != NULL, "retrieve missing job 404")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_move_invalid_state(void) {
    char template[] = "/tmp/pap_test_http_move_invalid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for move invalid")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9110;
    if (!assert_true(start_server(root, port, &pid), "start server for move invalid")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/move?uuid=missing&from=bad&to=jobs'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "move invalid state")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "400") != NULL, "move invalid state 400")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_finalize_missing_job(void) {
    char template[] = "/tmp/pap_test_http_finalize_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for finalize missing")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9111;
    if (!assert_true(start_server(root, port, &pid), "start server for finalize missing")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/finalize?uuid=missing&from=jobs&to=complete'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "finalize missing job")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "404") != NULL, "finalize missing job 404")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_release_missing_job(void) {
    char template[] = "/tmp/pap_test_http_release_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for release missing")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9112;
    if (!assert_true(start_server(root, port, &pid), "start server for release missing")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/release?uuid=missing&state=jobs'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "release missing job")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "404") != NULL, "release missing job 404")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_status_and_retrieve(void) {
    char template[] = "/tmp/pap_test_http_status_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for status")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for status")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for status")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9104;
    if (!assert_true(start_server(root, port, &pid), "start server for status")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[RESPONSE_BUFFER];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=status-job&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_src, metadata_src);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit status job")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "200") != NULL, "submit status job 200")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/status?uuid=status-job", port);
    char output[RESPONSE_BUFFER];
    if (!assert_true(read_command_output(command, output, sizeof(output)), "status unlocked request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "state=jobs locked=0") != NULL, "status unlocked response")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/claim", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "claim for status")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/status?uuid=status-job", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "status locked request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "state=jobs locked=1") != NULL, "status locked response")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/finalize?uuid=status-job&from=jobs&to=complete'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "finalize for status")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "200") != NULL, "finalize status 200")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/status?uuid=status-job", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "status complete request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "state=complete locked=0") != NULL, "status complete response")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s 'http://127.0.0.1:%d/retrieve?uuid=status-job&state=complete&kind=metadata'",
             port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "retrieve metadata")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strncmp(output, "metadata", strlen("metadata")) == 0, "metadata response")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s 'http://127.0.0.1:%d/retrieve?uuid=status-job&state=complete&kind=pdf'",
             port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "retrieve pdf")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strncmp(output, "pdf data", strlen("pdf data")) == 0, "pdf response")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

int main(void) {
    int passed = 1;
    passed &= test_http_submit_claim_finalize();
    passed &= test_http_release_and_errors();
    passed &= test_http_invalid_method_and_not_found();
    passed &= test_http_missing_params();
    passed &= test_http_submit_missing_file();
    passed &= test_http_claim_empty();
    passed &= test_http_move_endpoint();
    passed &= test_http_status_missing_uuid();
    passed &= test_http_retrieve_invalid_kind();
    passed &= test_http_retrieve_not_found();
    passed &= test_http_move_invalid_state();
    passed &= test_http_finalize_missing_job();
    passed &= test_http_release_missing_job();
    passed &= test_http_status_and_retrieve();

    if (!passed) {
        fprintf(stderr, "Some HTTP tests failed.\n");
        return 1;
    }

    printf("All HTTP tests passed.\n");
    return 0;
}
