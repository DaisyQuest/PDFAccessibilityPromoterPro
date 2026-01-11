#include "pap/pdf_accessibility.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

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

static pdfa_result_t pdfa_append_html(char *buffer,
                                      size_t buffer_len,
                                      size_t *offset,
                                      const char *format,
                                      ...) {
    if (!buffer || !offset || *offset >= buffer_len) {
        return PDFA_ERR_BUFFER_TOO_SMALL;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *offset, buffer_len - *offset, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= buffer_len - *offset) {
        return PDFA_ERR_BUFFER_TOO_SMALL;
    }
    *offset += (size_t)written;
    return PDFA_OK;
}

pdfa_result_t pdfa_report_to_html(const pdfa_report_t *report,
                                  const char *before_link,
                                  const char *after_link,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out) {
    if (!report || !before_link || !after_link || !buffer || buffer_len == 0) {
        return PDFA_ERR_INVALID_ARGUMENT;
    }

    size_t offset = 0;
    pdfa_result_t result = pdfa_append_html(
        buffer,
        buffer_len,
        &offset,
        "<!doctype html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Accessibility Transformation Report</title>"
        "<style>"
        ":root{color-scheme:light;--bg:#0b1020;--card:#141b33;--accent:#5ce1e6;--accent-2:#9b7bff;--text:#eef2ff;--muted:#a9b4d0;}"
        "body{margin:0;font-family:'Segoe UI',system-ui,sans-serif;background:radial-gradient(circle at top,#1b2550,#0b1020 60%);color:var(--text);}"
        ".hero{padding:48px 32px;text-align:center;}"
        ".hero h1{font-size:36px;margin:0 0 12px;letter-spacing:0.4px;}"
        ".hero p{margin:0;color:var(--muted);}"
        ".grid{display:grid;gap:20px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));padding:0 32px 40px;}"
        ".card{background:var(--card);border-radius:16px;padding:20px;box-shadow:0 12px 24px rgba(6,10,25,0.45);}"
        ".badge{display:inline-block;padding:6px 12px;border-radius:999px;background:rgba(92,225,230,0.15);color:var(--accent);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:1px;}"
        ".stats{display:flex;flex-wrap:wrap;gap:12px;margin-top:12px;color:var(--muted);}"
        ".stat{background:rgba(255,255,255,0.04);border-radius:12px;padding:10px 12px;min-width:120px;}"
        ".link{color:var(--accent);text-decoration:none;font-weight:600;}"
        ".link:hover{text-decoration:underline;}"
        ".issues{margin:0;padding-left:18px;color:var(--muted);}"
        ".footer{padding:0 32px 40px;color:var(--muted);}"
        "</style>"
        "</head>"
        "<body>"
        "<section class=\"hero\">"
        "<span class=\"badge\">PDF Accessibility Promoter Pro</span>"
        "<h1>Accessibility Transformation Report</h1>"
        "<p>Comprehensive visibility into the transformations applied to elevate PDF accessibility.</p>"
        "</section>");
    if (result != PDFA_OK) {
        return result;
    }

    result = pdfa_append_html(
        buffer,
        buffer_len,
        &offset,
        "<section class=\"grid\">"
        "<div class=\"card\">"
        "<h2>Before &amp; After</h2>"
        "<p>Review the source and optimized deliverables:</p>"
        "<p><a class=\"link\" href=\"%s\">Before PDF</a> · <a class=\"link\" href=\"%s\">After PDF</a></p>"
        "</div>"
        "<div class=\"card\">"
        "<h2>Analysis Summary</h2>"
        "<div class=\"stats\">"
        "<div class=\"stat\"><strong>PDF Version</strong><div>%d.%d</div></div>"
        "<div class=\"stat\"><strong>Bytes Scanned</strong><div>%zu</div></div>"
        "<div class=\"stat\"><strong>Total Size</strong><div>%zu</div></div>"
        "</div>"
        "</div>"
        "<div class=\"card\">"
        "<h2>Accessibility Signals</h2>"
        "<div class=\"stats\">"
        "<div class=\"stat\"><strong>Tagged</strong><div>%s</div></div>"
        "<div class=\"stat\"><strong>Language</strong><div>%s</div></div>"
        "<div class=\"stat\"><strong>Title</strong><div>%s</div></div>"
        "<div class=\"stat\"><strong>Alt Text</strong><div>%s</div></div>"
        "</div>"
        "</div>"
        "</section>",
        before_link,
        after_link,
        report->pdf_version_major,
        report->pdf_version_minor,
        report->bytes_scanned,
        report->byte_count,
        report->has_struct_tree_root ? "Present" : "Missing",
        report->has_lang ? "Present" : "Missing",
        report->has_title ? "Present" : "Missing",
        (report->has_alt_text || report->has_actual_text) ? "Present" : "Missing");
    if (result != PDFA_OK) {
        return result;
    }

    result = pdfa_append_html(
        buffer,
        buffer_len,
        &offset,
        "<section class=\"grid\">"
        "<div class=\"card\">"
        "<h2>Outstanding Issues</h2>");
    if (result != PDFA_OK) {
        return result;
    }

    if (report->issue_count == 0) {
        result = pdfa_append_html(buffer, buffer_len, &offset,
                                  "<p>No outstanding accessibility issues detected.</p>");
        if (result != PDFA_OK) {
            return result;
        }
    } else {
        result = pdfa_append_html(buffer, buffer_len, &offset, "<ul class=\"issues\">");
        if (result != PDFA_OK) {
            return result;
        }
        for (size_t i = 0; i < report->issue_count; ++i) {
            const char *name = pdfa_issue_code_name(report->issues[i]);
            result = pdfa_append_html(buffer, buffer_len, &offset, "<li>%s</li>", name);
            if (result != PDFA_OK) {
                return result;
            }
        }
        result = pdfa_append_html(buffer, buffer_len, &offset, "</ul>");
        if (result != PDFA_OK) {
            return result;
        }
    }

    result = pdfa_append_html(
        buffer,
        buffer_len,
        &offset,
        "</div>"
        "<div class=\"card\">"
        "<h2>Transformation Highlights</h2>"
        "<p>Applied improvements include catalog metadata, tagging structure, language declaration, and rich text alternatives.</p>"
        "<p>See <span class=\"badge\">problems_we_correct.md</span> for a full catalog.</p>"
        "</div>"
        "</section>"
        "<div class=\"footer\">Generated by PDF Accessibility Promoter Pro.</div>"
        "</body>"
        "</html>");
    if (result != PDFA_OK) {
        return result;
    }

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
