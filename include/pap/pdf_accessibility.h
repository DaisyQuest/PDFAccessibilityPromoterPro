#ifndef PAP_PDF_ACCESSIBILITY_H
#define PAP_PDF_ACCESSIBILITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PDFA_OK = 0,
    PDFA_ERR_INVALID_ARGUMENT,
    PDFA_ERR_NOT_FOUND,
    PDFA_ERR_IO,
    PDFA_ERR_PARSE,
    PDFA_ERR_BUFFER_TOO_SMALL
} pdfa_result_t;

typedef enum {
    PDFA_ISSUE_MISSING_CATALOG = 0,
    PDFA_ISSUE_MISSING_PAGES,
    PDFA_ISSUE_MISSING_OUTLINES,
    PDFA_ISSUE_MISSING_TAGS,
    PDFA_ISSUE_MISSING_LANGUAGE,
    PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES,
    PDFA_ISSUE_MISSING_TITLE,
    PDFA_ISSUE_MISSING_MARKED_CONTENT,
    PDFA_ISSUE_MISSING_DISPLAY_DOC_TITLE,
    PDFA_ISSUE_MISSING_ROLE_MAP,
    PDFA_ISSUE_MISSING_METADATA,
    PDFA_ISSUE_MISSING_MARK_INFO,
    PDFA_ISSUE_MISSING_VIEWER_PREFERENCES,
    PDFA_ISSUE_MISSING_PARENT_TREE,
    PDFA_ISSUE_MISSING_STRUCT_PARENTS,
    PDFA_ISSUE_MISSING_MCID
} pdfa_issue_code_t;

typedef struct {
    int pdf_version_major;
    int pdf_version_minor;
    int has_catalog;
    int has_pages;
    int has_outlines;
    int has_struct_tree_root;
    int has_lang;
    int has_alt_text;
    int has_actual_text;
    int has_title;
    int has_marked_content;
    int has_display_doc_title;
    int has_role_map;
    int has_metadata;
    int has_mark_info;
    int has_viewer_preferences;
    int has_parent_tree;
    int has_struct_parents;
    int has_mcid;
    size_t bytes_scanned;
    size_t byte_count;
    pdfa_issue_code_t issues[20];
    size_t issue_count;
} pdfa_report_t;

enum { PDFA_SCAN_CHUNK_SIZE = 4096 };

pdfa_result_t pdfa_report_init(pdfa_report_t *report);
pdfa_result_t pdfa_analyze_file(const char *path, pdfa_report_t *report);
pdfa_result_t pdfa_report_to_json(const pdfa_report_t *report,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out);
pdfa_result_t pdfa_report_to_html(const pdfa_report_t *report,
                                  const char *before_link,
                                  const char *after_link,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out);
pdfa_result_t pdfa_report_to_html_analysis(const pdfa_report_t *report,
                                           const char *source_link,
                                           char *buffer,
                                           size_t buffer_len,
                                           size_t *written_out);
const char *pdfa_result_str(pdfa_result_t result);

#ifdef __cplusplus
}
#endif

#endif
