#include "pap/job_queue.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int read_file(const char *path, char *buffer, size_t buffer_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }
    size_t bytes = fread(buffer, 1, buffer_len - 1, fp);
    buffer[bytes] = '\0';
    fclose(fp);
    return 1;
}

static int create_job_files(const char *root, const char *uuid, int priority) {
    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/%s.pdf", root, uuid);
    snprintf(metadata_src, sizeof(metadata_src), "%s/%s.metadata", root, uuid);

    if (!write_file(pdf_src, "pdf data")) {
        return 0;
    }
    if (!write_file(metadata_src, "metadata")) {
        return 0;
    }

    return jq_submit(root, uuid, pdf_src, metadata_src, priority) == JQ_OK;
}

static int build_locked_paths(const char *root,
                              const char *uuid,
                              const char *dir_name,
                              char *pdf_locked,
                              size_t pdf_locked_len,
                              char *metadata_locked,
                              size_t metadata_locked_len) {
    int pdf_written = snprintf(pdf_locked, pdf_locked_len, "%s/%s/%s.pdf.job.lock", root, dir_name, uuid);
    int metadata_written = snprintf(metadata_locked, metadata_locked_len, "%s/%s/%s.metadata.job.lock", root, dir_name, uuid);
    if (pdf_written < 0 || metadata_written < 0 ||
        (size_t)pdf_written >= pdf_locked_len || (size_t)metadata_written >= metadata_locked_len) {
        return 0;
    }
    return 1;
}

static int test_init_invalid(void) {
    return assert_true(jq_init(NULL) == JQ_ERR_INVALID_ARGUMENT, "jq_init should reject NULL");
}

static int test_job_paths_invalid(void) {
    char buffer[64];
    return assert_true(jq_job_paths(NULL, "id", JQ_STATE_JOBS, buffer, sizeof(buffer), buffer, sizeof(buffer)) ==
                           JQ_ERR_INVALID_ARGUMENT,
                       "jq_job_paths should reject NULL root");
}

static int test_job_paths_overflow(void) {
    char small[8];
    jq_result_t result = jq_job_paths("/tmp", "averylonguuid", JQ_STATE_JOBS, small, sizeof(small), small, sizeof(small));
    return assert_true(result == JQ_ERR_INVALID_ARGUMENT, "jq_job_paths should detect overflow");
}

static int test_submit_and_move(void) {
    char template[] = "/tmp/pap_test_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    jq_result_t init_result = jq_init(root);
    if (!assert_true(init_result == JQ_OK, "jq_init should succeed")) {
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

    jq_result_t submit_result = jq_submit(root, "job-1", pdf_src, metadata_src, 0);
    if (!assert_true(submit_result == JQ_OK, "jq_submit should succeed")) {
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    jq_result_t path_result =
        jq_job_paths(root, "job-1", JQ_STATE_JOBS, pdf_dest, sizeof(pdf_dest), metadata_dest, sizeof(metadata_dest));
    if (!assert_true(path_result == JQ_OK, "jq_job_paths should build paths")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_dest), "pdf job exists")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_dest), "metadata job exists")) {
        return 0;
    }

    char buffer[64];
    if (!assert_true(read_file(metadata_dest, buffer, sizeof(buffer)), "read metadata")) {
        return 0;
    }
    if (!assert_true(strcmp(buffer, "metadata") == 0, "metadata contents")) {
        return 0;
    }

    jq_result_t move_result = jq_move(root, "job-1", JQ_STATE_JOBS, JQ_STATE_COMPLETE);
    if (!assert_true(move_result == JQ_OK, "jq_move should succeed")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    jq_result_t complete_paths = jq_job_paths(root, "job-1", JQ_STATE_COMPLETE, pdf_complete, sizeof(pdf_complete),
                                              metadata_complete, sizeof(metadata_complete));
    if (!assert_true(complete_paths == JQ_OK, "complete paths")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_complete), "pdf moved to complete")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_complete), "metadata moved to complete")) {
        return 0;
    }

    return 1;
}

static int test_submit_missing_source(void) {
    char template[] = "/tmp/pap_test_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for missing source")) {
        return 0;
    }

    char missing_pdf[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(missing_pdf, sizeof(missing_pdf), "%s/missing.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/source.metadata", root);

    if (!assert_true(write_file(metadata_src, "metadata"), "write metadata source")) {
        return 0;
    }

    jq_result_t submit_result = jq_submit(root, "job-missing", missing_pdf, metadata_src, 1);
    if (!assert_true(submit_result == JQ_ERR_NOT_FOUND, "missing pdf should return not found")) {
        return 0;
    }

    return 1;
}

static int test_move_missing_job(void) {
    char template[] = "/tmp/pap_test_move_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for move missing")) {
        return 0;
    }

    jq_result_t move_result = jq_move(root, "missing-job", JQ_STATE_JOBS, JQ_STATE_COMPLETE);
    if (!assert_true(move_result == JQ_ERR_NOT_FOUND, "missing job should return not found")) {
        return 0;
    }

    return 1;
}

static int test_submit_invalid_args(void) {
    return assert_true(jq_submit(NULL, "id", "pdf", "meta", 0) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_submit should reject NULL root");
}

static int test_move_invalid_state(void) {
    char template[] = "/tmp/pap_test_invalid_state_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for invalid state")) {
        return 0;
    }

    jq_result_t move_result = jq_move(root, "job", (jq_state_t)99, JQ_STATE_COMPLETE);
    if (!assert_true(move_result == JQ_ERR_INVALID_ARGUMENT, "invalid state should be rejected")) {
        return 0;
    }

    return 1;
}

static int test_claim_priority(void) {
    char template[] = "/tmp/pap_test_claim_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for claim")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "job-standard", 0), "create standard job")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "job-priority", 1), "create priority job")) {
        return 0;
    }

    char uuid[64];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, 1, uuid, sizeof(uuid), &state);
    if (!assert_true(claim_result == JQ_OK, "jq_claim_next should succeed")) {
        return 0;
    }

    if (!assert_true(strcmp(uuid, "job-priority") == 0, "priority job claimed first")) {
        return 0;
    }

    if (!assert_true(state == JQ_STATE_PRIORITY, "priority state reported")) {
        return 0;
    }

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    if (!assert_true(build_locked_paths(root, uuid, "priority_jobs",
                                        pdf_locked, sizeof(pdf_locked),
                                        metadata_locked, sizeof(metadata_locked)),
                     "build locked paths")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_locked), "locked pdf exists")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_locked), "locked metadata exists")) {
        return 0;
    }

    return 1;
}

static int test_claim_no_jobs(void) {
    char template[] = "/tmp/pap_test_claim_none_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for claim none")) {
        return 0;
    }

    char uuid[32];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, 1, uuid, sizeof(uuid), &state);
    return assert_true(claim_result == JQ_ERR_NOT_FOUND, "jq_claim_next returns not found");
}

static int test_release_and_finalize(void) {
    char template[] = "/tmp/pap_test_release_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for release")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "job-release", 0), "create release job")) {
        return 0;
    }

    char uuid[64];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, 0, uuid, sizeof(uuid), &state);
    if (!assert_true(claim_result == JQ_OK, "claim for release")) {
        return 0;
    }

    jq_result_t release_result = jq_release(root, uuid, state);
    if (!assert_true(release_result == JQ_OK, "release should succeed")) {
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    if (!assert_true(jq_job_paths(root, uuid, state, pdf_dest, sizeof(pdf_dest),
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

    jq_result_t reclaim_result = jq_claim_next(root, 0, uuid, sizeof(uuid), &state);
    if (!assert_true(reclaim_result == JQ_OK, "reclaim after release")) {
        return 0;
    }

    jq_result_t finalize_result = jq_finalize(root, uuid, state, JQ_STATE_COMPLETE);
    if (!assert_true(finalize_result == JQ_OK, "finalize should succeed")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, uuid, JQ_STATE_COMPLETE, pdf_complete, sizeof(pdf_complete),
                                  metadata_complete, sizeof(metadata_complete)) == JQ_OK,
                     "complete paths after finalize")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_complete), "finalized pdf exists")) {
        return 0;
    }
    if (!assert_true(file_exists(metadata_complete), "finalized metadata exists")) {
        return 0;
    }

    return 1;
}

static int test_claim_invalid_args(void) {
    char uuid[8];
    jq_state_t state = JQ_STATE_JOBS;
    return assert_true(jq_claim_next(NULL, 1, uuid, sizeof(uuid), &state) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_claim_next should reject NULL root");
}

static int test_claim_skips_orphan(void) {
    char template[] = "/tmp/pap_test_orphan_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for orphan")) {
        return 0;
    }

    char orphan_pdf[PATH_MAX];
    snprintf(orphan_pdf, sizeof(orphan_pdf), "%s/jobs/orphan.pdf.job", root);
    if (!assert_true(write_file(orphan_pdf, "pdf data"), "write orphan pdf")) {
        return 0;
    }

    char uuid[32];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, 0, uuid, sizeof(uuid), &state);
    return assert_true(claim_result == JQ_ERR_NOT_FOUND, "orphan job should be ignored");
}

static int test_release_invalid_args(void) {
    return assert_true(jq_release(NULL, "job", JQ_STATE_JOBS) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_release should reject NULL root");
}

static int test_finalize_invalid_args(void) {
    return assert_true(jq_finalize(NULL, "job", JQ_STATE_JOBS, JQ_STATE_COMPLETE) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_finalize should reject NULL root");
}

static int test_status_unlocked_and_locked(void) {
    char template[] = "/tmp/pap_test_status_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for status")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "job-status", 0), "create job for status")) {
        return 0;
    }

    jq_state_t state = JQ_STATE_ERROR;
    int locked = 1;
    jq_result_t status_result = jq_status(root, "job-status", &state, &locked);
    if (!assert_true(status_result == JQ_OK, "status for unlocked job")) {
        return 0;
    }
    if (!assert_true(state == JQ_STATE_JOBS, "status state matches")) {
        return 0;
    }
    if (!assert_true(locked == 0, "status unlocked flag")) {
        return 0;
    }

    char uuid[64];
    jq_state_t claim_state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, 0, uuid, sizeof(uuid), &claim_state);
    if (!assert_true(claim_result == JQ_OK, "claim for status")) {
        return 0;
    }

    state = JQ_STATE_ERROR;
    locked = 0;
    status_result = jq_status(root, "job-status", &state, &locked);
    if (!assert_true(status_result == JQ_OK, "status for locked job")) {
        return 0;
    }
    if (!assert_true(state == JQ_STATE_JOBS, "status locked state")) {
        return 0;
    }
    if (!assert_true(locked == 1, "status locked flag")) {
        return 0;
    }

    return 1;
}

static int test_status_not_found(void) {
    char template[] = "/tmp/pap_test_status_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for status missing")) {
        return 0;
    }

    jq_state_t state = JQ_STATE_JOBS;
    int locked = 0;
    return assert_true(jq_status(root, "missing", &state, &locked) == JQ_ERR_NOT_FOUND,
                       "status missing returns not found");
}

static int test_status_partial_pair(void) {
    char template[] = "/tmp/pap_test_status_partial_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for status partial")) {
        return 0;
    }

    char orphan_pdf[PATH_MAX];
    snprintf(orphan_pdf, sizeof(orphan_pdf), "%s/jobs/partial.pdf.job", root);
    if (!assert_true(write_file(orphan_pdf, "pdf data"), "write partial pdf")) {
        return 0;
    }

    jq_state_t state = JQ_STATE_JOBS;
    int locked = 0;
    return assert_true(jq_status(root, "partial", &state, &locked) == JQ_ERR_IO,
                       "status partial pair returns io error");
}

int main(void) {
    int passed = 1;
    passed &= test_init_invalid();
    passed &= test_job_paths_invalid();
    passed &= test_job_paths_overflow();
    passed &= test_submit_and_move();
    passed &= test_submit_missing_source();
    passed &= test_move_missing_job();
    passed &= test_submit_invalid_args();
    passed &= test_move_invalid_state();
    passed &= test_claim_priority();
    passed &= test_claim_no_jobs();
    passed &= test_release_and_finalize();
    passed &= test_claim_invalid_args();
    passed &= test_claim_skips_orphan();
    passed &= test_release_invalid_args();
    passed &= test_finalize_invalid_args();
    passed &= test_status_unlocked_and_locked();
    passed &= test_status_not_found();
    passed &= test_status_partial_pair();

    if (!passed) {
        fprintf(stderr, "Some tests failed.\n");
        return 1;
    }

    printf("All tests passed.\n");
    return 0;
}
