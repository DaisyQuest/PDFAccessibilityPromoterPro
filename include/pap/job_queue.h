#ifndef PAP_JOB_QUEUE_H
#define PAP_JOB_QUEUE_H

#include <stddef.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JQ_OK = 0,
    JQ_ERR_INVALID_ARGUMENT = -1,
    JQ_ERR_IO = -2,
    JQ_ERR_NOT_FOUND = -3
} jq_result_t;

typedef enum {
    JQ_STATE_JOBS = 0,
    JQ_STATE_PRIORITY = 1,
    JQ_STATE_COMPLETE = 2,
    JQ_STATE_ERROR = 3
} jq_state_t;

typedef struct {
    size_t pdf_jobs;
    size_t metadata_jobs;
    size_t report_jobs;
    size_t pdf_locked;
    size_t metadata_locked;
    size_t report_locked;
    size_t orphan_pdf;
    size_t orphan_metadata;
    size_t orphan_report;
    unsigned long long pdf_bytes;
    unsigned long long metadata_bytes;
    unsigned long long report_bytes;
} jq_state_stats_t;

typedef struct {
    jq_state_stats_t states[JQ_STATE_ERROR + 1];
    size_t total_jobs;
    size_t total_locked;
    size_t total_orphans;
    unsigned long long total_bytes;
    time_t oldest_mtime;
    time_t newest_mtime;
} jq_stats_t;

jq_result_t jq_init(const char *root_path);

jq_result_t jq_submit(const char *root_path,
                      const char *uuid,
                      const char *pdf_path,
                      const char *metadata_path,
                      int priority);

jq_result_t jq_move(const char *root_path,
                    const char *uuid,
                    jq_state_t from_state,
                    jq_state_t to_state);

jq_result_t jq_claim_next(const char *root_path,
                          int prefer_priority,
                          char *uuid_out,
                          size_t uuid_out_len,
                          jq_state_t *state_out);

jq_result_t jq_release(const char *root_path,
                       const char *uuid,
                       jq_state_t state);

jq_result_t jq_finalize(const char *root_path,
                        const char *uuid,
                        jq_state_t from_state,
                        jq_state_t to_state);

jq_result_t jq_status(const char *root_path,
                      const char *uuid,
                      jq_state_t *state_out,
                      int *locked_out);

jq_result_t jq_job_paths(const char *root_path,
                         const char *uuid,
                         jq_state_t state,
                         char *pdf_out,
                         size_t pdf_out_len,
                         char *metadata_out,
                         size_t metadata_out_len);

jq_result_t jq_job_paths_locked(const char *root_path,
                                const char *uuid,
                                jq_state_t state,
                                char *pdf_out,
                                size_t pdf_out_len,
                                char *metadata_out,
                                size_t metadata_out_len);

jq_result_t jq_job_report_paths(const char *root_path,
                                const char *uuid,
                                jq_state_t state,
                                char *report_out,
                                size_t report_out_len);

jq_result_t jq_job_report_paths_locked(const char *root_path,
                                       const char *uuid,
                                       jq_state_t state,
                                       char *report_out,
                                       size_t report_out_len);

jq_result_t jq_collect_stats(const char *root_path,
                             jq_stats_t *stats_out);

#ifdef __cplusplus
}
#endif

#endif
