#include "pap/job_queue.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMMAND_BUFFER 16384

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        return 0;
    }
    return 1;
}

static int write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    fputs(contents, fp);
    fclose(fp);
    return 1;
}

static int read_file(const char *path, char *buffer, size_t buffer_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    size_t bytes = fread(buffer, 1, buffer_len - 1, fp);
    buffer[bytes] = '\0';
    fclose(fp);
    return 1;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int run_command(const char *command) {
    int result = system(command);
    if (result == -1) {
        return -1;
    }
    if (WIFEXITED(result)) {
        return WEXITSTATUS(result);
    }
    return -1;
}

static int test_redact_success(void) {
    char template[] = "/tmp/pap_test_redact_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init redact root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    const char *contents = "%PDF-1.7\nSECRET DATA";
    if (!assert_true(write_file(pdf_src, contents), "write redact pdf")) {
        return 0;
    }
    const char *plan = "{\"redactions\":[\"SECRET\"]}";
    if (!assert_true(write_file(metadata_src, plan), "write metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "redact-job", pdf_src, metadata_src, 0) == JQ_OK, "submit redact job")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_redact %s", root);
    if (!assert_true(run_command(command) == 0, "redact command success")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "redact-job", JQ_STATE_COMPLETE,
                                  pdf_complete, sizeof(pdf_complete),
                                  metadata_complete, sizeof(metadata_complete)) == JQ_OK,
                     "complete paths")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_complete), "complete pdf exists")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_complete), "complete metadata exists")) {
        return 0;
    }

    char pdf_buffer[256];
    if (!assert_true(read_file(pdf_complete, pdf_buffer, sizeof(pdf_buffer)), "read pdf")) {
        return 0;
    }
    if (!assert_true(strstr(pdf_buffer, "XXXXXX") != NULL, "pdf redacted")) {
        return 0;
    }

    char metadata_buffer[256];
    if (!assert_true(read_file(metadata_complete, metadata_buffer, sizeof(metadata_buffer)), "read metadata")) {
        return 0;
    }

    return assert_true(strstr(metadata_buffer, "\"redaction_status\":\"complete\"") != NULL, "metadata status") &&
           assert_true(strstr(metadata_buffer, "\"matches\":1") != NULL, "metadata matches");
}

static int test_redact_plan_error(void) {
    char template[] = "/tmp/pap_test_redact_error_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init redact error root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    const char *contents = "%PDF-1.7\nSECRET DATA";
    if (!assert_true(write_file(pdf_src, contents), "write redact pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "{}"), "write invalid metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "bad-redact", pdf_src, metadata_src, 0) == JQ_OK, "submit bad redact job")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_redact %s", root);
    if (!assert_true(run_command(command) == 1, "redact command failure")) {
        return 0;
    }

    char pdf_error[PATH_MAX];
    char metadata_error[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "bad-redact", JQ_STATE_ERROR,
                                  pdf_error, sizeof(pdf_error),
                                  metadata_error, sizeof(metadata_error)) == JQ_OK,
                     "error paths")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_error), "error pdf exists")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_error), "error metadata exists")) {
        return 0;
    }

    char metadata_buffer[256];
    if (!assert_true(read_file(metadata_error, metadata_buffer, sizeof(metadata_buffer)), "read error metadata")) {
        return 0;
    }
    return assert_true(strstr(metadata_buffer, "\"error\"") != NULL, "error metadata includes error") &&
           assert_true(strstr(metadata_buffer, "plan_parse_failed") != NULL, "error metadata includes plan_parse_failed");
}

static int test_redact_empty_queue(void) {
    char template[] = "/tmp/pap_test_redact_empty_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init redact empty root")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_redact %s", root);
    return assert_true(run_command(command) == 2, "redact command no jobs");
}

int main(void) {
    int passed = 1;

    passed &= test_redact_success();
    passed &= test_redact_plan_error();
    passed &= test_redact_empty_queue();

    if (!passed) {
        fprintf(stderr, "Redaction job tests failed.\n");
        return 1;
    }

    printf("Redaction job tests passed.\n");
    return 0;
}
