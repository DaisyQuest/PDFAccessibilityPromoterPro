#ifndef PAP_PDF_OCR_H
#define PAP_PDF_OCR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POCR_OK = 0,
    POCR_ERR_INVALID_ARGUMENT = -1,
    POCR_ERR_IO = -2,
    POCR_ERR_PARSE = -3,
    POCR_ERR_BUFFER_TOO_SMALL = -4,
    POCR_ERR_NOT_FOUND = -5,
    POCR_ERR_PROVIDER_NOT_FOUND = -6,
    POCR_ERR_PROVIDER_EXISTS = -7,
    POCR_ERR_PROVIDER_LIMIT = -8
} pocr_result_t;

typedef struct {
    int pdf_version_major;
    int pdf_version_minor;
    size_t bytes_scanned;
    const char *provider_name;
    size_t handwriting_marker_hits;
    unsigned int handwriting_confidence;
} pocr_report_t;

typedef enum {
    POCR_LOG_DEBUG = 0,
    POCR_LOG_INFO = 1,
    POCR_LOG_WARN = 2,
    POCR_LOG_ERROR = 3
} pocr_log_level_t;

typedef void (*pocr_log_fn)(pocr_log_level_t level, const char *message, void *user_data);

typedef struct {
    const char *name;
    pocr_result_t (*scan_file)(const char *path, pocr_report_t *report, void *user_data);
    void *user_data;
} pocr_provider_t;

pocr_result_t pocr_report_init(pocr_report_t *report);

pocr_result_t pocr_scan_file(const char *path, pocr_report_t *report);

pocr_result_t pocr_scan_file_with_provider(const char *provider_name,
                                           const char *path,
                                           pocr_report_t *report);

pocr_result_t pocr_report_to_json(const pocr_report_t *report,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out);

const char *pocr_result_str(pocr_result_t result);

void pocr_set_logger(pocr_log_fn logger, void *user_data);

const char *pocr_log_level_str(pocr_log_level_t level);

pocr_result_t pocr_register_provider(const pocr_provider_t *provider);

const pocr_provider_t *pocr_find_provider(const char *name);

const pocr_provider_t *pocr_default_provider(void);

size_t pocr_provider_capacity(void);

size_t pocr_provider_count(void);

#ifdef __cplusplus
}
#endif

#endif
