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

static int test_ocr_success(void) {
    char template[] = "/tmp/pap_test_ocr_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init ocr root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    const char *contents = "%PDF-1.6\n1 0 obj\n<<>>\nendobj\n";
    if (!assert_true(write_file(pdf_src, contents), "write ocr pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "{}"), "write metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "ocr-job", pdf_src, metadata_src, 0) == JQ_OK, "submit ocr job")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_ocr %s", root);
    if (!assert_true(run_command(command) == 0, "ocr command success")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "ocr-job", JQ_STATE_COMPLETE,
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

    char metadata_buffer[256];
    if (!assert_true(read_file(metadata_complete, metadata_buffer, sizeof(metadata_buffer)), "read metadata")) {
        return 0;
    }

    return assert_true(strstr(metadata_buffer, "\"ocr_status\":\"complete\"") != NULL, "metadata status") &&
           assert_true(strstr(metadata_buffer, "\"ocr_provider\":\"builtin\"") != NULL, "metadata provider") &&
           assert_true(strstr(metadata_buffer, "\"handwriting_detected\":false") != NULL,
                       "metadata handwriting detected false") &&
           assert_true(strstr(metadata_buffer, "\"handwriting_confidence\":0") != NULL,
                       "metadata handwriting confidence zero") &&
           assert_true(strstr(metadata_buffer, "\"handwriting_markers\":0") != NULL,
                       "metadata handwriting markers zero") &&
           assert_true(strstr(metadata_buffer, "\"pdf_version\":\"1.6\"") != NULL, "metadata version");
}

static int test_ocr_parse_error(void) {
    char template[] = "/tmp/pap_test_ocr_error_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init ocr error root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "NOTPDF"), "write bad pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "{}"), "write metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "bad-ocr", pdf_src, metadata_src, 0) == JQ_OK, "submit bad job")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_ocr %s", root);
    if (!assert_true(run_command(command) == 1, "ocr command failure")) {
        return 0;
    }

    char pdf_error[PATH_MAX];
    char metadata_error[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "bad-ocr", JQ_STATE_ERROR,
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
           assert_true(strstr(metadata_buffer, "parse_error") != NULL, "error metadata includes parse_error");
}

static int test_ocr_empty_queue(void) {
    char template[] = "/tmp/pap_test_ocr_empty_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init ocr empty root")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_ocr %s", root);
    return assert_true(run_command(command) == 2, "ocr command no jobs");
}

static int test_ocr_provider_missing(void) {
    char template[] = "/tmp/pap_test_ocr_provider_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init ocr provider root")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    const char *contents = "%PDF-1.6\n1 0 obj\n<<>>\nendobj\n";
    if (!assert_true(write_file(pdf_src, contents), "write provider pdf")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "{}"), "write metadata")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "missing-provider", pdf_src, metadata_src, 0) == JQ_OK,
                     "submit missing provider job")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "PAP_OCR_PROVIDER=missing ./job_queue_ocr %s", root);
    if (!assert_true(run_command(command) == 1, "ocr command missing provider failure")) {
        return 0;
    }

    char pdf_error[PATH_MAX];
    char metadata_error[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "missing-provider", JQ_STATE_ERROR,
                                  pdf_error, sizeof(pdf_error),
                                  metadata_error, sizeof(metadata_error)) == JQ_OK,
                     "missing provider error paths")) {
        return 0;
    }

    char metadata_buffer[256];
    if (!assert_true(read_file(metadata_error, metadata_buffer, sizeof(metadata_buffer)),
                     "read provider error metadata")) {
        return 0;
    }
    return assert_true(strstr(metadata_buffer, "\"error\"") != NULL, "provider error metadata includes error") &&
           assert_true(strstr(metadata_buffer, "provider_not_found") != NULL, "metadata includes provider_not_found");
}

int main(void) {
    int passed = 1;

    passed &= test_ocr_success();
    passed &= test_ocr_parse_error();
    passed &= test_ocr_empty_queue();
    passed &= test_ocr_provider_missing();

    if (!passed) {
        fprintf(stderr, "OCR job tests failed.\n");
        return 1;
    }

    printf("OCR job tests passed.\n");
    return 0;
}
