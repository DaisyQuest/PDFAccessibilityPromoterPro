#include "pap/pdf_accessibility.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *pdfa_issue_code_name(pdfa_issue_code_t code) {
    switch (code) {
        case PDFA_ISSUE_MISSING_CATALOG:
            return "missing_catalog";
        case PDFA_ISSUE_MISSING_PAGES:
            return "missing_pages";
        case PDFA_ISSUE_MISSING_OUTLINES:
            return "missing_outlines";
        case PDFA_ISSUE_MISSING_TAGS:
            return "missing_tags";
        case PDFA_ISSUE_MISSING_LANGUAGE:
            return "missing_language";
        case PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES:
            return "missing_text_alternatives";
        case PDFA_ISSUE_MISSING_TITLE:
            return "missing_title";
        case PDFA_ISSUE_MISSING_MARKED_CONTENT:
            return "missing_marked_content";
        case PDFA_ISSUE_MISSING_DISPLAY_DOC_TITLE:
            return "missing_display_doc_title";
        case PDFA_ISSUE_MISSING_ROLE_MAP:
            return "missing_role_map";
        case PDFA_ISSUE_MISSING_METADATA:
            return "missing_metadata";
        case PDFA_ISSUE_MISSING_MARK_INFO:
            return "missing_mark_info";
        case PDFA_ISSUE_MISSING_VIEWER_PREFERENCES:
            return "missing_viewer_preferences";
        case PDFA_ISSUE_MISSING_PARENT_TREE:
            return "missing_parent_tree";
        case PDFA_ISSUE_MISSING_STRUCT_PARENTS:
            return "missing_struct_parents";
        case PDFA_ISSUE_MISSING_MCID:
            return "missing_mcid";
        default:
            return "unknown";
    }
}

static void pdfa_note_issue(pdfa_report_t *report, pdfa_issue_code_t code) {
    if (report->issue_count >= sizeof(report->issues) / sizeof(report->issues[0])) {
        return;
    }
    report->issues[report->issue_count++] = code;
}

static void pdfa_track_text_alternatives(pdfa_report_t *report) {
    if (!report->has_alt_text && !report->has_actual_text) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES);
    }
}

static void pdfa_track_catalog(pdfa_report_t *report) {
    if (!report->has_catalog) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_CATALOG);
    }
}

static void pdfa_track_pages(pdfa_report_t *report) {
    if (!report->has_pages) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_PAGES);
    }
}

static void pdfa_track_outlines(pdfa_report_t *report) {
    if (!report->has_outlines) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_OUTLINES);
    }
}

static void pdfa_track_tags(pdfa_report_t *report) {
    if (!report->has_struct_tree_root) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_TAGS);
    }
}

static void pdfa_track_language(pdfa_report_t *report) {
    if (!report->has_lang) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_LANGUAGE);
    }
}

static void pdfa_track_title(pdfa_report_t *report) {
    if (!report->has_title) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_TITLE);
    }
}

static void pdfa_track_marked_content(pdfa_report_t *report) {
    if (!report->has_marked_content) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_MARKED_CONTENT);
    }
}

static void pdfa_track_display_doc_title(pdfa_report_t *report) {
    if (!report->has_display_doc_title) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_DISPLAY_DOC_TITLE);
    }
}

static void pdfa_track_role_map(pdfa_report_t *report) {
    if (!report->has_role_map) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_ROLE_MAP);
    }
}

static void pdfa_track_metadata(pdfa_report_t *report) {
    if (!report->has_metadata) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_METADATA);
    }
}

static void pdfa_track_mark_info(pdfa_report_t *report) {
    if (!report->has_mark_info) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_MARK_INFO);
    }
}

static void pdfa_track_viewer_preferences(pdfa_report_t *report) {
    if (!report->has_viewer_preferences) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_VIEWER_PREFERENCES);
    }
}

static void pdfa_track_parent_tree(pdfa_report_t *report) {
    if (!report->has_parent_tree) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_PARENT_TREE);
    }
}

static void pdfa_track_struct_parents(pdfa_report_t *report) {
    if (!report->has_struct_parents) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_STRUCT_PARENTS);
    }
}

static void pdfa_track_mcid(pdfa_report_t *report) {
    if (report->has_struct_tree_root && !report->has_mcid) {
        pdfa_note_issue(report, PDFA_ISSUE_MISSING_MCID);
    }
}

pdfa_result_t pdfa_report_init(pdfa_report_t *report) {
    if (!report) {
        return PDFA_ERR_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    report->pdf_version_major = -1;
    report->pdf_version_minor = -1;
    return PDFA_OK;
}

static pdfa_result_t pdfa_scan_version_line(FILE *fp, pdfa_report_t *report) {
    char buffer[64];
    size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, fp);
    if (bytes == 0) {
        return PDFA_ERR_PARSE;
    }
    buffer[bytes] = '\0';

    const char *marker = "%PDF-";
    char *found = strstr(buffer, marker);
    if (!found) {
        return PDFA_ERR_PARSE;
    }
    found += strlen(marker);
    int major = -1;
    int minor = -1;
    if (isdigit((unsigned char)found[0]) && found[1] == '.' && isdigit((unsigned char)found[2])) {
        major = found[0] - '0';
        minor = found[2] - '0';
    } else {
        return PDFA_ERR_PARSE;
    }
    report->pdf_version_major = major;
    report->pdf_version_minor = minor;
    return PDFA_OK;
}

static void pdfa_mark_token(const char *token, size_t token_len, pdfa_report_t *report) {
    if (token_len == 0) {
        return;
    }
    if (strncmp(token, "/Type", token_len) == 0) {
        return;
    }
    if (strncmp(token, "/Catalog", token_len) == 0) {
        report->has_catalog = 1;
    } else if (strncmp(token, "/Pages", token_len) == 0) {
        report->has_pages = 1;
    } else if (strncmp(token, "/Outlines", token_len) == 0) {
        report->has_outlines = 1;
    } else if (strncmp(token, "/StructTreeRoot", token_len) == 0) {
        report->has_struct_tree_root = 1;
    } else if (strncmp(token, "/Lang", token_len) == 0) {
        return;
    } else if (strncmp(token, "/Alt", token_len) == 0) {
        return;
    } else if (strncmp(token, "/ActualText", token_len) == 0) {
        return;
    } else if (strncmp(token, "/Title", token_len) == 0) {
        return;
    } else if (strncmp(token, "/RoleMap", token_len) == 0) {
        report->has_role_map = 1;
    } else if (strncmp(token, "/Metadata", token_len) == 0) {
        report->has_metadata = 1;
    } else if (strncmp(token, "/MarkInfo", token_len) == 0) {
        report->has_mark_info = 1;
    } else if (strncmp(token, "/ViewerPreferences", token_len) == 0) {
        report->has_viewer_preferences = 1;
    } else if (strncmp(token, "/ParentTree", token_len) == 0) {
        report->has_parent_tree = 1;
    } else if (strncmp(token, "/StructParents", token_len) == 0) {
        report->has_struct_parents = 1;
    } else if (strncmp(token, "/MCID", token_len) == 0) {
        report->has_mcid = 1;
    }
}

typedef enum {
    PDFA_PENDING_NONE = 0,
    PDFA_PENDING_MARKED,
    PDFA_PENDING_DISPLAY_DOC_TITLE,
    PDFA_PENDING_LANG_VALUE,
    PDFA_PENDING_TITLE_VALUE,
    PDFA_PENDING_ALT_VALUE,
    PDFA_PENDING_ACTUAL_TEXT_VALUE
} pdfa_pending_key_t;

static int pdfa_is_value_start(int c) {
    if (c == '/' || c == '(' || c == '<') {
        return 1;
    }
    return isalnum((unsigned char)c) ? 1 : 0;
}

static void pdfa_set_value_flag(pdfa_report_t *report, pdfa_pending_key_t pending) {
    if (!report) {
        return;
    }
    switch (pending) {
        case PDFA_PENDING_LANG_VALUE:
            report->has_lang = 1;
            break;
        case PDFA_PENDING_TITLE_VALUE:
            report->has_title = 1;
            break;
        case PDFA_PENDING_ALT_VALUE:
            report->has_alt_text = 1;
            break;
        case PDFA_PENDING_ACTUAL_TEXT_VALUE:
            report->has_actual_text = 1;
            break;
        default:
            break;
    }
}

static void pdfa_mark_keyword(const char *token, size_t token_len, pdfa_report_t *report, pdfa_pending_key_t *pending) {
    if (token_len == 0 || !pending) {
        return;
    }
    if (*pending == PDFA_PENDING_MARKED) {
        if (token_len == 4 && strncmp(token, "true", token_len) == 0) {
            report->has_marked_content = 1;
        }
        *pending = PDFA_PENDING_NONE;
        return;
    }
    if (*pending == PDFA_PENDING_DISPLAY_DOC_TITLE) {
        if (token_len == 4 && strncmp(token, "true", token_len) == 0) {
            report->has_display_doc_title = 1;
        }
        *pending = PDFA_PENDING_NONE;
        return;
    }
}

static void pdfa_note_pending_key(const char *token, size_t token_len, pdfa_pending_key_t *pending) {
    if (!pending || token_len == 0) {
        return;
    }
    if (strncmp(token, "/Marked", token_len) == 0) {
        *pending = PDFA_PENDING_MARKED;
    } else if (strncmp(token, "/DisplayDocTitle", token_len) == 0) {
        *pending = PDFA_PENDING_DISPLAY_DOC_TITLE;
    } else if (strncmp(token, "/Lang", token_len) == 0) {
        *pending = PDFA_PENDING_LANG_VALUE;
    } else if (strncmp(token, "/Title", token_len) == 0) {
        *pending = PDFA_PENDING_TITLE_VALUE;
    } else if (strncmp(token, "/Alt", token_len) == 0) {
        *pending = PDFA_PENDING_ALT_VALUE;
    } else if (strncmp(token, "/ActualText", token_len) == 0) {
        *pending = PDFA_PENDING_ACTUAL_TEXT_VALUE;
    }
}

static void pdfa_scan_tokens(FILE *fp, pdfa_report_t *report) {
    char buffer[PDFA_SCAN_CHUNK_SIZE + 1];
    char token[128];
    size_t token_len = 0;
    int token_is_name = 0;
    pdfa_pending_key_t pending = PDFA_PENDING_NONE;

    while (!feof(fp)) {
        size_t bytes = fread(buffer, 1, PDFA_SCAN_CHUNK_SIZE, fp);
        if (bytes == 0) {
            break;
        }
        report->bytes_scanned += bytes;
        buffer[bytes] = '\0';

        for (size_t i = 0; i < bytes; ++i) {
            char c = buffer[i];
            if (pending == PDFA_PENDING_LANG_VALUE ||
                pending == PDFA_PENDING_TITLE_VALUE ||
                pending == PDFA_PENDING_ALT_VALUE ||
                pending == PDFA_PENDING_ACTUAL_TEXT_VALUE) {
                if (pdfa_is_value_start((unsigned char)c)) {
                    pdfa_set_value_flag(report, pending);
                    pending = PDFA_PENDING_NONE;
                }
            }
            if (c == '/') {
                if (token_len > 0) {
                    if (token_is_name) {
                        pdfa_mark_token(token, token_len, report);
                        pdfa_note_pending_key(token, token_len, &pending);
                    } else {
                        pdfa_mark_keyword(token, token_len, report, &pending);
                    }
                    token_len = 0;
                    token_is_name = 0;
                }
                token[token_len++] = c;
                token_is_name = 1;
                continue;
            }
            if (token_len > 0) {
                if (token_is_name) {
                    if (isalnum((unsigned char)c) || c == '#') {
                        if (token_len < sizeof(token) - 1) {
                            token[token_len++] = c;
                        }
                    } else {
                        token[token_len] = '\0';
                        pdfa_mark_token(token, token_len, report);
                        pdfa_note_pending_key(token, token_len, &pending);
                        token_len = 0;
                        token_is_name = 0;
                    }
                } else if (isalpha((unsigned char)c)) {
                    if (token_len < sizeof(token) - 1) {
                        token[token_len++] = c;
                    }
                } else {
                    token[token_len] = '\0';
                    pdfa_mark_keyword(token, token_len, report, &pending);
                    token_len = 0;
                }
            } else if (isalpha((unsigned char)c)) {
                token[token_len++] = c;
                token_is_name = 0;
            }
        }
        if (token_len >= sizeof(token) - 1) {
            token[token_len] = '\0';
            if (token_is_name) {
                pdfa_mark_token(token, token_len, report);
                pdfa_note_pending_key(token, token_len, &pending);
            } else {
                pdfa_mark_keyword(token, token_len, report, &pending);
            }
            token_len = 0;
            token_is_name = 0;
        }
    }
    if (token_len > 0) {
        token[token_len] = '\0';
        if (token_is_name) {
            pdfa_mark_token(token, token_len, report);
            pdfa_note_pending_key(token, token_len, &pending);
        } else {
            pdfa_mark_keyword(token, token_len, report, &pending);
        }
    }
}

static void pdfa_finalize_report(pdfa_report_t *report) {
    report->issue_count = 0;
    pdfa_track_catalog(report);
    pdfa_track_pages(report);
    pdfa_track_outlines(report);
    pdfa_track_tags(report);
    pdfa_track_language(report);
    pdfa_track_text_alternatives(report);
    pdfa_track_title(report);
    pdfa_track_marked_content(report);
    pdfa_track_display_doc_title(report);
    pdfa_track_role_map(report);
    pdfa_track_metadata(report);
    pdfa_track_mark_info(report);
    pdfa_track_viewer_preferences(report);
    pdfa_track_parent_tree(report);
    pdfa_track_struct_parents(report);
    pdfa_track_mcid(report);
}

pdfa_result_t pdfa_analyze_file(const char *path, pdfa_report_t *report) {
    if (!path || !report) {
        return PDFA_ERR_INVALID_ARGUMENT;
    }

    pdfa_result_t init_result = pdfa_report_init(report);
    if (init_result != PDFA_OK) {
        return init_result;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return errno == ENOENT ? PDFA_ERR_NOT_FOUND : PDFA_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size >= 0) {
            report->byte_count = (size_t)size;
        }
        rewind(fp);
    }

    pdfa_result_t version_result = pdfa_scan_version_line(fp, report);
    if (version_result != PDFA_OK) {
        fclose(fp);
        return version_result;
    }

    rewind(fp);
    pdfa_scan_tokens(fp, report);
    fclose(fp);

    pdfa_finalize_report(report);
    return PDFA_OK;
}

pdfa_result_t pdfa_report_to_json(const pdfa_report_t *report,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out) {
    if (!report || !buffer || buffer_len == 0) {
        return PDFA_ERR_INVALID_ARGUMENT;
    }

    size_t offset = 0;
    int written = snprintf(buffer + offset, buffer_len - offset,
                           "{"
                           "\"pdf_version\":\"%d.%d\","
                           "\"bytes_scanned\":%zu,"
                           "\"byte_count\":%zu,"
                           "\"has_catalog\":%s,"
                           "\"has_pages\":%s,"
                           "\"has_outlines\":%s,"
                           "\"has_struct_tree_root\":%s,"
                           "\"has_lang\":%s,"
                           "\"has_alt_text\":%s,"
                           "\"has_actual_text\":%s,"
                           "\"has_title\":%s,"
                           "\"has_marked_content\":%s,"
                           "\"has_display_doc_title\":%s,"
                           "\"has_role_map\":%s,"
                           "\"has_metadata\":%s,"
                           "\"has_mark_info\":%s,"
                           "\"has_viewer_preferences\":%s,"
                           "\"has_parent_tree\":%s,"
                           "\"has_struct_parents\":%s,"
                           "\"has_mcid\":%s,"
                           "\"issues\":[",
                           report->pdf_version_major,
                           report->pdf_version_minor,
                           report->bytes_scanned,
                           report->byte_count,
                           report->has_catalog ? "true" : "false",
                           report->has_pages ? "true" : "false",
                           report->has_outlines ? "true" : "false",
                           report->has_struct_tree_root ? "true" : "false",
                           report->has_lang ? "true" : "false",
                           report->has_alt_text ? "true" : "false",
                           report->has_actual_text ? "true" : "false",
                           report->has_title ? "true" : "false",
                           report->has_marked_content ? "true" : "false",
                           report->has_display_doc_title ? "true" : "false",
                           report->has_role_map ? "true" : "false",
                           report->has_metadata ? "true" : "false",
                           report->has_mark_info ? "true" : "false",
                           report->has_viewer_preferences ? "true" : "false",
                           report->has_parent_tree ? "true" : "false",
                           report->has_struct_parents ? "true" : "false",
                           report->has_mcid ? "true" : "false");
    if (written < 0 || (size_t)written >= buffer_len) {
        return PDFA_ERR_BUFFER_TOO_SMALL;
    }
    offset += (size_t)written;

    for (size_t i = 0; i < report->issue_count; ++i) {
        const char *name = pdfa_issue_code_name(report->issues[i]);
        written = snprintf(buffer + offset, buffer_len - offset, "%s\"%s\"",
                           i == 0 ? "" : ",", name);
        if (written < 0 || (size_t)written >= buffer_len) {
            return PDFA_ERR_BUFFER_TOO_SMALL;
        }
        offset += (size_t)written;
    }

    written = snprintf(buffer + offset, buffer_len - offset, "]}");
    if (written < 0 || (size_t)written >= buffer_len) {
        return PDFA_ERR_BUFFER_TOO_SMALL;
    }
    offset += (size_t)written;

    if (written_out) {
        *written_out = offset;
    }
    return PDFA_OK;
}

const char *pdfa_result_str(pdfa_result_t result) {
    switch (result) {
        case PDFA_OK:
            return "ok";
        case PDFA_ERR_INVALID_ARGUMENT:
            return "invalid_argument";
        case PDFA_ERR_NOT_FOUND:
            return "not_found";
        case PDFA_ERR_IO:
            return "io_error";
        case PDFA_ERR_PARSE:
            return "parse_error";
        case PDFA_ERR_BUFFER_TOO_SMALL:
            return "buffer_too_small";
        default:
            return "unknown";
    }
}
