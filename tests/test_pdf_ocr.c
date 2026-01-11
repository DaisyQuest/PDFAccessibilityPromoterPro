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

typedef struct {
    pocr_log_level_t level;
    char message[256];
} log_entry_t;

static log_entry_t log_entries[16];
static size_t log_entry_count = 0;

static void test_logger(pocr_log_level_t level, const char *message, void *user_data) {
    (void)user_data;
    if (log_entry_count >= sizeof(log_entries) / sizeof(log_entries[0])) {
        return;
    }
    log_entries[log_entry_count].level = level;
    snprintf(log_entries[log_entry_count].message, sizeof(log_entries[log_entry_count].message),
             "%s", message ? message : "");
    log_entry_count++;
}

static void reset_logs(void) {
    log_entry_count = 0;
    memset(log_entries, 0, sizeof(log_entries));
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

static int test_scan_provider_missing(void) {
    pocr_report_t report;
    reset_logs();
    pocr_set_logger(test_logger, NULL);
    pocr_result_t result = pocr_scan_file_with_provider("missing-provider", "file.pdf", &report);
    pocr_set_logger(NULL, NULL);
    return assert_true(result == POCR_ERR_PROVIDER_NOT_FOUND, "unknown provider should return provider not found") &&
           assert_true(log_entry_count > 0, "missing provider should log") &&
           assert_true(log_entries[0].level == POCR_LOG_ERROR, "missing provider log level");
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
           assert_true(report.bytes_scanned == expected_size, "bytes scanned matches file size") &&
           assert_true(report.provider_name != NULL, "provider name set");
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
    report.provider_name = "custom";

    char buffer[256];
    size_t written = 0;
    if (!assert_true(pocr_report_to_json(&report, buffer, sizeof(buffer), &written) == POCR_OK,
                     "report_to_json success")) {
        return 0;
    }
    buffer[written] = '\0';
    return assert_true(strstr(buffer, "\"ocr_status\":\"complete\"") != NULL, "contains status") &&
           assert_true(strstr(buffer, "\"ocr_provider\":\"custom\"") != NULL, "contains provider") &&
           assert_true(strstr(buffer, "\"pdf_version\":\"1.6\"") != NULL, "contains version") &&
           assert_true(strstr(buffer, "\"bytes_scanned\":120") != NULL, "contains bytes");
}

static int test_result_str(void) {
    return assert_true(strcmp(pocr_result_str(POCR_ERR_PARSE), "parse_error") == 0,
                       "result string for parse error") &&
           assert_true(strcmp(pocr_result_str(POCR_ERR_PROVIDER_NOT_FOUND), "provider_not_found") == 0,
                       "result string for provider not found") &&
           assert_true(strcmp(pocr_result_str(POCR_ERR_PROVIDER_EXISTS), "provider_exists") == 0,
                       "result string for provider exists") &&
           assert_true(strcmp(pocr_result_str(POCR_ERR_PROVIDER_LIMIT), "provider_limit") == 0,
                       "result string for provider limit");
}

static pocr_result_t stub_provider_scan(const char *path, pocr_report_t *report, void *user_data) {
    const char *expected = user_data ? (const char *)user_data : "";
    if (!path || !report) {
        return POCR_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(path, expected) != 0) {
        return POCR_ERR_PARSE;
    }
    report->pdf_version_major = 2;
    report->pdf_version_minor = 0;
    report->bytes_scanned = 99;
    return POCR_OK;
}

static int test_provider_registry_invalid(void) {
    pocr_provider_t provider;
    memset(&provider, 0, sizeof(provider));
    return assert_true(pocr_register_provider(NULL) == POCR_ERR_INVALID_ARGUMENT,
                       "provider register should reject NULL") &&
           assert_true(pocr_register_provider(&provider) == POCR_ERR_INVALID_ARGUMENT,
                       "provider register should reject missing name") &&
           assert_true(pocr_register_provider(&(pocr_provider_t){ .name = "", .scan_file = stub_provider_scan }) ==
                           POCR_ERR_INVALID_ARGUMENT,
                       "provider register should reject empty name") &&
           assert_true(pocr_register_provider(&(pocr_provider_t){ .name = "bad", .scan_file = NULL }) ==
                           POCR_ERR_INVALID_ARGUMENT,
                       "provider register should reject missing scan");
}

static int test_provider_registry_duplicate(void) {
    pocr_provider_t provider = {
        .name = "builtin",
        .scan_file = stub_provider_scan,
        .user_data = NULL
    };
    return assert_true(pocr_register_provider(&provider) == POCR_ERR_PROVIDER_EXISTS,
                       "duplicate provider name should be rejected");
}

static int test_provider_registry_success(void) {
    pocr_provider_t provider = {
        .name = "stub",
        .scan_file = stub_provider_scan,
        .user_data = "expected.pdf"
    };
    pocr_result_t result = pocr_register_provider(&provider);
    if (!assert_true(result == POCR_OK, "register stub provider")) {
        return 0;
    }

    const pocr_provider_t *found = pocr_find_provider("stub");
    if (!assert_true(found != NULL, "find stub provider")) {
        return 0;
    }

    pocr_report_t report;
    if (!assert_true(pocr_scan_file_with_provider("stub", "expected.pdf", &report) == POCR_OK,
                     "scan with stub provider")) {
        return 0;
    }

    return assert_true(report.pdf_version_major == 2, "stub provider major") &&
           assert_true(report.pdf_version_minor == 0, "stub provider minor") &&
           assert_true(report.bytes_scanned == 99, "stub provider bytes") &&
           assert_true(report.provider_name != NULL &&
                           strcmp(report.provider_name, "stub") == 0,
                       "stub provider name set");
}

static int test_provider_registry_limit(void) {
    size_t capacity = pocr_provider_capacity();
    size_t count = pocr_provider_count();
    if (capacity == 0 || count > capacity) {
        return assert_true(0, "provider capacity invalid");
    }

    size_t index = 0;
    while (count < capacity) {
        char *name_buffer = malloc(32);
        if (!name_buffer) {
            return assert_true(0, "malloc provider name failed");
        }
        snprintf(name_buffer, 32, "extra_%zu", index++);
        pocr_provider_t provider = {
            .name = name_buffer,
            .scan_file = stub_provider_scan,
            .user_data = "ignored"
        };
        pocr_result_t result = pocr_register_provider(&provider);
        if (result == POCR_ERR_PROVIDER_EXISTS) {
            continue;
        }
        if (result != POCR_OK) {
            return assert_true(0, "register provider failed before limit");
        }
        count++;
    }

    pocr_provider_t extra = {
        .name = "overflow",
        .scan_file = stub_provider_scan,
        .user_data = "ignored"
    };
    return assert_true(pocr_register_provider(&extra) == POCR_ERR_PROVIDER_LIMIT,
                       "provider limit reached");
}

int main(void) {
    int passed = 1;

    passed &= test_report_init_invalid();
    passed &= test_scan_invalid();
    passed &= test_scan_missing_file();
    passed &= test_scan_provider_missing();
    passed &= test_scan_parse_error();
    passed &= test_scan_success();
    passed &= test_report_to_json();
    passed &= test_report_to_json_success();
    passed &= test_result_str();
    passed &= test_provider_registry_invalid();
    passed &= test_provider_registry_duplicate();
    passed &= test_provider_registry_success();
    passed &= test_provider_registry_limit();

    if (!passed) {
        fprintf(stderr, "OCR tests failed.\n");
        return 1;
    }

    printf("OCR tests passed.\n");
    return 0;
}
