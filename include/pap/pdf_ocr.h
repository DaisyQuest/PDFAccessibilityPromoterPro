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
    POCR_ERR_NOT_FOUND = -5
} pocr_result_t;

typedef struct {
    int pdf_version_major;
    int pdf_version_minor;
    size_t bytes_scanned;
} pocr_report_t;

pocr_result_t pocr_report_init(pocr_report_t *report);

pocr_result_t pocr_scan_file(const char *path, pocr_report_t *report);

pocr_result_t pocr_report_to_json(const pocr_report_t *report,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out);

const char *pocr_result_str(pocr_result_t result);

#ifdef __cplusplus
}
#endif

#endif
