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

#define RESPONSE_BUFFER 65536
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

static int start_server(const char *root, int port, const char *token, pid_t *pid_out) {
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        char port_arg[16];
        snprintf(port_arg, sizeof(port_arg), "%d", port);
        const char *args[8];
        int idx = 0;
        args[idx++] = "./job_queue_http";
        args[idx++] = root;
        args[idx++] = port_arg;
        if (token) {
            args[idx++] = "--token";
            args[idx++] = token;
        }
        args[idx] = NULL;
        execv("./job_queue_http", (char *const *)args);
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

static int extract_json_value(const char *json, const char *key, char *output, size_t output_len) {
    if (!json || !key || !output || output_len == 0) {
        return 0;
    }
    char needle[128];
    int written = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (written < 0 || (size_t)written >= sizeof(needle)) {
        return 0;
    }
    const char *start = strstr(json, needle);
    if (!start) {
        return 0;
    }
    start += strlen(needle);
    const char *end = strchr(start, '"');
    if (!end) {
        return 0;
    }
    size_t len = (size_t)(end - start);
    if (len + 1 > output_len) {
        return 0;
    }
    memcpy(output, start, len);
    output[len] = '\0';
    return 1;
}

static int extract_json_value_after(const char *json,
                                    const char *marker,
                                    const char *key,
                                    char *output,
                                    size_t output_len) {
    const char *start = strstr(json, marker);
    if (!start) {
        return 0;
    }
    return extract_json_value(start, key, output, output_len);
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

    const char *pdf_rel = "source.pdf";
    const char *metadata_rel = "source.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9099;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=http-job&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_rel, metadata_rel);
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

static int test_http_retrieve_report(void) {
    char template[] = "/tmp/pap_test_http_report_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for report")) {
        return 0;
    }

    char report_path[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "report-job", JQ_STATE_COMPLETE,
                                         report_path, sizeof(report_path)) == JQ_OK,
                     "report path")) {
        return 0;
    }
    if (!assert_true(write_file(report_path, "<html>report</html>"), "write report file")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9106;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for report")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char output[RESPONSE_BUFFER];
    snprintf(command, sizeof(command),
             "curl -s 'http://127.0.0.1:%d/retrieve?uuid=report-job&state=complete&kind=report'",
             port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "retrieve report request")) {
        stop_server(pid);
        return 0;
    }

    if (!assert_true(strstr(output, "report</html>") != NULL, "report response contains html")) {
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

    const char *pdf_rel = "source.pdf";
    const char *metadata_rel = "source.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server")) {
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

    const char *metadata_rel = "source.metadata";
    char metadata_src[PATH_MAX];
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for missing submit")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9105;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for submit missing")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=missing-file&pdf=%s&metadata=%s'",
             "%{http_code}", port, "missing.pdf", metadata_rel);
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

static int test_http_upload_submit(void) {
    char template[] = "/tmp/pap_test_http_upload_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for upload")) {
        return 0;
    }

    char pdf_path[PATH_MAX];
    snprintf(pdf_path, sizeof(pdf_path), "%s/source.pdf", root);
    const char *pdf_contents = "%PDF-1.7\n1 0 obj\n<<>>\nendobj\n";
    if (!assert_true(write_file(pdf_path, pdf_contents), "write upload pdf")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9120;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for upload")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char response[RESPONSE_BUFFER];
    snprintf(command, sizeof(command),
             "curl -s -X POST "
             "-F \"pdf=@%s\" "
             "-F \"output_dir=uploads/ui\" "
             "-F \"label=upload\" "
             "-F \"priority=1\" "
             "-F \"redact=1\" "
             "-F \"redactions=SECRET\" "
             "http://127.0.0.1:%d/upload",
             pdf_path, port);

    if (!assert_true(read_command_output(command, response, sizeof(response)), "upload request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(response, "\"status\":\"ok\"") != NULL, "upload status ok")) {
        stop_server(pid);
        return 0;
    }

    char ocr_uuid[128];
    if (!assert_true(extract_json_value(response, "ocr_uuid", ocr_uuid, sizeof(ocr_uuid)),
                     "extract ocr uuid")) {
        stop_server(pid);
        return 0;
    }

    char redact_uuid[128];
    if (!assert_true(extract_json_value_after(response, "\"redact\":", "uuid", redact_uuid, sizeof(redact_uuid)),
                     "extract redact uuid")) {
        stop_server(pid);
        return 0;
    }

    char upload_dir[256];
    snprintf(upload_dir, sizeof(upload_dir), "%s/uploads/ui", root);
    char pdf_uploaded[512];
    char ocr_metadata[512];
    snprintf(pdf_uploaded, sizeof(pdf_uploaded), "%s/%s.pdf", upload_dir, ocr_uuid);
    snprintf(ocr_metadata, sizeof(ocr_metadata), "%s/%s.metadata.json", upload_dir, ocr_uuid);
    if (!assert_true(file_exists(pdf_uploaded), "uploaded pdf exists")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(ocr_metadata), "ocr metadata exists")) {
        stop_server(pid);
        return 0;
    }

    char redact_metadata[512];
    snprintf(redact_metadata, sizeof(redact_metadata), "%s/%s.metadata.json", upload_dir, redact_uuid);
    if (!assert_true(file_exists(redact_metadata), "redact metadata exists")) {
        stop_server(pid);
        return 0;
    }

    char pdf_job[PATH_MAX];
    char metadata_job[PATH_MAX];
    if (!assert_true(jq_job_paths(root, ocr_uuid, JQ_STATE_PRIORITY,
                                  pdf_job, sizeof(pdf_job),
                                  metadata_job, sizeof(metadata_job)) == JQ_OK,
                     "ocr job paths priority")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(pdf_job), "ocr job pdf queued")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(metadata_job), "ocr job metadata queued")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server")) {
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

    const char *pdf_rel = "source.pdf";
    const char *metadata_rel = "source.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for move")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for move")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9106;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for move")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=move-job&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_rel, metadata_rel);
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for status missing")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for retrieve invalid")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for retrieve missing")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for move invalid")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for finalize missing")) {
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
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for release missing")) {
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

    const char *pdf_rel = "source.pdf";
    const char *metadata_rel = "source.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for status")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for status")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9104;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for status")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[RESPONSE_BUFFER];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=status-job&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_rel, metadata_rel);
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

static int test_http_auth_token(void) {
    char template[] = "/tmp/pap_test_http_auth_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for auth")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9113;
    const char *token = "secret-token";
    if (!assert_true(start_server(root, port, token, &pid), "start server for auth")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null http://127.0.0.1:%d/health",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "health without token")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "200") != NULL, "health no token 200")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null http://127.0.0.1:%d/claim",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "claim without token")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "401") != NULL, "claim without token 401")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null 'http://127.0.0.1:%d/claim?token=%s'",
             "%{http_code}", port, token);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "claim with token")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "404") != NULL, "claim with token 404")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_url_decoding(void) {
    char template[] = "/tmp/pap_test_http_decode_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for decode")) {
        return 0;
    }

    const char *pdf_rel = "spaced file.pdf";
    const char *metadata_rel = "spaced file.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for decode")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for decode")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9114;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for decode")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=decode-job&pdf=spaced+file.pdf&metadata=spaced+file.metadata'",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit decode")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "200") != NULL, "submit decode 200")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_invalid_uuid_and_path(void) {
    char template[] = "/tmp/pap_test_http_invalid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for invalid")) {
        return 0;
    }

    const char *pdf_rel = "source.pdf";
    const char *metadata_rel = "source.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for invalid")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for invalid")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9115;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for invalid")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=bad%%20uuid&pdf=%s&metadata=%s'",
             "%{http_code}", port, pdf_rel, metadata_rel);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit invalid uuid")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "400") != NULL, "invalid uuid 400")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null "
             "'http://127.0.0.1:%d/submit?uuid=ok-uuid&pdf=../escape.pdf&metadata=%s'",
             "%{http_code}", port, metadata_rel);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "submit invalid path")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "400") != NULL, "invalid path 400")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_metrics(void) {
    char template[] = "/tmp/pap_test_http_metrics_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for metrics")) {
        return 0;
    }

    const char *pdf_rel = "source.pdf";
    const char *metadata_rel = "source.metadata";
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s", root, pdf_rel);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s", root, metadata_rel);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf for metrics")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata for metrics")) {
        return 0;
    }
    if (!assert_true(jq_submit(root, "metrics-job", pdf_src, metadata_src, 0) == JQ_OK,
                     "submit metrics job")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9116;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for metrics")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char output[RESPONSE_BUFFER];
    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/metrics", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "metrics request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "\"status\":\"ok\"") != NULL, "metrics status ok")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "\"states\"") != NULL, "metrics states present")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "\"totals\"") != NULL, "metrics totals present")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_panel_page(void) {
    char template[] = "/tmp/pap_test_http_panel_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for panel")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9117;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for panel")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char output[RESPONSE_BUFFER];
    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/panel", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "panel request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "Job Queue Monitor") != NULL, "panel page title")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "/metrics") != NULL, "panel metrics link")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "Submit OCR") != NULL, "panel upload form")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "Run worker jobs") != NULL, "panel worker section")) {
        stop_server(pid);
        return 0;
    }

    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "root panel request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "Job Queue Monitor") != NULL, "root panel title")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "Submit OCR") != NULL, "root panel upload form")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "Run worker jobs") != NULL, "root panel worker section")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_run_worker(void) {
    char template[] = "/tmp/pap_test_http_run_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for run worker")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    const char *contents = "%PDF-1.6\n1 0 obj\n<<>>\nendobj\n";
    if (!assert_true(write_file(pdf_src, contents), "write run worker pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "{}"), "write run worker metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "run-ocr", pdf_src, metadata_src, 0) == JQ_OK,
                     "submit run worker job")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9116;
    if (!assert_true(start_server(root, port, NULL, &pid), "start server for run worker")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char output[RESPONSE_BUFFER];
    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/run?job=ocr", port);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "run worker request")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "\"status\":\"ok\"") != NULL, "run worker status ok")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, "\"job\":\"ocr\"") != NULL, "run worker job field")) {
        stop_server(pid);
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "run-ocr", JQ_STATE_COMPLETE,
                                  pdf_complete, sizeof(pdf_complete),
                                  metadata_complete, sizeof(metadata_complete)) == JQ_OK,
                     "run worker complete paths")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(pdf_complete), "run worker pdf complete")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(file_exists(metadata_complete), "run worker metadata complete")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

static int test_http_panel_auth(void) {
    char template[] = "/tmp/pap_test_http_panel_auth_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init root for panel auth")) {
        return 0;
    }

    pid_t pid = 0;
    int port = 9118;
    const char *token = "panel-token";
    if (!assert_true(start_server(root, port, token, &pid), "start server for panel auth")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    char status_buffer[64];
    snprintf(command, sizeof(command),
             "curl -s -w \"%s\" -o /dev/null http://127.0.0.1:%d/panel",
             "%{http_code}", port);
    if (!assert_true(read_http_status(command, status_buffer, sizeof(status_buffer)), "panel no token")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(status_buffer, "401") != NULL, "panel unauthorized 401")) {
        stop_server(pid);
        return 0;
    }

    char output[RESPONSE_BUFFER];
    snprintf(command, sizeof(command), "curl -s http://127.0.0.1:%d/panel?token=%s", port, token);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "panel with token")) {
        stop_server(pid);
        return 0;
    }
    if (!assert_true(strstr(output, token) != NULL, "panel includes token")) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

int main(void) {
    int passed = 1;
    passed &= test_http_submit_claim_finalize();
    passed &= test_http_retrieve_report();
    passed &= test_http_release_and_errors();
    passed &= test_http_invalid_method_and_not_found();
    passed &= test_http_missing_params();
    passed &= test_http_submit_missing_file();
    passed &= test_http_upload_submit();
    passed &= test_http_claim_empty();
    passed &= test_http_move_endpoint();
    passed &= test_http_status_missing_uuid();
    passed &= test_http_retrieve_invalid_kind();
    passed &= test_http_retrieve_not_found();
    passed &= test_http_move_invalid_state();
    passed &= test_http_finalize_missing_job();
    passed &= test_http_release_missing_job();
    passed &= test_http_status_and_retrieve();
    passed &= test_http_auth_token();
    passed &= test_http_url_decoding();
    passed &= test_http_invalid_uuid_and_path();
    passed &= test_http_metrics();
    passed &= test_http_run_worker();
    passed &= test_http_panel_page();
    passed &= test_http_panel_auth();

    if (!passed) {
        fprintf(stderr, "Some HTTP tests failed.\n");
        return 1;
    }

    printf("All HTTP tests passed.\n");
    return 0;
}
