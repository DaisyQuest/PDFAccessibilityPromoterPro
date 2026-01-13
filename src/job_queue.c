#include "pap/job_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *jq_state_dir(jq_state_t state) {
    switch (state) {
        case JQ_STATE_JOBS:
            return "jobs";
        case JQ_STATE_PRIORITY:
            return "priority_jobs";
        case JQ_STATE_COMPLETE:
            return "complete";
        case JQ_STATE_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static jq_result_t jq_ensure_dir(const char *path) {
    if (!path) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    if (mkdir(path, 0755) == 0) {
        return JQ_OK;
    }

    if (errno == EEXIST) {
        return JQ_OK;
    }

    return JQ_ERR_IO;
}

static jq_result_t jq_ensure_state_dir(const char *root_path, jq_state_t state) {
    if (!root_path) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *dir = jq_state_dir(state);
    if (!dir) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s", root_path, dir);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    return jq_ensure_dir(path);
}

jq_result_t jq_init(const char *root_path) {
    if (!root_path || root_path[0] == '\0') {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    char path[PATH_MAX];
    const char *dirs[] = {"jobs", "priority_jobs", "complete", "error"};

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        int written = snprintf(path, sizeof(path), "%s/%s", root_path, dirs[i]);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            return JQ_ERR_INVALID_ARGUMENT;
        }
        jq_result_t result = jq_ensure_dir(path);
        if (result != JQ_OK) {
            return result;
        }
    }

    return JQ_OK;
}

static jq_result_t jq_copy_file(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    char tmp_path[PATH_MAX];
    int tmp_written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", dst_path);
    if (tmp_written < 0 || (size_t)tmp_written >= sizeof(tmp_path)) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        return errno == ENOENT ? JQ_ERR_NOT_FOUND : JQ_ERR_IO;
    }

    struct stat src_stat;
    if (fstat(src_fd, &src_stat) != 0) {
        close(src_fd);
        return JQ_ERR_IO;
    }

    int dst_fd = mkstemp(tmp_path);
    if (dst_fd < 0) {
        close(src_fd);
        return JQ_ERR_IO;
    }

    mode_t mode = src_stat.st_mode & 0777;
    if (fchmod(dst_fd, mode) != 0) {
        close(src_fd);
        close(dst_fd);
        unlink(tmp_path);
        return JQ_ERR_IO;
    }
#if defined(POSIX_FADV_SEQUENTIAL)
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#if defined(POSIX_FADV_NOREUSE)
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_NOREUSE);
#endif
    if (src_stat.st_size > 0) {
        int fallocate_result = posix_fallocate(dst_fd, 0, src_stat.st_size);
        if (fallocate_result != 0 && fallocate_result != EINTR) {
            if (ftruncate(dst_fd, src_stat.st_size) != 0) {
                close(src_fd);
                close(dst_fd);
                unlink(tmp_path);
                return JQ_ERR_IO;
            }
        }
    }

    char stack_buffer[16 * 1024];
    size_t buffer_size = 1024 * 1024;
    char *buffer = malloc(buffer_size);
    if (!buffer) {
        buffer = stack_buffer;
        buffer_size = sizeof(stack_buffer);
    }
    ssize_t bytes_read;
    while (1) {
        bytes_read = read(src_fd, buffer, buffer_size);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(src_fd);
            close(dst_fd);
            if (buffer != stack_buffer) {
                free(buffer);
            }
            unlink(tmp_path);
            return JQ_ERR_IO;
        }
        if (bytes_read == 0) {
            break;
        }
        ssize_t bytes_written = 0;
        while (bytes_written < bytes_read) {
            ssize_t chunk = write(dst_fd, buffer + bytes_written, (size_t)(bytes_read - bytes_written));
            if (chunk < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(src_fd);
                close(dst_fd);
                if (buffer != stack_buffer) {
                    free(buffer);
                }
                unlink(tmp_path);
                return JQ_ERR_IO;
            }
            bytes_written += chunk;
        }
    }

    if (fsync(dst_fd) != 0) {
        close(src_fd);
        close(dst_fd);
        if (buffer != stack_buffer) {
            free(buffer);
        }
        unlink(tmp_path);
        return JQ_ERR_IO;
    }

    close(src_fd);
    close(dst_fd);
    if (buffer != stack_buffer) {
        free(buffer);
    }

    if (rename(tmp_path, dst_path) != 0) {
        unlink(tmp_path);
        return JQ_ERR_IO;
    }

    const char *slash = strrchr(dst_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - dst_path);
        if (dir_len > 0 && dir_len < PATH_MAX) {
            char dir_path[PATH_MAX];
            memcpy(dir_path, dst_path, dir_len);
            dir_path[dir_len] = '\0';
            int dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
            if (dir_fd >= 0) {
                (void)fsync(dir_fd);
                close(dir_fd);
            }
        }
    }
    return JQ_OK;
}

jq_result_t jq_job_paths_locked(const char *root_path,
                                const char *uuid,
                                jq_state_t state,
                                char *pdf_out,
                                size_t pdf_out_len,
                                char *metadata_out,
                                size_t metadata_out_len) {
    if (!root_path || !uuid || !pdf_out || !metadata_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *dir = jq_state_dir(state);
    if (!dir) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    int pdf_written = snprintf(pdf_out, pdf_out_len, "%s/%s/%s.pdf.job.lock", root_path, dir, uuid);
    int metadata_written =
        snprintf(metadata_out, metadata_out_len, "%s/%s/%s.metadata.job.lock", root_path, dir, uuid);

    if (pdf_written < 0 || metadata_written < 0 ||
        (size_t)pdf_written >= pdf_out_len || (size_t)metadata_written >= metadata_out_len) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    return JQ_OK;
}

jq_result_t jq_job_report_paths(const char *root_path,
                                const char *uuid,
                                jq_state_t state,
                                char *report_out,
                                size_t report_out_len) {
    if (!root_path || !uuid || !report_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *dir = jq_state_dir(state);
    if (!dir) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    int written = snprintf(report_out, report_out_len, "%s/%s/%s.report.html", root_path, dir, uuid);
    if (written < 0 || (size_t)written >= report_out_len) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    return JQ_OK;
}

jq_result_t jq_job_report_paths_locked(const char *root_path,
                                       const char *uuid,
                                       jq_state_t state,
                                       char *report_out,
                                       size_t report_out_len) {
    if (!root_path || !uuid || !report_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *dir = jq_state_dir(state);
    if (!dir) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    int written = snprintf(report_out, report_out_len, "%s/%s/%s.report.html.lock", root_path, dir, uuid);
    if (written < 0 || (size_t)written >= report_out_len) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    return JQ_OK;
}

jq_result_t jq_job_paths(const char *root_path,
                         const char *uuid,
                         jq_state_t state,
                         char *pdf_out,
                         size_t pdf_out_len,
                         char *metadata_out,
                         size_t metadata_out_len) {
    if (!root_path || !uuid || !pdf_out || !metadata_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *dir = jq_state_dir(state);
    if (!dir) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    int pdf_written = snprintf(pdf_out, pdf_out_len, "%s/%s/%s.pdf.job", root_path, dir, uuid);
    int metadata_written =
        snprintf(metadata_out, metadata_out_len, "%s/%s/%s.metadata.job", root_path, dir, uuid);

    if (pdf_written < 0 || metadata_written < 0 ||
        (size_t)pdf_written >= pdf_out_len || (size_t)metadata_written >= metadata_out_len) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    return JQ_OK;
}

jq_result_t jq_submit(const char *root_path,
                      const char *uuid,
                      const char *pdf_path,
                      const char *metadata_path,
                      int priority) {
    if (!root_path || !uuid || !pdf_path || !metadata_path) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    jq_state_t state = priority ? JQ_STATE_PRIORITY : JQ_STATE_JOBS;
    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];

    jq_result_t path_result =
        jq_job_paths(root_path, uuid, state, pdf_dest, sizeof(pdf_dest), metadata_dest, sizeof(metadata_dest));
    if (path_result != JQ_OK) {
        return path_result;
    }

    jq_result_t pdf_result = jq_copy_file(pdf_path, pdf_dest);
    if (pdf_result != JQ_OK) {
        return pdf_result;
    }

    jq_result_t metadata_result = jq_copy_file(metadata_path, metadata_dest);
    if (metadata_result != JQ_OK) {
        unlink(pdf_dest);
        return metadata_result;
    }

    return JQ_OK;
}

static jq_result_t jq_rename(const char *src, const char *dst) {
    if (rename(src, dst) == 0) {
        return JQ_OK;
    }

    if (errno == ENOENT) {
        return JQ_ERR_NOT_FOUND;
    }

    return JQ_ERR_IO;
}

static jq_result_t jq_move_report_if_present(const char *report_src, const char *report_dst) {
    if (!report_src || !report_dst) {
        return JQ_ERR_INVALID_ARGUMENT;
    }
    if (access(report_src, F_OK) != 0) {
        if (errno == ENOENT) {
            return JQ_OK;
        }
        return JQ_ERR_IO;
    }
    return jq_rename(report_src, report_dst);
}

static jq_result_t jq_check_pair_exists(const char *pdf_path, const char *metadata_path, int *present_out) {
    if (!pdf_path || !metadata_path || !present_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    int pdf_ok = access(pdf_path, F_OK) == 0;
    int pdf_errno = errno;
    int metadata_ok = access(metadata_path, F_OK) == 0;
    int metadata_errno = errno;

    if (pdf_ok && metadata_ok) {
        *present_out = 1;
        return JQ_OK;
    }

    if (!pdf_ok && !metadata_ok) {
        if (pdf_errno != ENOENT || metadata_errno != ENOENT) {
            return JQ_ERR_IO;
        }
        *present_out = 0;
        return JQ_OK;
    }

    return JQ_ERR_IO;
}

jq_result_t jq_move(const char *root_path,
                    const char *uuid,
                    jq_state_t from_state,
                    jq_state_t to_state) {
    if (!root_path || !uuid) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    jq_result_t ensure_result = jq_ensure_state_dir(root_path, to_state);
    if (ensure_result != JQ_OK) {
        return ensure_result;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    char pdf_dst[PATH_MAX];
    char metadata_dst[PATH_MAX];
    char report_src[PATH_MAX];
    char report_dst[PATH_MAX];

    jq_result_t src_result =
        jq_job_paths(root_path, uuid, from_state, pdf_src, sizeof(pdf_src), metadata_src, sizeof(metadata_src));
    if (src_result != JQ_OK) {
        return src_result;
    }

    jq_result_t dst_result =
        jq_job_paths(root_path, uuid, to_state, pdf_dst, sizeof(pdf_dst), metadata_dst, sizeof(metadata_dst));
    if (dst_result != JQ_OK) {
        return dst_result;
    }

    jq_result_t report_src_result =
        jq_job_report_paths(root_path, uuid, from_state, report_src, sizeof(report_src));
    if (report_src_result != JQ_OK) {
        return report_src_result;
    }

    jq_result_t report_dst_result =
        jq_job_report_paths(root_path, uuid, to_state, report_dst, sizeof(report_dst));
    if (report_dst_result != JQ_OK) {
        return report_dst_result;
    }

    jq_result_t pdf_move = jq_rename(pdf_src, pdf_dst);
    if (pdf_move != JQ_OK) {
        return pdf_move;
    }

    jq_result_t metadata_move = jq_rename(metadata_src, metadata_dst);
    if (metadata_move != JQ_OK) {
        jq_rename(pdf_dst, pdf_src);
        return metadata_move;
    }

    jq_result_t report_move = jq_move_report_if_present(report_src, report_dst);
    if (report_move != JQ_OK) {
        jq_rename(metadata_dst, metadata_src);
        jq_rename(pdf_dst, pdf_src);
        return report_move;
    }

    return JQ_OK;
}

jq_result_t jq_status(const char *root_path,
                      const char *uuid,
                      jq_state_t *state_out,
                      int *locked_out) {
    if (!root_path || !uuid || !state_out || !locked_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const jq_state_t states[] = {JQ_STATE_PRIORITY, JQ_STATE_JOBS, JQ_STATE_COMPLETE, JQ_STATE_ERROR};

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        char pdf_path[PATH_MAX];
        char metadata_path[PATH_MAX];
        jq_result_t path_result =
            jq_job_paths(root_path, uuid, states[i], pdf_path, sizeof(pdf_path), metadata_path, sizeof(metadata_path));
        if (path_result != JQ_OK) {
            return path_result;
        }

        int present = 0;
        jq_result_t check_result = jq_check_pair_exists(pdf_path, metadata_path, &present);
        if (check_result != JQ_OK) {
            return check_result;
        }
        if (present) {
            *state_out = states[i];
            *locked_out = 0;
            return JQ_OK;
        }

        char pdf_locked[PATH_MAX];
        char metadata_locked[PATH_MAX];
        jq_result_t locked_result =
            jq_job_paths_locked(root_path, uuid, states[i], pdf_locked, sizeof(pdf_locked),
                                metadata_locked, sizeof(metadata_locked));
        if (locked_result != JQ_OK) {
            return locked_result;
        }

        present = 0;
        check_result = jq_check_pair_exists(pdf_locked, metadata_locked, &present);
        if (check_result != JQ_OK) {
            return check_result;
        }
        if (present) {
            *state_out = states[i];
            *locked_out = 1;
            return JQ_OK;
        }
    }

    return JQ_ERR_NOT_FOUND;
}

static int jq_has_suffix(const char *name, const char *suffix) {
    if (!name || !suffix) {
        return 0;
    }
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > name_len) {
        return 0;
    }
    return strcmp(name + name_len - suffix_len, suffix) == 0;
}

static void jq_update_mtime(time_t mtime, time_t *oldest, time_t *newest) {
    if (mtime <= 0) {
        return;
    }
    if (*oldest == 0 || mtime < *oldest) {
        *oldest = mtime;
    }
    if (*newest == 0 || mtime > *newest) {
        *newest = mtime;
    }
}

static int jq_build_dir_path(const char *root_path, const char *dir_name, char *out, size_t out_len) {
    int written = snprintf(out, out_len, "%s/%s", root_path, dir_name);
    if (written < 0 || (size_t)written >= out_len) {
        return 0;
    }
    return 1;
}

static int jq_build_entry_path(const char *dir_path, const char *name, char *out, size_t out_len) {
    int written = snprintf(out, out_len, "%s/%s", dir_path, name);
    if (written < 0 || (size_t)written >= out_len) {
        return 0;
    }
    return 1;
}

static int jq_check_counterpart(const char *dir_path,
                                const char *name,
                                const char *suffix,
                                const char *counter_suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > name_len) {
        return 0;
    }
    size_t base_len = name_len - suffix_len;
    char counterpart[PATH_MAX];
    if (base_len + strlen(counter_suffix) + 1 > sizeof(counterpart)) {
        return 0;
    }
    memcpy(counterpart, name, base_len);
    memcpy(counterpart + base_len, counter_suffix, strlen(counter_suffix) + 1);

    char path[PATH_MAX];
    if (!jq_build_entry_path(dir_path, counterpart, path, sizeof(path))) {
        return 0;
    }
    if (access(path, F_OK) == 0) {
        return 1;
    }
    if (errno == ENOENT) {
        return 0;
    }
    return 1;
}

static jq_result_t jq_collect_state_stats(const char *root_path,
                                          jq_state_t state,
                                          jq_state_stats_t *stats,
                                          time_t *oldest_mtime,
                                          time_t *newest_mtime) {
    if (!root_path || !stats) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *dir_name = jq_state_dir(state);
    if (!dir_name) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    char dir_path[PATH_MAX];
    if (!jq_build_dir_path(root_path, dir_name, dir_path, sizeof(dir_path))) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return errno == ENOENT ? JQ_ERR_NOT_FOUND : JQ_ERR_IO;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        const char *suffix = NULL;
        enum {
            JQ_FILE_UNKNOWN = 0,
            JQ_FILE_PDF = 1,
            JQ_FILE_METADATA = 2,
            JQ_FILE_REPORT = 3
        } file_type = JQ_FILE_UNKNOWN;
        int locked = 0;

        if (jq_has_suffix(name, ".pdf.job.lock")) {
            suffix = ".pdf.job.lock";
            file_type = JQ_FILE_PDF;
            locked = 1;
        } else if (jq_has_suffix(name, ".pdf.job")) {
            suffix = ".pdf.job";
            file_type = JQ_FILE_PDF;
        } else if (jq_has_suffix(name, ".metadata.job.lock")) {
            suffix = ".metadata.job.lock";
            file_type = JQ_FILE_METADATA;
            locked = 1;
        } else if (jq_has_suffix(name, ".metadata.job")) {
            suffix = ".metadata.job";
            file_type = JQ_FILE_METADATA;
        } else if (jq_has_suffix(name, ".report.html.lock")) {
            suffix = ".report.html.lock";
            file_type = JQ_FILE_REPORT;
            locked = 1;
        } else if (jq_has_suffix(name, ".report.html")) {
            suffix = ".report.html";
            file_type = JQ_FILE_REPORT;
        } else {
            continue;
        }

        char path[PATH_MAX];
        if (!jq_build_entry_path(dir_path, name, path, sizeof(path))) {
            closedir(dir);
            return JQ_ERR_INVALID_ARGUMENT;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            closedir(dir);
            return JQ_ERR_IO;
        }
        jq_update_mtime(st.st_mtime, oldest_mtime, newest_mtime);

        if (file_type == JQ_FILE_PDF) {
            if (locked) {
                stats->pdf_locked++;
            } else {
                stats->pdf_jobs++;
            }
            stats->pdf_bytes += (unsigned long long)st.st_size;
            const char *counter_suffix = locked ? ".metadata.job.lock" : ".metadata.job";
            if (!jq_check_counterpart(dir_path, name, suffix, counter_suffix)) {
                stats->orphan_pdf++;
            }
        } else if (file_type == JQ_FILE_METADATA) {
            if (locked) {
                stats->metadata_locked++;
            } else {
                stats->metadata_jobs++;
            }
            stats->metadata_bytes += (unsigned long long)st.st_size;
            const char *counter_suffix = locked ? ".pdf.job.lock" : ".pdf.job";
            if (!jq_check_counterpart(dir_path, name, suffix, counter_suffix)) {
                stats->orphan_metadata++;
            }
        } else if (file_type == JQ_FILE_REPORT) {
            if (locked) {
                stats->report_locked++;
            } else {
                stats->report_jobs++;
            }
            stats->report_bytes += (unsigned long long)st.st_size;
            const char *counter_suffix = locked ? ".pdf.job.lock" : ".pdf.job";
            if (!jq_check_counterpart(dir_path, name, suffix, counter_suffix)) {
                stats->orphan_report++;
            }
        }
    }

    closedir(dir);
    return JQ_OK;
}

static jq_result_t jq_claim_in_dir(const char *root_path,
                                   const char *dir_name,
                                   jq_state_t state,
                                   char *uuid_out,
                                   size_t uuid_out_len) {
    if (!root_path || !dir_name || !uuid_out || uuid_out_len == 0) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    char dir_path[PATH_MAX];
    int written = snprintf(dir_path, sizeof(dir_path), "%s/%s", root_path, dir_name);
    if (written < 0 || (size_t)written >= sizeof(dir_path)) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return errno == ENOENT ? JQ_ERR_NOT_FOUND : JQ_ERR_IO;
    }

    struct dirent *entry;
    jq_result_t result = JQ_ERR_NOT_FOUND;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (!jq_has_suffix(name, ".pdf.job")) {
            continue;
        }
        if (jq_has_suffix(name, ".pdf.job.lock")) {
            continue;
        }

        size_t base_len = strlen(name) - strlen(".pdf.job");
        if (base_len + 1 > uuid_out_len) {
            result = JQ_ERR_INVALID_ARGUMENT;
            break;
        }

        memcpy(uuid_out, name, base_len);
        uuid_out[base_len] = '\0';

        char pdf_src[PATH_MAX];
        char metadata_src[PATH_MAX];
        char pdf_locked[PATH_MAX];
        char metadata_locked[PATH_MAX];

        jq_result_t src_paths =
            jq_job_paths(root_path, uuid_out, state, pdf_src, sizeof(pdf_src), metadata_src, sizeof(metadata_src));
        if (src_paths != JQ_OK) {
            result = src_paths;
            break;
        }

        jq_result_t locked_paths = jq_job_paths_locked(root_path, uuid_out, state,
                                                       pdf_locked, sizeof(pdf_locked),
                                                       metadata_locked, sizeof(metadata_locked));
        if (locked_paths != JQ_OK) {
            result = locked_paths;
            break;
        }

        if (access(metadata_src, F_OK) != 0) {
            continue;
        }

        jq_result_t pdf_lock = jq_rename(pdf_src, pdf_locked);
        if (pdf_lock != JQ_OK) {
            result = pdf_lock;
            break;
        }

        jq_result_t metadata_lock = jq_rename(metadata_src, metadata_locked);
        if (metadata_lock != JQ_OK) {
            jq_rename(pdf_locked, pdf_src);
            result = metadata_lock;
            break;
        }

        result = JQ_OK;
        break;
    }

    closedir(dir);
    return result;
}

jq_result_t jq_claim_next(const char *root_path,
                          int prefer_priority,
                          char *uuid_out,
                          size_t uuid_out_len,
                          jq_state_t *state_out) {
    if (!root_path || !uuid_out || uuid_out_len == 0 || !state_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    const char *first_dir = prefer_priority ? "priority_jobs" : "jobs";
    const char *second_dir = prefer_priority ? "jobs" : "priority_jobs";
    jq_state_t first_state = prefer_priority ? JQ_STATE_PRIORITY : JQ_STATE_JOBS;
    jq_state_t second_state = prefer_priority ? JQ_STATE_JOBS : JQ_STATE_PRIORITY;

    jq_result_t first_result = jq_claim_in_dir(root_path, first_dir, first_state, uuid_out, uuid_out_len);
    if (first_result == JQ_OK) {
        *state_out = first_state;
        return JQ_OK;
    }

    if (first_result != JQ_ERR_NOT_FOUND) {
        return first_result;
    }

    jq_result_t second_result = jq_claim_in_dir(root_path, second_dir, second_state, uuid_out, uuid_out_len);
    if (second_result == JQ_OK) {
        *state_out = second_state;
        return JQ_OK;
    }

    return second_result;
}

jq_result_t jq_release(const char *root_path,
                       const char *uuid,
                       jq_state_t state) {
    if (!root_path || !uuid) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    jq_result_t ensure_result = jq_ensure_state_dir(root_path, state);
    if (ensure_result != JQ_OK) {
        return ensure_result;
    }

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    char report_locked[PATH_MAX];
    char report_dest[PATH_MAX];

    jq_result_t locked_result = jq_job_paths_locked(root_path, uuid, state,
                                                    pdf_locked, sizeof(pdf_locked),
                                                    metadata_locked, sizeof(metadata_locked));
    if (locked_result != JQ_OK) {
        return locked_result;
    }

    jq_result_t dest_result = jq_job_paths(root_path, uuid, state,
                                           pdf_dest, sizeof(pdf_dest),
                                           metadata_dest, sizeof(metadata_dest));
    if (dest_result != JQ_OK) {
        return dest_result;
    }

    jq_result_t report_locked_result =
        jq_job_report_paths_locked(root_path, uuid, state, report_locked, sizeof(report_locked));
    if (report_locked_result != JQ_OK) {
        return report_locked_result;
    }

    jq_result_t report_dest_result =
        jq_job_report_paths(root_path, uuid, state, report_dest, sizeof(report_dest));
    if (report_dest_result != JQ_OK) {
        return report_dest_result;
    }

    jq_result_t pdf_release = jq_rename(pdf_locked, pdf_dest);
    if (pdf_release != JQ_OK) {
        return pdf_release;
    }

    jq_result_t metadata_release = jq_rename(metadata_locked, metadata_dest);
    if (metadata_release != JQ_OK) {
        jq_rename(pdf_dest, pdf_locked);
        return metadata_release;
    }

    jq_result_t report_release = jq_move_report_if_present(report_locked, report_dest);
    if (report_release != JQ_OK) {
        jq_rename(metadata_dest, metadata_locked);
        jq_rename(pdf_dest, pdf_locked);
        return report_release;
    }

    return JQ_OK;
}

jq_result_t jq_finalize(const char *root_path,
                        const char *uuid,
                        jq_state_t from_state,
                        jq_state_t to_state) {
    if (!root_path || !uuid) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    jq_result_t ensure_result = jq_ensure_state_dir(root_path, to_state);
    if (ensure_result != JQ_OK) {
        return ensure_result;
    }

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];
    char report_locked[PATH_MAX];
    char report_dest[PATH_MAX];

    jq_result_t locked_result = jq_job_paths_locked(root_path, uuid, from_state,
                                                    pdf_locked, sizeof(pdf_locked),
                                                    metadata_locked, sizeof(metadata_locked));
    if (locked_result != JQ_OK) {
        return locked_result;
    }

    jq_result_t dest_result = jq_job_paths(root_path, uuid, to_state,
                                           pdf_dest, sizeof(pdf_dest),
                                           metadata_dest, sizeof(metadata_dest));
    if (dest_result != JQ_OK) {
        return dest_result;
    }

    jq_result_t report_locked_result =
        jq_job_report_paths_locked(root_path, uuid, from_state, report_locked, sizeof(report_locked));
    if (report_locked_result != JQ_OK) {
        return report_locked_result;
    }

    jq_result_t report_dest_result =
        jq_job_report_paths(root_path, uuid, to_state, report_dest, sizeof(report_dest));
    if (report_dest_result != JQ_OK) {
        return report_dest_result;
    }

    jq_result_t pdf_move = jq_rename(pdf_locked, pdf_dest);
    if (pdf_move != JQ_OK) {
        return pdf_move;
    }

    jq_result_t metadata_move = jq_rename(metadata_locked, metadata_dest);
    if (metadata_move != JQ_OK) {
        jq_rename(pdf_dest, pdf_locked);
        return metadata_move;
    }

    jq_result_t report_move = jq_move_report_if_present(report_locked, report_dest);
    if (report_move != JQ_OK) {
        jq_rename(metadata_dest, metadata_locked);
        jq_rename(pdf_dest, pdf_locked);
        return report_move;
    }

    return JQ_OK;
}

jq_result_t jq_collect_stats(const char *root_path,
                             jq_stats_t *stats_out) {
    if (!root_path || !stats_out) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    memset(stats_out, 0, sizeof(*stats_out));

    const jq_state_t states[] = {JQ_STATE_JOBS, JQ_STATE_PRIORITY, JQ_STATE_COMPLETE, JQ_STATE_ERROR};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        jq_result_t result = jq_collect_state_stats(root_path,
                                                    states[i],
                                                    &stats_out->states[states[i]],
                                                    &stats_out->oldest_mtime,
                                                    &stats_out->newest_mtime);
        if (result != JQ_OK) {
            return result;
        }
    }

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        const jq_state_stats_t *state_stats = &stats_out->states[states[i]];
        stats_out->total_jobs += state_stats->pdf_jobs + state_stats->metadata_jobs + state_stats->report_jobs;
        stats_out->total_locked += state_stats->pdf_locked + state_stats->metadata_locked + state_stats->report_locked;
        stats_out->total_orphans += state_stats->orphan_pdf + state_stats->orphan_metadata + state_stats->orphan_report;
        stats_out->total_bytes += state_stats->pdf_bytes + state_stats->metadata_bytes + state_stats->report_bytes;
    }

    return JQ_OK;
}
