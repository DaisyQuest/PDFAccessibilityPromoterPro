#include "pap/job_queue.h"

#include <errno.h>
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

static int test_cli_submit_claim_finalize(void) {
    char template[] = "/tmp/pap_test_cli_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_cli init %s", root);
    if (!assert_true(run_command(command) == 0, "cli init")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf source")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata source")) {
        return 0;
    }

    snprintf(command, sizeof(command), "./job_queue_cli submit %s job-cli %s %s", root, pdf_src, metadata_src);
    if (!assert_true(run_command(command) == 0, "cli submit")) {
        return 0;
    }

    char output[128];
    snprintf(command, sizeof(command), "./job_queue_cli claim %s", root);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "cli claim output")) {
        return 0;
    }

    if (!assert_true(strncmp(output, "job-cli jobs", strlen("job-cli jobs")) == 0, "claim output")) {
        return 0;
    }

    snprintf(command, sizeof(command), "./job_queue_cli finalize %s job-cli jobs complete", root);
    if (!assert_true(run_command(command) == 0, "cli finalize")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    jq_result_t paths_result = jq_job_paths(root, "job-cli", JQ_STATE_COMPLETE, pdf_complete, sizeof(pdf_complete),
                                            metadata_complete, sizeof(metadata_complete));
    if (!assert_true(paths_result == JQ_OK, "paths complete")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_complete), "pdf complete exists")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_complete), "metadata complete exists")) {
        return 0;
    }

    return 1;
}

static int test_cli_claim_empty(void) {
    char template[] = "/tmp/pap_test_cli_empty_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_cli init %s", root);
    if (!assert_true(run_command(command) == 0, "cli init empty")) {
        return 0;
    }

    snprintf(command, sizeof(command), "./job_queue_cli claim %s", root);
    if (!assert_true(run_command(command) == 2, "cli claim empty returns 2")) {
        return 0;
    }

    return 1;
}

static int test_cli_release(void) {
    char template[] = "/tmp/pap_test_cli_release_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init for release")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf source")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata source")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "job-release", pdf_src, metadata_src, 0) == JQ_OK, "submit job")) {
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_cli claim %s", root);
    if (!assert_true(run_command(command) == 0, "claim for release")) {
        return 0;
    }

    snprintf(command, sizeof(command), "./job_queue_cli release %s job-release jobs", root);
    if (!assert_true(run_command(command) == 0, "release via cli")) {
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "job-release", JQ_STATE_JOBS, pdf_dest, sizeof(pdf_dest),
                                  metadata_dest, sizeof(metadata_dest)) == JQ_OK,
                     "paths after release")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_dest), "pdf released")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_dest), "metadata released")) {
        return 0;
    }

    return 1;
}

static int test_cli_stats(void) {
    char template[] = "/tmp/pap_test_cli_stats_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    char command[COMMAND_BUFFER];
    snprintf(command, sizeof(command), "./job_queue_cli init %s", root);
    if (!assert_true(run_command(command) == 0, "cli init stats")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf stats source")) {
        return 0;
    }
    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata stats source")) {
        return 0;
    }

    if (!assert_true(jq_submit(root, "job-stats", pdf_src, metadata_src, 0) == JQ_OK, "submit stats job")) {
        return 0;
    }

    char output[1024];
    snprintf(command, sizeof(command), "./job_queue_cli stats %s", root);
    if (!assert_true(read_command_output(command, output, sizeof(output)), "cli stats output")) {
        return 0;
    }
    if (!assert_true(strstr(output, "jobs:") != NULL, "stats output has jobs")) {
        return 0;
    }
    if (!assert_true(strstr(output, "totals:") != NULL, "stats output has totals")) {
        return 0;
    }

    return 1;
}

int main(void) {
    int passed = 1;
    passed &= test_cli_submit_claim_finalize();
    passed &= test_cli_claim_empty();
    passed &= test_cli_release();
    passed &= test_cli_stats();

    if (!passed) {
        fprintf(stderr, "Some CLI tests failed.\n");
        return 1;
    }

    printf("All CLI tests passed.\n");
    return 0;
}
