#ifndef PAP_PDF_REDACTION_H
#define PAP_PDF_REDACTION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDRX_MAX_REDACTIONS 32
#define PDRX_MAX_PATTERN_LEN 128

typedef enum {
    PDRX_OK = 0,
    PDRX_ERR_INVALID_ARGUMENT = -1,
    PDRX_ERR_IO = -2,
    PDRX_ERR_PARSE = -3,
    PDRX_ERR_BUFFER_TOO_SMALL = -4,
    PDRX_ERR_NOT_FOUND = -5
} pdrx_result_t;

typedef struct {
    size_t redaction_count;
    char patterns[PDRX_MAX_REDACTIONS][PDRX_MAX_PATTERN_LEN];
    size_t pattern_lengths[PDRX_MAX_REDACTIONS];
} pdrx_plan_t;

typedef struct {
    int pdf_version_major;
    int pdf_version_minor;
    size_t bytes_redacted;
    size_t match_count;
    size_t bytes_scanned;
} pdrx_report_t;

pdrx_result_t pdrx_plan_init(pdrx_plan_t *plan);

pdrx_result_t pdrx_plan_from_json(const char *json, size_t length, pdrx_plan_t *plan);

pdrx_result_t pdrx_apply_file(const char *input_path,
                              const char *output_path,
                              const pdrx_plan_t *plan,
                              pdrx_report_t *report);

pdrx_result_t pdrx_report_to_json(const pdrx_report_t *report,
                                  const pdrx_plan_t *plan,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out);

const char *pdrx_result_str(pdrx_result_t result);

#ifdef __cplusplus
}
#endif

#endif
