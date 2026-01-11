#include "pap/pdf_accessibility.h"

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

static int write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    fputs(contents, fp);
    fclose(fp);
    return 1;
}

static int write_buffer(const char *path, const char *contents, size_t length) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    if (fwrite(contents, 1, length, fp) != length) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
}

static int append_text(char *buffer, size_t buffer_len, size_t *offset, const char *text) {
    size_t length = strlen(text);
    if (*offset + length >= buffer_len) {
        return 0;
    }
    memcpy(buffer + *offset, text, length);
    *offset += length;
    return 1;
}

static int append_padding(char *buffer, size_t buffer_len, size_t *offset, size_t count, char fill) {
    if (*offset + count >= buffer_len) {
        return 0;
    }
    memset(buffer + *offset, fill, count);
    *offset += count;
    return 1;
}

static int test_report_init_invalid(void) {
    return assert_true(pdfa_report_init(NULL) == PDFA_ERR_INVALID_ARGUMENT,
                       "pdfa_report_init should reject NULL");
}

static int test_analyze_invalid(void) {
    pdfa_report_t report;
    return assert_true(pdfa_analyze_file(NULL, &report) == PDFA_ERR_INVALID_ARGUMENT,
                       "pdfa_analyze_file should reject NULL path") &&
           assert_true(pdfa_analyze_file("file.pdf", NULL) == PDFA_ERR_INVALID_ARGUMENT,
                       "pdfa_analyze_file should reject NULL report");
}

static int test_analyze_missing_file(void) {
    pdfa_report_t report;
    return assert_true(pdfa_analyze_file("missing.pdf", &report) == PDFA_ERR_NOT_FOUND,
                       "pdfa_analyze_file should report missing file");
}

static int test_analyze_parse_error(void) {
    char template[] = "/tmp/pap_pdfa_parse_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/bad.pdf", root);
    if (!assert_true(write_file(path, "NOTPDF"), "write bad pdf")) {
        return 0;
    }

    pdfa_report_t report;
    return assert_true(pdfa_analyze_file(path, &report) == PDFA_ERR_PARSE,
                       "should detect parse error");
}

static int test_analyze_complete_pdf(void) {
    char template[] = "/tmp/pap_pdfa_complete_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/complete.pdf", root);
    const char *contents =
        "%PDF-1.7\n"
        "1 0 obj\n"
        "<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R /StructTreeRoot 4 0 R /Lang (en-US) >>\n"
        "endobj\n"
        "<< /Alt (alt text) /ActualText (actual text) >>\n"
        "<< /Marked true /DisplayDocTitle true /Title (Document Title) /RoleMap <<>> /Metadata 5 0 R >>\n"
        "<< /MarkInfo << /Marked true >> /ViewerPreferences << /DisplayDocTitle true >> /ParentTree 6 0 R >>\n"
        "<< /StructParents 1 /MCID 0 >>\n";
    if (!assert_true(write_file(path, contents), "write complete pdf")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze complete pdf")) {
        return 0;
    }

    return assert_true(report.pdf_version_major == 1 && report.pdf_version_minor == 7, "version parsed") &&
           assert_true(report.has_catalog, "catalog present") &&
           assert_true(report.has_pages, "pages present") &&
           assert_true(report.has_outlines, "outlines present") &&
           assert_true(report.has_struct_tree_root, "tags present") &&
           assert_true(report.has_lang, "lang present") &&
           assert_true(report.has_alt_text, "alt present") &&
           assert_true(report.has_actual_text, "actual text present") &&
           assert_true(report.has_title, "title present") &&
           assert_true(report.has_marked_content, "marked content present") &&
           assert_true(report.has_display_doc_title, "display doc title present") &&
           assert_true(report.has_role_map, "role map present") &&
           assert_true(report.has_metadata, "metadata present") &&
           assert_true(report.has_mark_info, "mark info present") &&
           assert_true(report.has_viewer_preferences, "viewer preferences present") &&
           assert_true(report.has_parent_tree, "parent tree present") &&
           assert_true(report.has_struct_parents, "struct parents present") &&
           assert_true(report.has_mcid, "mcid present") &&
           assert_true(report.issue_count == 0, "no issues");
}

static int test_analyze_missing_features(void) {
    char template[] = "/tmp/pap_pdfa_missing_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/missing.pdf", root);
    const char *contents =
        "%PDF-1.4\n"
        "1 0 obj\n"
        "<< /Type /Catalog /Pages 2 0 R >>\n"
        "endobj\n";
    if (!assert_true(write_file(path, contents), "write minimal pdf")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze missing pdf")) {
        return 0;
    }

    int has_missing_tags = 0;
    int has_missing_lang = 0;
    int has_missing_outlines = 0;
    int has_missing_alt = 0;
    int has_missing_title = 0;
    int has_missing_marked = 0;
    int has_missing_display_doc_title = 0;
    int has_missing_role_map = 0;
    int has_missing_metadata = 0;
    int has_missing_mark_info = 0;
    int has_missing_viewer_preferences = 0;
    int has_missing_parent_tree = 0;
    int has_missing_struct_parents = 0;
    int has_missing_mcid = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        switch (report.issues[i]) {
            case PDFA_ISSUE_MISSING_TAGS:
                has_missing_tags = 1;
                break;
            case PDFA_ISSUE_MISSING_LANGUAGE:
                has_missing_lang = 1;
                break;
            case PDFA_ISSUE_MISSING_OUTLINES:
                has_missing_outlines = 1;
                break;
            case PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES:
                has_missing_alt = 1;
                break;
            case PDFA_ISSUE_MISSING_TITLE:
                has_missing_title = 1;
                break;
            case PDFA_ISSUE_MISSING_MARKED_CONTENT:
                has_missing_marked = 1;
                break;
            case PDFA_ISSUE_MISSING_DISPLAY_DOC_TITLE:
                has_missing_display_doc_title = 1;
                break;
            case PDFA_ISSUE_MISSING_ROLE_MAP:
                has_missing_role_map = 1;
                break;
            case PDFA_ISSUE_MISSING_METADATA:
                has_missing_metadata = 1;
                break;
            case PDFA_ISSUE_MISSING_MARK_INFO:
                has_missing_mark_info = 1;
                break;
            case PDFA_ISSUE_MISSING_VIEWER_PREFERENCES:
                has_missing_viewer_preferences = 1;
                break;
            case PDFA_ISSUE_MISSING_PARENT_TREE:
                has_missing_parent_tree = 1;
                break;
            case PDFA_ISSUE_MISSING_STRUCT_PARENTS:
                has_missing_struct_parents = 1;
                break;
            case PDFA_ISSUE_MISSING_MCID:
                has_missing_mcid = 1;
                break;
            default:
                break;
        }
    }

    return assert_true(report.has_catalog, "catalog present") &&
           assert_true(report.has_pages, "pages present") &&
           assert_true(has_missing_tags, "missing tags issue") &&
           assert_true(has_missing_lang, "missing lang issue") &&
           assert_true(has_missing_outlines, "missing outlines issue") &&
           assert_true(has_missing_alt, "missing alt issue") &&
           assert_true(has_missing_title, "missing title issue") &&
           assert_true(has_missing_marked, "missing marked issue") &&
           assert_true(has_missing_display_doc_title, "missing display doc title issue") &&
           assert_true(has_missing_role_map, "missing role map issue") &&
           assert_true(has_missing_metadata, "missing metadata issue") &&
           assert_true(has_missing_mark_info, "missing mark info issue") &&
           assert_true(has_missing_viewer_preferences, "missing viewer preferences issue") &&
           assert_true(has_missing_parent_tree, "missing parent tree issue") &&
           assert_true(has_missing_struct_parents, "missing struct parents issue") &&
           assert_true(!has_missing_mcid, "mcid not required without tags");
}

static int test_analyze_marked_flags(void) {
    char template[] = "/tmp/pap_pdfa_marked_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/marked.pdf", root);
    const char *contents =
        "%PDF-1.5\n"
        "<< /Catalog /Pages /StructTreeRoot /Lang (en) /Marked false /DisplayDocTitle false /RoleMap <<>> /Metadata 5 0 R >>\n"
        "<< /MarkInfo << /Marked false >> /ViewerPreferences << /DisplayDocTitle false >> /ParentTree 6 0 R >>\n"
        "<< /StructParents 1 /MCID 2 >>\n";
    if (!assert_true(write_file(path, contents), "write marked pdf")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze marked pdf")) {
        return 0;
    }

    int has_missing_marked = 0;
    int has_missing_display_doc_title = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        if (report.issues[i] == PDFA_ISSUE_MISSING_MARKED_CONTENT) {
            has_missing_marked = 1;
        }
        if (report.issues[i] == PDFA_ISSUE_MISSING_DISPLAY_DOC_TITLE) {
            has_missing_display_doc_title = 1;
        }
    }

    return assert_true(!report.has_marked_content, "marked content absent") &&
           assert_true(!report.has_display_doc_title, "display doc title absent") &&
           assert_true(report.has_role_map, "role map present") &&
           assert_true(report.has_metadata, "metadata present") &&
           assert_true(report.has_mark_info, "mark info present") &&
           assert_true(report.has_viewer_preferences, "viewer preferences present") &&
           assert_true(report.has_parent_tree, "parent tree present") &&
           assert_true(report.has_struct_parents, "struct parents present") &&
           assert_true(report.has_mcid, "mcid present") &&
           assert_true(has_missing_marked, "missing marked issue") &&
           assert_true(has_missing_display_doc_title, "missing display doc title issue");
}

static int test_analyze_missing_mcid(void) {
    char template[] = "/tmp/pap_pdfa_mcid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/mcid.pdf", root);
    const char *contents =
        "%PDF-1.7\n"
        "<< /Catalog /Pages /StructTreeRoot /Lang (en-US) >>\n";
    if (!assert_true(write_file(path, contents), "write mcid pdf")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze mcid pdf")) {
        return 0;
    }

    int has_missing_mcid = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        if (report.issues[i] == PDFA_ISSUE_MISSING_MCID) {
            has_missing_mcid = 1;
        }
    }

    return assert_true(report.has_struct_tree_root, "tags present") &&
           assert_true(!report.has_mcid, "mcid missing") &&
           assert_true(has_missing_mcid, "missing mcid issue");
}

static int test_analyze_text_alternatives_variants(void) {
    char template[] = "/tmp/pap_pdfa_alt_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char alt_path[PATH_MAX];
    snprintf(alt_path, sizeof(alt_path), "%s/alt.pdf", root);
    const char *alt_contents =
        "%PDF-1.7\n"
        "<< /Catalog /Pages /StructTreeRoot /Lang (en-US) >>\n"
        "<< /Alt (Figure description) >>\n";
    if (!assert_true(write_file(alt_path, alt_contents), "write alt-only pdf")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(alt_path, &report) == PDFA_OK, "analyze alt-only pdf")) {
        return 0;
    }

    int has_missing_alt = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        if (report.issues[i] == PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES) {
            has_missing_alt = 1;
        }
    }
    if (!assert_true(report.has_alt_text, "alt text present") ||
        !assert_true(!report.has_actual_text, "actual text absent") ||
        !assert_true(!has_missing_alt, "no missing alt issue when alt text present")) {
        return 0;
    }

    char actual_path[PATH_MAX];
    snprintf(actual_path, sizeof(actual_path), "%s/actual.pdf", root);
    const char *actual_contents =
        "%PDF-1.7\n"
        "<< /Catalog /Pages /StructTreeRoot /Lang (en-US) >>\n"
        "<< /ActualText (Replacement text) >>\n";
    if (!assert_true(write_file(actual_path, actual_contents), "write actual-text-only pdf")) {
        return 0;
    }

    if (!assert_true(pdfa_analyze_file(actual_path, &report) == PDFA_OK, "analyze actual-text-only pdf")) {
        return 0;
    }

    int has_missing_actual = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        if (report.issues[i] == PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES) {
            has_missing_actual = 1;
        }
    }

    return assert_true(!report.has_alt_text, "alt text absent") &&
           assert_true(report.has_actual_text, "actual text present") &&
           assert_true(!has_missing_actual, "no missing alt issue when actual text present");
}

static int test_analyze_lang_requires_value(void) {
    char template[] = "/tmp/pap_pdfa_lang_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/lang.pdf", root);
    const char *contents =
        "%PDF-1.7\n"
        "<< /Type /Catalog /Pages 2 0 R /Lang >>\n";
    if (!assert_true(write_file(path, contents), "write lang missing value pdf")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze lang missing value pdf")) {
        return 0;
    }

    int has_missing_lang = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        if (report.issues[i] == PDFA_ISSUE_MISSING_LANGUAGE) {
            has_missing_lang = 1;
        }
    }

    return assert_true(!report.has_lang, "lang value absent") &&
           assert_true(has_missing_lang, "missing lang issue");
}

static int test_analyze_chunk_boundary_values(void) {
    char template[] = "/tmp/pap_pdfa_boundary_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/boundary.pdf", root);

    size_t buffer_len = (size_t)PDFA_SCAN_CHUNK_SIZE * 2 + 512;
    char *buffer = calloc(buffer_len, 1);
    if (!buffer) {
        perror("calloc failed");
        return 0;
    }

    size_t offset = 0;
    size_t boundary1 = (size_t)PDFA_SCAN_CHUNK_SIZE - 5;
    size_t boundary2 = (size_t)PDFA_SCAN_CHUNK_SIZE * 2 - 7;
    if (offset >= boundary1 || boundary1 >= buffer_len || boundary2 >= buffer_len) {
        free(buffer);
        return assert_true(0, "invalid boundary offsets");
    }
    if (!append_text(buffer, buffer_len, &offset,
                     "%PDF-1.7\n"
                     "<< /Type /Catalog /Pages /Outlines /StructTreeRoot ") ||
        !append_padding(buffer, buffer_len, &offset, boundary1 - offset, 'A') ||
        !append_text(buffer, buffer_len, &offset, "/Lang") ||
        !append_text(buffer, buffer_len, &offset, " (en-US) ") ||
        !append_padding(buffer, buffer_len, &offset, boundary2 - offset, 'B') ||
        !append_text(buffer, buffer_len, &offset, "/Marked") ||
        !append_text(buffer, buffer_len, &offset,
                     " true /DisplayDocTitle true /Title (Advanced PDF) "
                     "/Alt (Alt text) /ActualText (Actual text) "
                     "/RoleMap <<>> /Metadata 5 0 R >>\n"
                     "<< /MarkInfo << /Marked true >> "
                     "/ViewerPreferences << /DisplayDocTitle true >> "
                     "/ParentTree 6 0 R >>\n"
                     "<< /StructParents 2 /MCID 7 >>\n")) {
        free(buffer);
        return assert_true(0, "failed to build chunk boundary buffer");
    }

    if (!assert_true(write_buffer(path, buffer, offset), "write boundary pdf")) {
        free(buffer);
        return 0;
    }
    free(buffer);

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze boundary pdf")) {
        return 0;
    }

    return assert_true(report.has_lang, "boundary lang present") &&
           assert_true(report.has_marked_content, "boundary marked content present") &&
           assert_true(report.has_display_doc_title, "boundary display doc title present") &&
           assert_true(report.has_title, "boundary title present") &&
           assert_true(report.has_alt_text, "boundary alt text present") &&
           assert_true(report.has_actual_text, "boundary actual text present") &&
           assert_true(report.has_role_map, "boundary role map present") &&
           assert_true(report.has_metadata, "boundary metadata present") &&
           assert_true(report.has_mark_info, "boundary mark info present") &&
           assert_true(report.has_viewer_preferences, "boundary viewer preferences present") &&
           assert_true(report.has_parent_tree, "boundary parent tree present") &&
           assert_true(report.has_struct_parents, "boundary struct parents present") &&
           assert_true(report.has_mcid, "boundary mcid present") &&
           assert_true(report.issue_count == 0, "boundary has no issues");
}

static int test_analyze_fixture_pdfs(void) {
    const char *missing_path = "tests/fixtures/problem_document.pdf";
    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(missing_path, &report) == PDFA_OK,
                     "analyze fixture missing pdf")) {
        return 0;
    }

    int has_missing_outlines = 0;
    int has_missing_tags = 0;
    int has_missing_lang = 0;
    int has_missing_title = 0;
    int has_missing_alt = 0;
    for (size_t i = 0; i < report.issue_count; ++i) {
        switch (report.issues[i]) {
            case PDFA_ISSUE_MISSING_OUTLINES:
                has_missing_outlines = 1;
                break;
            case PDFA_ISSUE_MISSING_TAGS:
                has_missing_tags = 1;
                break;
            case PDFA_ISSUE_MISSING_LANGUAGE:
                has_missing_lang = 1;
                break;
            case PDFA_ISSUE_MISSING_TITLE:
                has_missing_title = 1;
                break;
            case PDFA_ISSUE_MISSING_TEXT_ALTERNATIVES:
                has_missing_alt = 1;
                break;
            default:
                break;
        }
    }

    if (!assert_true(report.has_catalog, "fixture missing catalog present") ||
        !assert_true(report.has_pages, "fixture missing pages present") ||
        !assert_true(has_missing_outlines, "fixture missing outlines issue") ||
        !assert_true(has_missing_tags, "fixture missing tags issue") ||
        !assert_true(has_missing_lang, "fixture missing language issue") ||
        !assert_true(has_missing_title, "fixture missing title issue") ||
        !assert_true(has_missing_alt, "fixture missing alt issue")) {
        return 0;
    }

    const char *fixed_path = "tests/fixtures/fixed_document.pdf";
    if (!assert_true(pdfa_analyze_file(fixed_path, &report) == PDFA_OK,
                     "analyze fixture fixed pdf")) {
        return 0;
    }

    return assert_true(report.pdf_version_major == 1 && report.pdf_version_minor == 7,
                       "fixture fixed version parsed") &&
           assert_true(report.has_catalog, "fixture fixed catalog present") &&
           assert_true(report.has_pages, "fixture fixed pages present") &&
           assert_true(report.has_outlines, "fixture fixed outlines present") &&
           assert_true(report.has_struct_tree_root, "fixture fixed tags present") &&
           assert_true(report.has_lang, "fixture fixed lang present") &&
           assert_true(report.has_alt_text, "fixture fixed alt text present") &&
           assert_true(report.has_actual_text, "fixture fixed actual text present") &&
           assert_true(report.has_title, "fixture fixed title present") &&
           assert_true(report.has_marked_content, "fixture fixed marked content present") &&
           assert_true(report.has_display_doc_title, "fixture fixed display doc title present") &&
           assert_true(report.has_role_map, "fixture fixed role map present") &&
           assert_true(report.has_metadata, "fixture fixed metadata present") &&
           assert_true(report.has_mark_info, "fixture fixed mark info present") &&
           assert_true(report.has_viewer_preferences, "fixture fixed viewer preferences present") &&
           assert_true(report.has_parent_tree, "fixture fixed parent tree present") &&
           assert_true(report.has_struct_parents, "fixture fixed struct parents present") &&
           assert_true(report.has_mcid, "fixture fixed mcid present") &&
           assert_true(report.issue_count == 0, "fixture fixed has no issues");
}

static int test_json_invalid_args(void) {
    char buffer[32];
    pdfa_report_t report;
    pdfa_report_init(&report);
    return assert_true(pdfa_report_to_json(NULL, buffer, sizeof(buffer), NULL) == PDFA_ERR_INVALID_ARGUMENT,
                       "json should reject NULL report") &&
           assert_true(pdfa_report_to_json(&report, NULL, sizeof(buffer), NULL) == PDFA_ERR_INVALID_ARGUMENT,
                       "json should reject NULL buffer") &&
           assert_true(pdfa_report_to_json(&report, buffer, 0, NULL) == PDFA_ERR_INVALID_ARGUMENT,
                       "json should reject zero buffer");
}

static int test_json_buffer_too_small(void) {
    char template[] = "/tmp/pap_pdfa_json_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/complete.pdf", root);
    const char *contents =
        "%PDF-1.7\n"
        "<< /Catalog /Pages /Outlines /StructTreeRoot /Lang (en-US) /Alt (alt) /ActualText (actual) /Title (Doc) "
        "/Marked true /DisplayDocTitle true /RoleMap <<>> /Metadata 5 0 R >>\n"
        "<< /MarkInfo << /Marked true >> /ViewerPreferences << /DisplayDocTitle true >> /ParentTree 6 0 R >>\n"
        "<< /StructParents 1 /MCID 0 >>\n";
    if (!assert_true(write_file(path, contents), "write pdf for json")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze for json")) {
        return 0;
    }

    char buffer[8];
    return assert_true(pdfa_report_to_json(&report, buffer, sizeof(buffer), NULL) == PDFA_ERR_BUFFER_TOO_SMALL,
                       "json should report buffer too small");
}

static int test_json_success(void) {
    char template[] = "/tmp/pap_pdfa_json_ok_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/complete.pdf", root);
    const char *contents =
        "%PDF-1.6\n"
        "<< /Catalog /Pages /Outlines /StructTreeRoot /Lang (en-US) /Alt (alt) /ActualText (actual) /Title (Doc) "
        "/Marked true /DisplayDocTitle true /RoleMap <<>> /Metadata 5 0 R >>\n"
        "<< /MarkInfo << /Marked true >> /ViewerPreferences << /DisplayDocTitle true >> /ParentTree 6 0 R >>\n"
        "<< /StructParents 1 /MCID 0 >>\n";
    if (!assert_true(write_file(path, contents), "write pdf for json success")) {
        return 0;
    }

    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(path, &report) == PDFA_OK, "analyze for json success")) {
        return 0;
    }

    char buffer[512];
    size_t written = 0;
    if (!assert_true(pdfa_report_to_json(&report, buffer, sizeof(buffer), &written) == PDFA_OK,
                     "json should succeed")) {
        return 0;
    }
    return assert_true(strstr(buffer, "\"pdf_version\":\"1.6\"") != NULL, "json includes version") &&
           assert_true(strstr(buffer, "\"has_title\":true") != NULL, "json includes title") &&
           assert_true(strstr(buffer, "\"has_marked_content\":true") != NULL, "json includes marked content") &&
           assert_true(strstr(buffer, "\"has_display_doc_title\":true") != NULL, "json includes display doc title") &&
           assert_true(strstr(buffer, "\"has_role_map\":true") != NULL, "json includes role map") &&
           assert_true(strstr(buffer, "\"has_metadata\":true") != NULL, "json includes metadata") &&
           assert_true(strstr(buffer, "\"has_mark_info\":true") != NULL, "json includes mark info") &&
           assert_true(strstr(buffer, "\"has_viewer_preferences\":true") != NULL, "json includes viewer preferences") &&
           assert_true(strstr(buffer, "\"has_parent_tree\":true") != NULL, "json includes parent tree") &&
           assert_true(strstr(buffer, "\"has_struct_parents\":true") != NULL, "json includes struct parents") &&
           assert_true(strstr(buffer, "\"has_mcid\":true") != NULL, "json includes mcid") &&
           assert_true(strstr(buffer, "\"issues\":[]") != NULL, "json includes empty issues") &&
           assert_true(written == strlen(buffer), "written matches length");
}

static int test_html_invalid_args(void) {
    char buffer[128];
    pdfa_report_t report;
    pdfa_report_init(&report);
    return assert_true(pdfa_report_to_html(NULL, "before.pdf", "after.pdf", buffer, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "html should reject NULL report") &&
           assert_true(pdfa_report_to_html(&report, NULL, "after.pdf", buffer, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "html should reject NULL before link") &&
           assert_true(pdfa_report_to_html(&report, "before.pdf", NULL, buffer, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "html should reject NULL after link") &&
           assert_true(pdfa_report_to_html(&report, "before.pdf", "after.pdf", NULL, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "html should reject NULL buffer") &&
           assert_true(pdfa_report_to_html(&report, "before.pdf", "after.pdf", buffer, 0, NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "html should reject zero buffer");
}

static int test_html_buffer_too_small(void) {
    pdfa_report_t report;
    pdfa_report_init(&report);
    report.pdf_version_major = 1;
    report.pdf_version_minor = 7;
    char buffer[16];
    return assert_true(pdfa_report_to_html(&report,
                                           "before.pdf",
                                           "after.pdf",
                                           buffer,
                                           sizeof(buffer),
                                           NULL) == PDFA_ERR_BUFFER_TOO_SMALL,
                       "html should report buffer too small");
}

static int test_html_success(void) {
    const char *fixed_path = "tests/fixtures/fixed_document.pdf";
    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(fixed_path, &report) == PDFA_OK, "analyze for html success")) {
        return 0;
    }

    char buffer[4096];
    size_t written = 0;
    if (!assert_true(pdfa_report_to_html(&report,
                                         "tests/fixtures/problem_document.pdf",
                                         "tests/fixtures/fixed_document.pdf",
                                         buffer,
                                         sizeof(buffer),
                                         &written) == PDFA_OK,
                     "html should succeed")) {
        return 0;
    }

    return assert_true(strstr(buffer, "<!doctype html>") != NULL, "html includes doctype") &&
           assert_true(strstr(buffer, "Accessibility Transformation Report") != NULL,
                       "html includes report title") &&
           assert_true(strstr(buffer, "Before PDF") != NULL, "html includes before link text") &&
           assert_true(strstr(buffer, "After PDF") != NULL, "html includes after link text") &&
           assert_true(strstr(buffer, "tests/fixtures/problem_document.pdf") != NULL,
                       "html includes before link href") &&
           assert_true(strstr(buffer, "tests/fixtures/fixed_document.pdf") != NULL,
                       "html includes after link href") &&
           assert_true(written == strlen(buffer), "html written matches length");
}

static int test_html_analysis_invalid_args(void) {
    char buffer[128];
    pdfa_report_t report;
    pdfa_report_init(&report);
    return assert_true(pdfa_report_to_html_analysis(NULL, "source.pdf", buffer, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "analysis html should reject NULL report") &&
           assert_true(pdfa_report_to_html_analysis(&report, NULL, buffer, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "analysis html should reject NULL source link") &&
           assert_true(pdfa_report_to_html_analysis(&report, "source.pdf", NULL, sizeof(buffer), NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "analysis html should reject NULL buffer") &&
           assert_true(pdfa_report_to_html_analysis(&report, "source.pdf", buffer, 0, NULL) ==
                           PDFA_ERR_INVALID_ARGUMENT,
                       "analysis html should reject zero buffer");
}

static int test_html_analysis_buffer_too_small(void) {
    pdfa_report_t report;
    pdfa_report_init(&report);
    report.pdf_version_major = 1;
    report.pdf_version_minor = 7;
    char buffer[16];
    return assert_true(pdfa_report_to_html_analysis(&report,
                                                    "source.pdf",
                                                    buffer,
                                                    sizeof(buffer),
                                                    NULL) == PDFA_ERR_BUFFER_TOO_SMALL,
                       "analysis html should report buffer too small");
}

static int test_html_analysis_success(void) {
    const char *fixed_path = "tests/fixtures/fixed_document.pdf";
    pdfa_report_t report;
    if (!assert_true(pdfa_analyze_file(fixed_path, &report) == PDFA_OK, "analyze for analysis html success")) {
        return 0;
    }

    char buffer[4096];
    size_t written = 0;
    if (!assert_true(pdfa_report_to_html_analysis(&report,
                                                  "tests/fixtures/fixed_document.pdf",
                                                  buffer,
                                                  sizeof(buffer),
                                                  &written) == PDFA_OK,
                     "analysis html should succeed")) {
        return 0;
    }

    return assert_true(strstr(buffer, "<!doctype html>") != NULL, "analysis html includes doctype") &&
           assert_true(strstr(buffer, "Accessibility Analysis Report") != NULL,
                       "analysis html includes report title") &&
           assert_true(strstr(buffer, "Source PDF") != NULL, "analysis html includes source link text") &&
           assert_true(strstr(buffer, "tests/fixtures/fixed_document.pdf") != NULL,
                       "analysis html includes source link href") &&
           assert_true(written == strlen(buffer), "analysis html written matches length");
}

static int test_result_str(void) {
    return assert_true(strcmp(pdfa_result_str(PDFA_OK), "ok") == 0, "result ok") &&
           assert_true(strcmp(pdfa_result_str(PDFA_ERR_INVALID_ARGUMENT), "invalid_argument") == 0,
                       "result invalid arg") &&
           assert_true(strcmp(pdfa_result_str(PDFA_ERR_NOT_FOUND), "not_found") == 0, "result not found") &&
           assert_true(strcmp(pdfa_result_str(PDFA_ERR_IO), "io_error") == 0, "result io error") &&
           assert_true(strcmp(pdfa_result_str(PDFA_ERR_PARSE), "parse_error") == 0, "result parse error") &&
           assert_true(strcmp(pdfa_result_str(PDFA_ERR_BUFFER_TOO_SMALL), "buffer_too_small") == 0,
                       "result buffer too small") &&
           assert_true(strcmp(pdfa_result_str((pdfa_result_t)99), "unknown") == 0, "result unknown");
}

int main(void) {
    int ok = 1;

    ok &= test_report_init_invalid();
    ok &= test_analyze_invalid();
    ok &= test_analyze_missing_file();
    ok &= test_analyze_parse_error();
    ok &= test_analyze_complete_pdf();
    ok &= test_analyze_missing_features();
    ok &= test_analyze_marked_flags();
    ok &= test_analyze_missing_mcid();
    ok &= test_analyze_text_alternatives_variants();
    ok &= test_analyze_lang_requires_value();
    ok &= test_analyze_chunk_boundary_values();
    ok &= test_analyze_fixture_pdfs();
    ok &= test_json_invalid_args();
    ok &= test_json_buffer_too_small();
    ok &= test_json_success();
    ok &= test_html_invalid_args();
    ok &= test_html_buffer_too_small();
    ok &= test_html_success();
    ok &= test_html_analysis_invalid_args();
    ok &= test_html_analysis_buffer_too_small();
    ok &= test_html_analysis_success();
    ok &= test_result_str();

    if (ok) {
        printf("All PDF accessibility tests passed.\n");
        return 0;
    }
    return 1;
}
