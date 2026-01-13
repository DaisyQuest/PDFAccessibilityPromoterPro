#include "pap/job_queue.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

static int write_report_file(const char *path) {
    return write_file(path, "<html>report</html>");
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

static int write_pattern_file(const char *path, size_t bytes, unsigned char seed, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        return 0;
    }
    if (fchmod(fd, mode) != 0) {
        close(fd);
        return 0;
    }
    unsigned char buffer[4096];
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        buffer[i] = (unsigned char)(seed + (i % 251));
    }
    size_t remaining = bytes;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        size_t written = 0;
        while (written < chunk) {
            ssize_t result = write(fd, buffer + written, chunk - written);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return 0;
            }
            written += (size_t)result;
        }
        remaining -= chunk;
    }
    close(fd);
    return 1;
}

static int compare_files(const char *left, const char *right) {
    int left_fd = open(left, O_RDONLY);
    if (left_fd < 0) {
        return 0;
    }
    int right_fd = open(right, O_RDONLY);
    if (right_fd < 0) {
        close(left_fd);
        return 0;
    }

    struct stat left_stat;
    struct stat right_stat;
    if (fstat(left_fd, &left_stat) != 0 || fstat(right_fd, &right_stat) != 0) {
        close(left_fd);
        close(right_fd);
        return 0;
    }
    if (left_stat.st_size != right_stat.st_size) {
        close(left_fd);
        close(right_fd);
        return 0;
    }

    unsigned char left_buf[8192];
    unsigned char right_buf[8192];
    ssize_t left_read;
    while ((left_read = read(left_fd, left_buf, sizeof(left_buf))) > 0) {
        ssize_t right_read = 0;
        while (right_read < left_read) {
            ssize_t chunk = read(right_fd, right_buf + right_read, (size_t)(left_read - right_read));
            if (chunk < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(left_fd);
                close(right_fd);
                return 0;
            }
            if (chunk == 0) {
                close(left_fd);
                close(right_fd);
                return 0;
            }
            right_read += chunk;
        }
        if (memcmp(left_buf, right_buf, (size_t)left_read) != 0) {
            close(left_fd);
            close(right_fd);
            return 0;
        }
    }
    if (left_read < 0) {
        close(left_fd);
        close(right_fd);
        return 0;
    }

    close(left_fd);
    close(right_fd);
    return 1;
}

static int directory_has_tmp_files(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".tmp.") != NULL) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
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

static int test_init_empty(void) {
    return assert_true(jq_init("") == JQ_ERR_INVALID_ARGUMENT, "jq_init should reject empty root");
}

static int test_job_paths_invalid(void) {
    char buffer[64];
    return assert_true(jq_job_paths(NULL, "id", JQ_STATE_JOBS, buffer, sizeof(buffer), buffer, sizeof(buffer)) ==
                           JQ_ERR_INVALID_ARGUMENT,
                       "jq_job_paths should reject NULL root");
}

static int test_report_paths_invalid(void) {
    char buffer[64];
    return assert_true(jq_job_report_paths(NULL, "id", JQ_STATE_JOBS, buffer, sizeof(buffer)) ==
                           JQ_ERR_INVALID_ARGUMENT,
                       "jq_job_report_paths should reject NULL root") &&
           assert_true(jq_job_report_paths("root", NULL, JQ_STATE_JOBS, buffer, sizeof(buffer)) ==
                           JQ_ERR_INVALID_ARGUMENT,
                       "jq_job_report_paths should reject NULL uuid") &&
           assert_true(jq_job_report_paths("root", "id", JQ_STATE_JOBS, NULL, sizeof(buffer)) ==
                           JQ_ERR_INVALID_ARGUMENT,
                       "jq_job_report_paths should reject NULL buffer");
}

static int test_collect_stats_invalid(void) {
    jq_stats_t stats;
    return assert_true(jq_collect_stats(NULL, &stats) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_collect_stats should reject NULL root") &&
           assert_true(jq_collect_stats("/tmp", NULL) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_collect_stats should reject NULL stats");
}

static int test_job_paths_overflow(void) {
    char small[8];
    jq_result_t result = jq_job_paths("/tmp", "averylonguuid", JQ_STATE_JOBS, small, sizeof(small), small, sizeof(small));
    return assert_true(result == JQ_ERR_INVALID_ARGUMENT, "jq_job_paths should detect overflow");
}

static int test_collect_stats(void) {
    char template[] = "/tmp/pap_test_stats_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "init stats root")) {
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

    if (!assert_true(jq_submit(root, "jobs-job", pdf_src, metadata_src, 0) == JQ_OK, "submit jobs job")) {
        return 0;
    }
    if (!assert_true(jq_submit(root, "priority-job", pdf_src, metadata_src, 1) == JQ_OK,
                     "submit priority job")) {
        return 0;
    }
    if (!assert_true(jq_submit(root, "complete-job", pdf_src, metadata_src, 0) == JQ_OK,
                     "submit complete job")) {
        return 0;
    }
    if (!assert_true(jq_move(root, "complete-job", JQ_STATE_JOBS, JQ_STATE_COMPLETE) == JQ_OK,
                     "move complete job")) {
        return 0;
    }

    char report_path[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "complete-job", JQ_STATE_COMPLETE,
                                         report_path, sizeof(report_path)) == JQ_OK,
                     "report path stats")) {
        return 0;
    }
    if (!assert_true(write_report_file(report_path), "write report stats")) {
        return 0;
    }

    char orphan_report[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "orphan-report", JQ_STATE_COMPLETE,
                                         orphan_report, sizeof(orphan_report)) == JQ_OK,
                     "orphan report path")) {
        return 0;
    }
    if (!assert_true(write_report_file(orphan_report), "write orphan report")) {
        return 0;
    }

    char orphan_metadata[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "orphan-metadata", JQ_STATE_ERROR,
                                  pdf_src, sizeof(pdf_src),
                                  orphan_metadata, sizeof(orphan_metadata)) == JQ_OK,
                     "orphan metadata path")) {
        return 0;
    }
    if (!assert_true(write_file(orphan_metadata, "orphan"), "write orphan metadata")) {
        return 0;
    }

    char claimed_uuid[128];
    jq_state_t claimed_state = JQ_STATE_JOBS;
    if (!assert_true(jq_claim_next(root, 1, claimed_uuid, sizeof(claimed_uuid), &claimed_state) == JQ_OK,
                     "claim priority job")) {
        return 0;
    }
    if (!assert_true(strcmp(claimed_uuid, "priority-job") == 0, "claimed priority job")) {
        return 0;
    }

    jq_stats_t stats;
    if (!assert_true(jq_collect_stats(root, &stats) == JQ_OK, "collect stats")) {
        return 0;
    }

    if (!assert_true(stats.states[JQ_STATE_JOBS].pdf_jobs == 1, "jobs pdf count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_JOBS].metadata_jobs == 1, "jobs metadata count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_PRIORITY].pdf_locked == 1, "priority locked pdf count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_PRIORITY].metadata_locked == 1, "priority locked metadata count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_COMPLETE].report_jobs == 2, "complete report count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_COMPLETE].orphan_report == 1, "complete orphan report count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_ERROR].metadata_jobs == 1, "error metadata count")) {
        return 0;
    }
    if (!assert_true(stats.states[JQ_STATE_ERROR].orphan_metadata == 1, "error orphan metadata count")) {
        return 0;
    }
    if (!assert_true(stats.total_jobs > 0, "total jobs tracked")) {
        return 0;
    }
    if (!assert_true(stats.total_locked > 0, "total locked tracked")) {
        return 0;
    }
    if (!assert_true(stats.total_orphans >= 2, "total orphans tracked")) {
        return 0;
    }
    if (!assert_true(stats.total_bytes > 0, "total bytes tracked")) {
        return 0;
    }

    return 1;
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

static int test_submit_metadata_cleanup(void) {
    char template[] = "/tmp/pap_test_submit_cleanup_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for submit cleanup")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/source.pdf", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write pdf source")) {
        return 0;
    }

    char metadata_src[PATH_MAX];
    snprintf(metadata_src, sizeof(metadata_src), "%s/missing.metadata", root);

    jq_result_t submit_result = jq_submit(root, "job-cleanup", pdf_src, metadata_src, 0);
    if (!assert_true(submit_result == JQ_ERR_NOT_FOUND, "missing metadata returns not found")) {
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "job-cleanup", JQ_STATE_JOBS, pdf_dest, sizeof(pdf_dest),
                                  metadata_dest, sizeof(metadata_dest)) == JQ_OK,
                     "paths for cleanup job")) {
        return 0;
    }

    if (!assert_true(!file_exists(pdf_dest), "pdf cleaned up after metadata failure")) {
        return 0;
    }
    if (!assert_true(!file_exists(metadata_dest), "metadata not created on failure")) {
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

static int test_move_partial_pair(void) {
    char template[] = "/tmp/pap_test_move_partial_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for move partial")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/jobs/partial.pdf.job", root);
    if (!assert_true(write_file(pdf_src, "pdf data"), "write partial pdf")) {
        return 0;
    }

    jq_result_t move_result = jq_move(root, "partial", JQ_STATE_JOBS, JQ_STATE_COMPLETE);
    if (!assert_true(move_result == JQ_ERR_NOT_FOUND, "move partial pair returns not found")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "partial", JQ_STATE_COMPLETE, pdf_complete, sizeof(pdf_complete),
                                  metadata_complete, sizeof(metadata_complete)) == JQ_OK,
                     "complete paths for partial")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_src), "pdf restored after failed move")) {
        return 0;
    }
    if (!assert_true(!file_exists(pdf_complete), "pdf not moved to complete")) {
        return 0;
    }

    return 1;
}

static int test_submit_invalid_args(void) {
    return assert_true(jq_submit(NULL, "id", "pdf", "meta", 0) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_submit should reject NULL root");
}

static int test_submit_large_files(void) {
    char template[] = "/tmp/pap_test_large_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for large files")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/large.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/large.metadata", root);

    size_t pdf_size = (1024 * 1024 * 3) + 123;
    size_t metadata_size = (1024 * 512) + 17;
    if (!assert_true(write_pattern_file(pdf_src, pdf_size, 17, 0600), "write large pdf")) {
        return 0;
    }
    if (!assert_true(write_pattern_file(metadata_src, metadata_size, 99, 0640), "write large metadata")) {
        return 0;
    }

    jq_result_t submit_result = jq_submit(root, "job-large", pdf_src, metadata_src, 0);
    if (!assert_true(submit_result == JQ_OK, "jq_submit large files")) {
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "job-large", JQ_STATE_JOBS,
                                  pdf_dest, sizeof(pdf_dest),
                                  metadata_dest, sizeof(metadata_dest)) == JQ_OK,
                     "job paths for large files")) {
        return 0;
    }

    if (!assert_true(compare_files(pdf_src, pdf_dest), "pdf contents preserved")) {
        return 0;
    }
    if (!assert_true(compare_files(metadata_src, metadata_dest), "metadata contents preserved")) {
        return 0;
    }

    struct stat pdf_stat;
    struct stat metadata_stat;
    if (!assert_true(stat(pdf_dest, &pdf_stat) == 0, "stat pdf dest")) {
        return 0;
    }
    if (!assert_true(stat(metadata_dest, &metadata_stat) == 0, "stat metadata dest")) {
        return 0;
    }
    if (!assert_true((pdf_stat.st_mode & 0777) == 0600, "pdf mode preserved")) {
        return 0;
    }
    if (!assert_true((metadata_stat.st_mode & 0777) == 0640, "metadata mode preserved")) {
        return 0;
    }

    return 1;
}

static int test_submit_atomic_cleanup(void) {
    char template[] = "/tmp/pap_test_atomic_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for atomic submit")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/atomic.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/atomic.metadata", root);

    if (!assert_true(write_pattern_file(pdf_src, 1024 * 128, 7, 0600), "write atomic pdf")) {
        return 0;
    }
    if (!assert_true(write_pattern_file(metadata_src, 1024 * 64, 3, 0644), "write atomic metadata")) {
        return 0;
    }

    jq_result_t submit_result = jq_submit(root, "job-atomic", pdf_src, metadata_src, 0);
    if (!assert_true(submit_result == JQ_OK, "jq_submit atomic")) {
        return 0;
    }

    char jobs_dir[PATH_MAX];
    snprintf(jobs_dir, sizeof(jobs_dir), "%s/jobs", root);
    return assert_true(!directory_has_tmp_files(jobs_dir), "no temp files after submit");
}

static int test_submit_missing_dir_cleanup(void) {
    char template[] = "/tmp/pap_test_missing_dir_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for perm submit")) {
        return 0;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    snprintf(pdf_src, sizeof(pdf_src), "%s/perm.pdf", root);
    snprintf(metadata_src, sizeof(metadata_src), "%s/perm.metadata", root);

    if (!assert_true(write_pattern_file(pdf_src, 1024 * 32, 11, 0600), "write perm pdf")) {
        return 0;
    }
    if (!assert_true(write_pattern_file(metadata_src, 1024 * 16, 22, 0644), "write perm metadata")) {
        return 0;
    }

    char jobs_dir[PATH_MAX];
    char jobs_backup[PATH_MAX];
    snprintf(jobs_dir, sizeof(jobs_dir), "%s/jobs", root);
    snprintf(jobs_backup, sizeof(jobs_backup), "%s/jobs_backup", root);
    if (rename(jobs_dir, jobs_backup) != 0) {
        perror("rename jobs dir");
        return 0;
    }

    jq_result_t submit_result = jq_submit(root, "job-perm", pdf_src, metadata_src, 0);
    if (!assert_true(submit_result == JQ_ERR_IO, "jq_submit missing jobs dir")) {
        rename(jobs_backup, jobs_dir);
        return 0;
    }

    if (!assert_true(!directory_has_tmp_files(root), "no temp files after failure")) {
        rename(jobs_backup, jobs_dir);
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "job-perm", JQ_STATE_JOBS,
                                  pdf_dest, sizeof(pdf_dest),
                                  metadata_dest, sizeof(metadata_dest)) == JQ_OK,
                     "paths for missing dir job")) {
        rename(jobs_backup, jobs_dir);
        return 0;
    }
    if (!assert_true(!file_exists(pdf_dest), "pdf not created on failure")) {
        rename(jobs_backup, jobs_dir);
        return 0;
    }
    if (!assert_true(!file_exists(metadata_dest), "metadata not created on failure")) {
        rename(jobs_backup, jobs_dir);
        return 0;
    }

    rename(jobs_backup, jobs_dir);
    return 1;
}

static int test_job_paths_invalid_state(void) {
    char buffer[64];
    return assert_true(jq_job_paths("/tmp", "id", (jq_state_t)77, buffer, sizeof(buffer), buffer, sizeof(buffer)) ==
                           JQ_ERR_INVALID_ARGUMENT,
                       "jq_job_paths should reject invalid state");
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

static int test_claim_uuid_too_small(void) {
    char template[] = "/tmp/pap_test_claim_small_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for claim small")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "long-job-uuid", 0), "create long job")) {
        return 0;
    }

    char uuid[4];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, 0, uuid, sizeof(uuid), &state);
    return assert_true(claim_result == JQ_ERR_INVALID_ARGUMENT, "claim rejects small uuid buffer");
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

static int test_release_rolls_back_on_missing_metadata(void) {
    char template[] = "/tmp/pap_test_release_partial_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for release partial")) {
        return 0;
    }

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    if (!assert_true(build_locked_paths(root, "job-partial", "jobs",
                                        pdf_locked, sizeof(pdf_locked),
                                        metadata_locked, sizeof(metadata_locked)),
                     "build locked paths for release partial")) {
        return 0;
    }

    if (!assert_true(write_file(pdf_locked, "pdf data"), "write locked pdf")) {
        return 0;
    }

    jq_result_t release_result = jq_release(root, "job-partial", JQ_STATE_JOBS);
    if (!assert_true(release_result == JQ_ERR_NOT_FOUND, "release missing metadata returns not found")) {
        return 0;
    }

    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "job-partial", JQ_STATE_JOBS,
                                  pdf_dest, sizeof(pdf_dest),
                                  metadata_dest, sizeof(metadata_dest)) == JQ_OK,
                     "job paths after release partial")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_locked), "locked pdf restored after release failure")) {
        return 0;
    }
    if (!assert_true(!file_exists(pdf_dest), "unlocked pdf not left behind")) {
        return 0;
    }
    if (!assert_true(!file_exists(metadata_dest), "metadata not created on release failure")) {
        return 0;
    }

    return 1;
}

static int test_finalize_rolls_back_on_missing_metadata(void) {
    char template[] = "/tmp/pap_test_finalize_partial_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for finalize partial")) {
        return 0;
    }

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    if (!assert_true(build_locked_paths(root, "job-finalize", "jobs",
                                        pdf_locked, sizeof(pdf_locked),
                                        metadata_locked, sizeof(metadata_locked)),
                     "build locked paths for finalize partial")) {
        return 0;
    }

    if (!assert_true(write_file(pdf_locked, "pdf data"), "write locked pdf for finalize")) {
        return 0;
    }

    jq_result_t finalize_result = jq_finalize(root, "job-finalize", JQ_STATE_JOBS, JQ_STATE_COMPLETE);
    if (!assert_true(finalize_result == JQ_ERR_NOT_FOUND, "finalize missing metadata returns not found")) {
        return 0;
    }

    char pdf_complete[PATH_MAX];
    char metadata_complete[PATH_MAX];
    if (!assert_true(jq_job_paths(root, "job-finalize", JQ_STATE_COMPLETE,
                                  pdf_complete, sizeof(pdf_complete),
                                  metadata_complete, sizeof(metadata_complete)) == JQ_OK,
                     "complete paths after finalize partial")) {
        return 0;
    }

    if (!assert_true(file_exists(pdf_locked), "locked pdf restored after finalize failure")) {
        return 0;
    }
    if (!assert_true(!file_exists(pdf_complete), "complete pdf not left behind")) {
        return 0;
    }
    if (!assert_true(!file_exists(metadata_complete), "metadata not created on finalize failure")) {
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

static int test_release_missing_job(void) {
    char template[] = "/tmp/pap_test_release_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for release missing")) {
        return 0;
    }

    return assert_true(jq_release(root, "missing", JQ_STATE_JOBS) == JQ_ERR_NOT_FOUND,
                       "jq_release returns not found for missing job");
}

static int test_release_invalid_state(void) {
    char template[] = "/tmp/pap_test_release_invalid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for release invalid")) {
        return 0;
    }

    return assert_true(jq_release(root, "job", (jq_state_t)99) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_release rejects invalid state");
}

static int test_finalize_invalid_args(void) {
    return assert_true(jq_finalize(NULL, "job", JQ_STATE_JOBS, JQ_STATE_COMPLETE) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_finalize should reject NULL root");
}

static int test_finalize_missing_job(void) {
    char template[] = "/tmp/pap_test_finalize_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for finalize missing")) {
        return 0;
    }

    return assert_true(jq_finalize(root, "missing", JQ_STATE_JOBS, JQ_STATE_COMPLETE) == JQ_ERR_NOT_FOUND,
                       "jq_finalize returns not found for missing job");
}

static int test_finalize_invalid_state(void) {
    char template[] = "/tmp/pap_test_finalize_invalid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for finalize invalid")) {
        return 0;
    }

    return assert_true(jq_finalize(root, "job", (jq_state_t)99, JQ_STATE_COMPLETE) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_finalize rejects invalid from state");
}

static int test_report_moves_on_finalize(void) {
    char template[] = "/tmp/pap_test_report_finalize_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for report finalize")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "report-finalize", 0), "create job files for report finalize")) {
        return 0;
    }

    char uuid[64];
    jq_state_t state = JQ_STATE_JOBS;
    if (!assert_true(jq_claim_next(root, 0, uuid, sizeof(uuid), &state) == JQ_OK, "claim for report finalize")) {
        return 0;
    }

    char report_locked[PATH_MAX];
    if (!assert_true(jq_job_report_paths_locked(root, "report-finalize", state,
                                                report_locked, sizeof(report_locked)) == JQ_OK,
                     "report locked path finalize")) {
        return 0;
    }
    if (!assert_true(write_report_file(report_locked), "write report locked finalize")) {
        return 0;
    }

    if (!assert_true(jq_finalize(root, "report-finalize", state, JQ_STATE_COMPLETE) == JQ_OK,
                     "finalize report job")) {
        return 0;
    }

    char report_complete[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "report-finalize", JQ_STATE_COMPLETE,
                                         report_complete, sizeof(report_complete)) == JQ_OK,
                     "report complete path finalize")) {
        return 0;
    }
    return assert_true(file_exists(report_complete), "report moved on finalize");
}

static int test_report_moves_on_release(void) {
    char template[] = "/tmp/pap_test_report_release_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for report release")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "report-release", 0), "create job files for report release")) {
        return 0;
    }

    char uuid[64];
    jq_state_t state = JQ_STATE_JOBS;
    if (!assert_true(jq_claim_next(root, 0, uuid, sizeof(uuid), &state) == JQ_OK, "claim for report release")) {
        return 0;
    }

    char report_locked[PATH_MAX];
    if (!assert_true(jq_job_report_paths_locked(root, "report-release", state,
                                                report_locked, sizeof(report_locked)) == JQ_OK,
                     "report locked path release")) {
        return 0;
    }
    if (!assert_true(write_report_file(report_locked), "write report locked release")) {
        return 0;
    }

    if (!assert_true(jq_release(root, "report-release", state) == JQ_OK, "release report job")) {
        return 0;
    }

    char report_unlocked[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "report-release", state,
                                         report_unlocked, sizeof(report_unlocked)) == JQ_OK,
                     "report unlocked path release")) {
        return 0;
    }
    return assert_true(file_exists(report_unlocked), "report moved on release");
}

static int test_report_moves_on_move(void) {
    char template[] = "/tmp/pap_test_report_move_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for report move")) {
        return 0;
    }

    if (!assert_true(create_job_files(root, "report-move", 0), "create job files for report move")) {
        return 0;
    }

    char report_jobs[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "report-move", JQ_STATE_JOBS,
                                         report_jobs, sizeof(report_jobs)) == JQ_OK,
                     "report jobs path move")) {
        return 0;
    }
    if (!assert_true(write_report_file(report_jobs), "write report jobs")) {
        return 0;
    }

    if (!assert_true(jq_move(root, "report-move", JQ_STATE_JOBS, JQ_STATE_COMPLETE) == JQ_OK, "move report job")) {
        return 0;
    }

    char report_complete[PATH_MAX];
    if (!assert_true(jq_job_report_paths(root, "report-move", JQ_STATE_COMPLETE,
                                         report_complete, sizeof(report_complete)) == JQ_OK,
                     "report complete path move")) {
        return 0;
    }
    return assert_true(file_exists(report_complete), "report moved on move");
}

static int test_status_invalid_args(void) {
    jq_state_t state = JQ_STATE_JOBS;
    int locked = 0;
    if (!assert_true(jq_status(NULL, "job", &state, &locked) == JQ_ERR_INVALID_ARGUMENT,
                     "jq_status rejects NULL root")) {
        return 0;
    }
    if (!assert_true(jq_status("/tmp", NULL, &state, &locked) == JQ_ERR_INVALID_ARGUMENT,
                     "jq_status rejects NULL uuid")) {
        return 0;
    }
    if (!assert_true(jq_status("/tmp", "job", NULL, &locked) == JQ_ERR_INVALID_ARGUMENT,
                     "jq_status rejects NULL state_out")) {
        return 0;
    }
    return assert_true(jq_status("/tmp", "job", &state, NULL) == JQ_ERR_INVALID_ARGUMENT,
                       "jq_status rejects NULL locked_out");
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

static int test_status_metadata_only(void) {
    char template[] = "/tmp/pap_test_status_meta_only_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }

    if (!assert_true(jq_init(root) == JQ_OK, "jq_init for status metadata only")) {
        return 0;
    }

    char orphan_metadata[PATH_MAX];
    snprintf(orphan_metadata, sizeof(orphan_metadata), "%s/jobs/partial.metadata.job", root);
    if (!assert_true(write_file(orphan_metadata, "metadata"), "write partial metadata")) {
        return 0;
    }

    jq_state_t state = JQ_STATE_JOBS;
    int locked = 0;
    return assert_true(jq_status(root, "partial", &state, &locked) == JQ_ERR_IO,
                       "status metadata-only returns io error");
}

int main(void) {
    int passed = 1;
    passed &= test_init_invalid();
    passed &= test_init_empty();
    passed &= test_job_paths_invalid();
    passed &= test_report_paths_invalid();
    passed &= test_collect_stats_invalid();
    passed &= test_job_paths_overflow();
    passed &= test_collect_stats();
    passed &= test_submit_and_move();
    passed &= test_submit_missing_source();
    passed &= test_submit_metadata_cleanup();
    passed &= test_move_missing_job();
    passed &= test_move_partial_pair();
    passed &= test_submit_invalid_args();
    passed &= test_submit_large_files();
    passed &= test_submit_atomic_cleanup();
    passed &= test_submit_missing_dir_cleanup();
    passed &= test_job_paths_invalid_state();
    passed &= test_move_invalid_state();
    passed &= test_claim_uuid_too_small();
    passed &= test_claim_priority();
    passed &= test_claim_no_jobs();
    passed &= test_release_and_finalize();
    passed &= test_release_rolls_back_on_missing_metadata();
    passed &= test_claim_invalid_args();
    passed &= test_claim_skips_orphan();
    passed &= test_release_invalid_args();
    passed &= test_release_missing_job();
    passed &= test_release_invalid_state();
    passed &= test_finalize_invalid_args();
    passed &= test_finalize_missing_job();
    passed &= test_finalize_invalid_state();
    passed &= test_report_moves_on_finalize();
    passed &= test_report_moves_on_release();
    passed &= test_report_moves_on_move();
    passed &= test_status_invalid_args();
    passed &= test_status_unlocked_and_locked();
    passed &= test_status_not_found();
    passed &= test_status_partial_pair();
    passed &= test_finalize_rolls_back_on_missing_metadata();
    passed &= test_status_metadata_only();

    if (!passed) {
        fprintf(stderr, "Some tests failed.\n");
        return 1;
    }

    printf("All tests passed.\n");
    return 0;
}
