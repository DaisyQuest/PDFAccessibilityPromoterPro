#include "pap/job_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        return errno == ENOENT ? JQ_ERR_NOT_FOUND : JQ_ERR_IO;
    }

    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        return JQ_ERR_IO;
    }

    char buffer[1024 * 1024];
    ssize_t bytes_read;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = 0;
        while (bytes_written < bytes_read) {
            ssize_t chunk = write(dst_fd, buffer + bytes_written, bytes_read - bytes_written);
            if (chunk < 0) {
                close(src_fd);
                close(dst_fd);
                return JQ_ERR_IO;
            }
            bytes_written += chunk;
        }
    }

    if (bytes_read < 0) {
        close(src_fd);
        close(dst_fd);
        return JQ_ERR_IO;
    }

    close(src_fd);
    close(dst_fd);
    return JQ_OK;
}

static jq_result_t jq_job_paths_locked(const char *root_path,
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

jq_result_t jq_move(const char *root_path,
                    const char *uuid,
                    jq_state_t from_state,
                    jq_state_t to_state) {
    if (!root_path || !uuid) {
        return JQ_ERR_INVALID_ARGUMENT;
    }

    char pdf_src[PATH_MAX];
    char metadata_src[PATH_MAX];
    char pdf_dst[PATH_MAX];
    char metadata_dst[PATH_MAX];

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

    jq_result_t pdf_move = jq_rename(pdf_src, pdf_dst);
    if (pdf_move != JQ_OK) {
        return pdf_move;
    }

    jq_result_t metadata_move = jq_rename(metadata_src, metadata_dst);
    if (metadata_move != JQ_OK) {
        jq_rename(pdf_dst, pdf_src);
        return metadata_move;
    }

    return JQ_OK;
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

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];

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

    jq_result_t pdf_release = jq_rename(pdf_locked, pdf_dest);
    if (pdf_release != JQ_OK) {
        return pdf_release;
    }

    jq_result_t metadata_release = jq_rename(metadata_locked, metadata_dest);
    if (metadata_release != JQ_OK) {
        jq_rename(pdf_dest, pdf_locked);
        return metadata_release;
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

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    char pdf_dest[PATH_MAX];
    char metadata_dest[PATH_MAX];

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

    jq_result_t pdf_move = jq_rename(pdf_locked, pdf_dest);
    if (pdf_move != JQ_OK) {
        return pdf_move;
    }

    jq_result_t metadata_move = jq_rename(metadata_locked, metadata_dest);
    if (metadata_move != JQ_OK) {
        jq_rename(pdf_dest, pdf_locked);
        return metadata_move;
    }

    return JQ_OK;
}
