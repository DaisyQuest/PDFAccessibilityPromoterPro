#include "pap/pdf_redaction.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", message);
        return 0;
    }
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

static int read_file(const char *path, char *buffer, size_t length) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    size_t bytes = fread(buffer, 1, length - 1, fp);
    buffer[bytes] = '\0';
    fclose(fp);
    return 1;
}

static int read_at_offset(const char *path, size_t offset, char *buffer, size_t length) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    size_t bytes = fread(buffer, 1, length, fp);
    fclose(fp);
    return bytes == length;
}

static int test_plan_init_invalid(void) {
    return assert_true(pdrx_plan_init(NULL) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_plan_init should reject NULL");
}

static int test_plan_from_json_invalid(void) {
    pdrx_plan_t plan;
    return assert_true(pdrx_plan_from_json(NULL, 0, &plan) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_plan_from_json should reject NULL") &&
           assert_true(pdrx_plan_from_json("{}", 2, NULL) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_plan_from_json should reject NULL plan");
}

static int test_plan_from_json_parse_error(void) {
    pdrx_plan_t plan;
    return assert_true(pdrx_plan_from_json("{}", 2, &plan) == PDRX_ERR_PARSE,
                       "pdrx_plan_from_json should detect missing redactions");
}

static int test_plan_from_json_empty_string(void) {
    pdrx_plan_t plan;
    const char *json = "{\"redactions\":[\"\"]}";
    return assert_true(pdrx_plan_from_json(json, strlen(json), &plan) == PDRX_ERR_PARSE,
                       "pdrx_plan_from_json should reject empty strings");
}

static int test_plan_from_json_success(void) {
    pdrx_plan_t plan;
    const char *json = "{\"redactions\":[\"SECRET\",\"CL\\\"ASS\"]}";
    if (!assert_true(pdrx_plan_from_json(json, strlen(json), &plan) == PDRX_OK,
                     "pdrx_plan_from_json success")) {
        return 0;
    }
    return assert_true(plan.redaction_count == 2, "plan count") &&
           assert_true(strcmp(plan.patterns[0], "SECRET") == 0, "pattern 1") &&
           assert_true(strcmp(plan.patterns[1], "CL\"ASS") == 0, "pattern 2") &&
           assert_true(plan.pattern_lengths[0] == 6, "pattern 1 length") &&
           assert_true(plan.pattern_lengths[1] == 6, "pattern 2 length");
}

static int test_plan_from_json_too_many(void) {
    char buffer[2048];
    size_t offset = 0;
    offset += (size_t)snprintf(buffer + offset, sizeof(buffer) - offset, "{\"redactions\":[");
    for (size_t i = 0; i < PDRX_MAX_REDACTIONS + 1; ++i) {
        offset += (size_t)snprintf(buffer + offset, sizeof(buffer) - offset,
                                   "%s\"A\"", i == 0 ? "" : ",");
    }
    offset += (size_t)snprintf(buffer + offset, sizeof(buffer) - offset, "]}");

    pdrx_plan_t plan;
    return assert_true(pdrx_plan_from_json(buffer, strlen(buffer), &plan) == PDRX_ERR_BUFFER_TOO_SMALL,
                       "pdrx_plan_from_json should detect too many patterns");
}

static int test_apply_invalid(void) {
    pdrx_plan_t plan;
    pdrx_report_t report;
    pdrx_plan_init(&plan);
    return assert_true(pdrx_apply_file(NULL, "out", &plan, &report) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_apply_file should reject NULL input") &&
           assert_true(pdrx_apply_file("in", NULL, &plan, &report) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_apply_file should reject NULL output") &&
           assert_true(pdrx_apply_file("in", "out", NULL, &report) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_apply_file should reject NULL plan") &&
           assert_true(pdrx_apply_file("in", "out", &plan, NULL) == PDRX_ERR_INVALID_ARGUMENT,
                       "pdrx_apply_file should reject NULL report");
}

static int test_apply_missing_file(void) {
    pdrx_plan_t plan;
    pdrx_report_t report;
    pdrx_plan_init(&plan);
    return assert_true(pdrx_apply_file("missing.pdf", "out.pdf", &plan, &report) == PDRX_ERR_NOT_FOUND,
                       "pdrx_apply_file should report missing file");
}

static int test_apply_empty_plan(void) {
    char template[] = "/tmp/pap_redact_empty_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char input[PATH_MAX];
    char output[PATH_MAX];
    snprintf(input, sizeof(input), "%s/input.pdf", root);
    snprintf(output, sizeof(output), "%s/output.pdf", root);

    const char *contents = "%PDF-1.7\nHello World";
    if (!assert_true(write_buffer(input, contents, strlen(contents)), "write input pdf")) {
        return 0;
    }

    pdrx_plan_t plan;
    pdrx_report_t report;
    pdrx_plan_init(&plan);
    if (!assert_true(pdrx_apply_file(input, output, &plan, &report) == PDRX_OK,
                     "apply empty plan")) {
        return 0;
    }

    char output_buffer[64];
    if (!assert_true(read_file(output, output_buffer, sizeof(output_buffer)), "read output")) {
        return 0;
    }

    return assert_true(strcmp(output_buffer, contents) == 0, "output matches input") &&
           assert_true(report.match_count == 0, "no matches") &&
           assert_true(report.bytes_redacted == 0, "no bytes redacted");
}

static int test_apply_boundary_redaction(void) {
    char template[] = "/tmp/pap_redact_boundary_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char input[PATH_MAX];
    char output[PATH_MAX];
    snprintf(input, sizeof(input), "%s/input.pdf", root);
    snprintf(output, sizeof(output), "%s/output.pdf", root);

    const char *header = "%PDF-1.7\n";
    size_t header_len = strlen(header);
    size_t prefix_len = 32768 - 3;
    size_t pad_len = prefix_len > header_len ? prefix_len - header_len : 0;
    size_t total_len = prefix_len + 6 + 10;
    char *buffer = malloc(total_len);
    if (!buffer) {
        perror("malloc failed");
        return 0;
    }
    size_t offset = 0;
    memcpy(buffer + offset, header, header_len);
    offset += header_len;
    memset(buffer + offset, 'A', pad_len);
    offset += pad_len;
    memcpy(buffer + offset, "SECRET", 6);
    offset += 6;
    memset(buffer + offset, 'B', total_len - offset);

    if (!assert_true(write_buffer(input, buffer, total_len), "write boundary pdf")) {
        free(buffer);
        return 0;
    }
    free(buffer);

    pdrx_plan_t plan;
    pdrx_plan_init(&plan);
    strcpy(plan.patterns[0], "SECRET");
    plan.pattern_lengths[0] = 6;
    plan.redaction_count = 1;

    pdrx_report_t report;
    if (!assert_true(pdrx_apply_file(input, output, &plan, &report) == PDRX_OK,
                     "apply boundary redaction")) {
        return 0;
    }

    char redacted[7];
    if (!assert_true(read_at_offset(output, prefix_len, redacted, 6), "read redacted bytes")) {
        return 0;
    }
    redacted[6] = '\0';

    return assert_true(strcmp(redacted, "XXXXXX") == 0, "redaction applied") &&
           assert_true(report.match_count == 1, "match count") &&
           assert_true(report.bytes_redacted == 6, "bytes redacted");
}

static int test_apply_pii_redaction(void) {
    char template[] = "/tmp/pap_redact_pii_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char input[PATH_MAX];
    char output[PATH_MAX];
    snprintf(input, sizeof(input), "%s/input.pdf", root);
    snprintf(output, sizeof(output), "%s/output.pdf", root);

    const char *contents =
        "%PDF-1.7\n"
        "SSN 123-45-6789\n"
        "SSN 123456789\n"
        "SSN 6789\n"
        "NINO AB 12 34 56 C\n"
        "SIN 046 454 286\n"
        "AADHAAR 1000 0000 0004\n";
    if (!assert_true(write_buffer(input, contents, strlen(contents)), "write pii pdf")) {
        return 0;
    }

    pdrx_plan_t plan;
    pdrx_report_t report;
    pdrx_plan_init(&plan);
    if (!assert_true(pdrx_apply_file(input, output, &plan, &report) == PDRX_OK,
                     "apply pii redaction")) {
        return 0;
    }

    char output_buffer[512];
    if (!assert_true(read_file(output, output_buffer, sizeof(output_buffer)), "read pii output")) {
        return 0;
    }

    return assert_true(strstr(output_buffer, "SSN XXXXXXXXXXX") != NULL, "redact ssn dashed") &&
           assert_true(strstr(output_buffer, "SSN XXXXXXXXX") != NULL, "redact ssn compact") &&
           assert_true(strstr(output_buffer, "SSN XXXX") != NULL, "redact ssn last4") &&
           assert_true(strstr(output_buffer, "NINO XXXXXXXXXXXXX") != NULL, "redact nino") &&
           assert_true(strstr(output_buffer, "SIN XXXXXXXXXXX") != NULL, "redact sin") &&
           assert_true(strstr(output_buffer, "AADHAAR XXXXXXXXXXXXXX") != NULL, "redact aadhaar") &&
           assert_true(report.match_count == 6, "pii match count") &&
           assert_true(report.bytes_redacted == 62, "pii bytes redacted");
}

static int test_apply_pii_invalid(void) {
    char template[] = "/tmp/pap_redact_pii_invalid_XXXXXX";
    char *root = mkdtemp(template);
    if (!root) {
        perror("mkdtemp failed");
        return 0;
    }
    char input[PATH_MAX];
    char output[PATH_MAX];
    snprintf(input, sizeof(input), "%s/input.pdf", root);
    snprintf(output, sizeof(output), "%s/output.pdf", root);

    const char *contents =
        "%PDF-1.7\n"
        "SSN 000-12-3456\n"
        "SSN 123-00-6789\n"
        "SSN 123-45-0000\n"
        "SSN 1234567890\n"
        "SIN 123 456 789\n"
        "AADHAAR 1000 0000 0000\n"
        "NINO DQ 12 34 56 C\n";
    if (!assert_true(write_buffer(input, contents, strlen(contents)), "write pii invalid pdf")) {
        return 0;
    }

    pdrx_plan_t plan;
    pdrx_report_t report;
    pdrx_plan_init(&plan);
    if (!assert_true(pdrx_apply_file(input, output, &plan, &report) == PDRX_OK,
                     "apply pii invalid redaction")) {
        return 0;
    }

    char output_buffer[512];
    if (!assert_true(read_file(output, output_buffer, sizeof(output_buffer)), "read pii invalid output")) {
        return 0;
    }

    return assert_true(strstr(output_buffer, "SSN 000-12-3456") != NULL, "invalid ssn area preserved") &&
           assert_true(strstr(output_buffer, "SSN 123-00-6789") != NULL, "invalid ssn group preserved") &&
           assert_true(strstr(output_buffer, "SSN 123-45-0000") != NULL, "invalid ssn serial preserved") &&
           assert_true(strstr(output_buffer, "SSN 1234567890") != NULL, "invalid ssn length preserved") &&
           assert_true(strstr(output_buffer, "SIN 123 456 789") != NULL, "invalid sin preserved") &&
           assert_true(strstr(output_buffer, "AADHAAR 1000 0000 0000") != NULL, "invalid aadhaar preserved") &&
           assert_true(strstr(output_buffer, "NINO DQ 12 34 56 C") != NULL, "invalid nino preserved") &&
           assert_true(report.match_count == 0, "invalid pii match count") &&
           assert_true(report.bytes_redacted == 0, "invalid pii bytes redacted");
}

static int test_report_to_json(void) {
    pdrx_report_t report;
    pdrx_plan_t plan;
    pdrx_plan_init(&plan);
    report.pdf_version_major = 1;
    report.pdf_version_minor = 5;
    report.match_count = 2;
    report.bytes_redacted = 12;
    report.bytes_scanned = 200;
    plan.redaction_count = 3;

    char small_buffer[16];
    return assert_true(pdrx_report_to_json(&report, &plan, small_buffer, sizeof(small_buffer), NULL) == PDRX_ERR_BUFFER_TOO_SMALL,
                       "report_to_json should detect small buffer") &&
           assert_true(pdrx_report_to_json(NULL, &plan, small_buffer, sizeof(small_buffer), NULL) == PDRX_ERR_INVALID_ARGUMENT,
                       "report_to_json should reject NULL report") &&
           assert_true(pdrx_report_to_json(&report, NULL, small_buffer, sizeof(small_buffer), NULL) == PDRX_ERR_INVALID_ARGUMENT,
                       "report_to_json should reject NULL plan") &&
           assert_true(pdrx_report_to_json(&report, &plan, NULL, sizeof(small_buffer), NULL) == PDRX_ERR_INVALID_ARGUMENT,
                       "report_to_json should reject NULL buffer");
}

static int test_report_to_json_success(void) {
    pdrx_report_t report;
    pdrx_plan_t plan;
    pdrx_plan_init(&plan);
    report.pdf_version_major = 1;
    report.pdf_version_minor = 6;
    report.match_count = 1;
    report.bytes_redacted = 6;
    report.bytes_scanned = 100;
    plan.redaction_count = 1;

    char buffer[256];
    size_t written = 0;
    if (!assert_true(pdrx_report_to_json(&report, &plan, buffer, sizeof(buffer), &written) == PDRX_OK,
                     "report_to_json success")) {
        return 0;
    }
    buffer[written] = '\0';
    return assert_true(strstr(buffer, "\"redaction_status\":\"complete\"") != NULL, "contains status") &&
           assert_true(strstr(buffer, "\"pdf_version\":\"1.6\"") != NULL, "contains version") &&
           assert_true(strstr(buffer, "\"patterns\":1") != NULL, "contains patterns") &&
           assert_true(strstr(buffer, "\"matches\":1") != NULL, "contains matches") &&
           assert_true(strstr(buffer, "\"bytes_redacted\":6") != NULL, "contains bytes redacted") &&
           assert_true(strstr(buffer, "\"bytes_scanned\":100") != NULL, "contains bytes scanned");
}

static int test_result_str(void) {
    return assert_true(strcmp(pdrx_result_str(PDRX_ERR_PARSE), "parse_error") == 0,
                       "result string for parse error");
}

int main(void) {
    int passed = 1;

    passed &= test_plan_init_invalid();
    passed &= test_plan_from_json_invalid();
    passed &= test_plan_from_json_parse_error();
    passed &= test_plan_from_json_empty_string();
    passed &= test_plan_from_json_success();
    passed &= test_plan_from_json_too_many();
    passed &= test_apply_invalid();
    passed &= test_apply_missing_file();
    passed &= test_apply_empty_plan();
    passed &= test_apply_boundary_redaction();
    passed &= test_apply_pii_redaction();
    passed &= test_apply_pii_invalid();
    passed &= test_report_to_json();
    passed &= test_report_to_json_success();
    passed &= test_result_str();

    if (!passed) {
        fprintf(stderr, "Redaction tests failed.\n");
        return 1;
    }

    printf("Redaction tests passed.\n");
    return 0;
}
