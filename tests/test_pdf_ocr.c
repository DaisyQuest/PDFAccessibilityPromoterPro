#include "pap/pdf_ocr.h"

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

static int test_report_init_invalid(void) {
    return assert_true(pocr_report_init(NULL) == POCR_ERR_INVALID_ARGUMENT,
                       "pocr_report_init should reject NULL");
}

static int test_scan_invalid(void) {
    pocr_report_t report;
    return assert_true(pocr_scan_file(NULL, &report) == POCR_ERR_INVALID_ARGUMENT,
                       "pocr_scan_file should reject NULL path") &&
           assert_true(pocr_scan_file("file.pdf", NULL) == POCR_ERR_INVALID_ARGUMENT,
                       "pocr_scan_file should reject NULL report");
}

static int test_scan_missing_file(void) {
    pocr_report_t report;
    return assert_true(pocr_scan_file("missing.pdf", &report) == POCR_ERR_NOT_FOUND,
                       "pocr_scan_file should report missing file");
}

static int test_scan_parse_error(void) {
    char template[] = "/tmp/pap_ocr_parse_XXXXXX";
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

    pocr_report_t report;
    return assert_true(pocr_scan_file(path, &report) == POCR_ERR_PARSE,
                       "pocr_scan_file should detect parse error");
}

static int test_scan_success(void) {
    char template[] = "/tmp/pap_ocr_success_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char path[PATH_MAX];
    const char *contents = "%PDF-1.7\n1 0 obj\n<<>>\nendobj\n";
    snprintf(path, sizeof(path), "%s/ok.pdf", root);
    if (!assert_true(write_file(path, contents), "write ok pdf")) {
        return 0;
    }

    pocr_report_t report;
    if (!assert_true(pocr_scan_file(path, &report) == POCR_OK, "pocr_scan_file success")) {
        return 0;
    }

    size_t expected_size = strlen(contents);
    return assert_true(report.pdf_version_major == 1, "pdf version major") &&
           assert_true(report.pdf_version_minor == 7, "pdf version minor") &&
           assert_true(report.bytes_scanned == expected_size, "bytes scanned matches file size");
}

static int test_report_to_json(void) {
    pocr_report_t report;
    if (!assert_true(pocr_report_init(&report) == POCR_OK, "init report")) {
        return 0;
    }
    report.pdf_version_major = 1;
    report.pdf_version_minor = 4;
    report.bytes_scanned = 42;

    char small_buffer[8];
    return assert_true(pocr_report_to_json(&report, small_buffer, sizeof(small_buffer), NULL) == POCR_ERR_BUFFER_TOO_SMALL,
                       "report_to_json should detect small buffer") &&
           assert_true(pocr_report_to_json(NULL, small_buffer, sizeof(small_buffer), NULL) == POCR_ERR_INVALID_ARGUMENT,
                       "report_to_json should reject NULL report") &&
           assert_true(pocr_report_to_json(&report, NULL, sizeof(small_buffer), NULL) == POCR_ERR_INVALID_ARGUMENT,
                       "report_to_json should reject NULL buffer") &&
           assert_true(pocr_report_to_json(&report, small_buffer, 0, NULL) == POCR_ERR_INVALID_ARGUMENT,
                       "report_to_json should reject empty buffer");
}

static int test_report_to_json_success(void) {
    pocr_report_t report;
    if (!assert_true(pocr_report_init(&report) == POCR_OK, "init report")) {
        return 0;
    }
    report.pdf_version_major = 1;
    report.pdf_version_minor = 6;
    report.bytes_scanned = 120;

    char buffer[256];
    size_t written = 0;
    if (!assert_true(pocr_report_to_json(&report, buffer, sizeof(buffer), &written) == POCR_OK,
                     "report_to_json success")) {
        return 0;
    }
    buffer[written] = '\0';
    return assert_true(strstr(buffer, "\"ocr_status\":\"complete\"") != NULL, "contains status") &&
           assert_true(strstr(buffer, "\"pdf_version\":\"1.6\"") != NULL, "contains version") &&
           assert_true(strstr(buffer, "\"bytes_scanned\":120") != NULL, "contains bytes");
}

static int test_result_str(void) {
    return assert_true(strcmp(pocr_result_str(POCR_ERR_PARSE), "parse_error") == 0,
                       "result string for parse error");
}

int main(void) {
    int passed = 1;

    passed &= test_report_init_invalid();
    passed &= test_scan_invalid();
    passed &= test_scan_missing_file();
    passed &= test_scan_parse_error();
    passed &= test_scan_success();
    passed &= test_report_to_json();
    passed &= test_report_to_json_success();
    passed &= test_result_str();

    if (!passed) {
        fprintf(stderr, "OCR tests failed.\n");
        return 1;
    }

    printf("OCR tests passed.\n");
    return 0;
}
