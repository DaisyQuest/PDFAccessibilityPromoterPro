#include "pap/pdf_redaction.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const size_t PDRX_MAX_PII_LEN = 14;

static void pdrx_report_init(pdrx_report_t *report) {
    memset(report, 0, sizeof(*report));
    report->pdf_version_major = -1;
    report->pdf_version_minor = -1;
}

static pdrx_result_t pdrx_scan_version(int fd, pdrx_report_t *report) {
    char buffer[64];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        return PDRX_ERR_PARSE;
    }
    buffer[bytes] = '\0';

    const char *marker = "%PDF-";
    char *found = strstr(buffer, marker);
    if (!found) {
        return PDRX_ERR_PARSE;
    }
    found += strlen(marker);
    if (!isdigit((unsigned char)found[0]) || found[1] != '.' || !isdigit((unsigned char)found[2])) {
        return PDRX_ERR_PARSE;
    }

    report->pdf_version_major = found[0] - '0';
    report->pdf_version_minor = found[2] - '0';
    return PDRX_OK;
}

static void pdrx_skip_ws(const char **cursor) {
    while (**cursor && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static int pdrx_parse_json_string(const char **cursor, char *out, size_t out_len) {
    if (**cursor != '"') {
        return 0;
    }
    (*cursor)++;
    size_t out_index = 0;
    while (**cursor) {
        char ch = **cursor;
        if (ch == '"') {
            (*cursor)++;
            if (out_index == 0) {
                return 0;
            }
            if (out_index >= out_len) {
                return 0;
            }
            out[out_index] = '\0';
            return 1;
        }
        if ((unsigned char)ch < 0x20) {
            return 0;
        }
        if (ch == '\\') {
            (*cursor)++;
            char esc = **cursor;
            if (esc == '\0') {
                return 0;
            }
            switch (esc) {
                case '"':
                case '\\':
                    ch = esc;
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                default:
                    return 0;
            }
        }
        if (out_index + 1 >= out_len) {
            return 0;
        }
        out[out_index++] = ch;
        (*cursor)++;
    }
    return 0;
}

pdrx_result_t pdrx_plan_init(pdrx_plan_t *plan) {
    if (!plan) {
        return PDRX_ERR_INVALID_ARGUMENT;
    }
    memset(plan, 0, sizeof(*plan));
    return PDRX_OK;
}

pdrx_result_t pdrx_plan_from_json(const char *json, size_t length, pdrx_plan_t *plan) {
    if (!json || length == 0 || !plan) {
        return PDRX_ERR_INVALID_ARGUMENT;
    }

    pdrx_result_t init_result = pdrx_plan_init(plan);
    if (init_result != PDRX_OK) {
        return init_result;
    }

    const char *cursor = json;
    const char *limit = json + length;
    const char *key = "\"redactions\"";
    const char *found = strstr(cursor, key);
    if (!found) {
        return PDRX_ERR_PARSE;
    }
    cursor = found + strlen(key);

    while (cursor < limit && *cursor && *cursor != '[') {
        cursor++;
    }
    if (cursor >= limit || *cursor != '[') {
        return PDRX_ERR_PARSE;
    }
    cursor++;

    while (cursor < limit) {
        pdrx_skip_ws(&cursor);
        if (cursor >= limit) {
            return PDRX_ERR_PARSE;
        }
        if (*cursor == ']') {
            cursor++;
            return PDRX_OK;
        }

        if (plan->redaction_count >= PDRX_MAX_REDACTIONS) {
            return PDRX_ERR_BUFFER_TOO_SMALL;
        }

        char *dest = plan->patterns[plan->redaction_count];
        if (!pdrx_parse_json_string(&cursor, dest, PDRX_MAX_PATTERN_LEN)) {
            return PDRX_ERR_PARSE;
        }
        plan->pattern_lengths[plan->redaction_count] = strlen(dest);
        plan->redaction_count++;

        pdrx_skip_ws(&cursor);
        if (cursor >= limit) {
            return PDRX_ERR_PARSE;
        }
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == ']') {
            cursor++;
            return PDRX_OK;
        }
        return PDRX_ERR_PARSE;
    }

    return PDRX_ERR_PARSE;
}

static int pdrx_is_boundary_before(const char *buffer, size_t buffer_len, size_t pos) {
    if (pos == 0) {
        return 1;
    }
    if (pos > buffer_len) {
        return 0;
    }
    return !isalnum((unsigned char)buffer[pos - 1]);
}

static int pdrx_is_boundary_after(const char *buffer, size_t buffer_len, size_t pos) {
    if (pos >= buffer_len) {
        return 1;
    }
    return !isalnum((unsigned char)buffer[pos]);
}

static int pdrx_window_contains_label(const char *buffer,
                                      size_t window_start,
                                      size_t window_end,
                                      const char *label) {
    size_t label_len = strlen(label);
    if (label_len == 0 || window_end < window_start || window_end - window_start < label_len) {
        return 0;
    }
    for (size_t i = window_start; i + label_len <= window_end; ++i) {
        size_t matched = 0;
        while (matched < label_len) {
            char a = (char)tolower((unsigned char)buffer[i + matched]);
            char b = (char)tolower((unsigned char)label[matched]);
            if (a != b) {
                break;
            }
            matched++;
        }
        if (matched == label_len) {
            return 1;
        }
    }
    return 0;
}

static int pdrx_ssn_groups_valid(int area, int group, int serial) {
    if (area == 0 || area == 666 || area >= 900) {
        return 0;
    }
    if (group == 0 || serial == 0) {
        return 0;
    }
    return 1;
}

static int pdrx_match_us_ssn(const char *buffer,
                             size_t buffer_len,
                             size_t pos,
                             size_t *match_len) {
    if (pos + 4 > buffer_len) {
        return 0;
    }
    if (!isdigit((unsigned char)buffer[pos])) {
        return 0;
    }
    if (!pdrx_is_boundary_before(buffer, buffer_len, pos)) {
        return 0;
    }

    if (!isdigit((unsigned char)buffer[pos + 1]) || !isdigit((unsigned char)buffer[pos + 2])) {
        return 0;
    }
    int area = (buffer[pos] - '0') * 100 + (buffer[pos + 1] - '0') * 10 + (buffer[pos + 2] - '0');

    char sep = buffer[pos + 3];
    if (sep == '-' || sep == ' ') {
        size_t end = pos + 11;
        if (end > buffer_len) {
            return 0;
        }
        if (!isdigit((unsigned char)buffer[pos + 4]) || !isdigit((unsigned char)buffer[pos + 5])) {
            return 0;
        }
        if (buffer[pos + 6] != sep) {
            return 0;
        }
        if (!isdigit((unsigned char)buffer[pos + 7]) ||
            !isdigit((unsigned char)buffer[pos + 8]) ||
            !isdigit((unsigned char)buffer[pos + 9]) ||
            !isdigit((unsigned char)buffer[pos + 10])) {
            return 0;
        }
        int group = (buffer[pos + 4] - '0') * 10 + (buffer[pos + 5] - '0');
        int serial = (buffer[pos + 7] - '0') * 1000 +
                     (buffer[pos + 8] - '0') * 100 +
                     (buffer[pos + 9] - '0') * 10 +
                     (buffer[pos + 10] - '0');
        if (!pdrx_ssn_groups_valid(area, group, serial)) {
            return 0;
        }
        if (!pdrx_is_boundary_after(buffer, buffer_len, end)) {
            return 0;
        }
        *match_len = 11;
        return 1;
    }

    if (isdigit((unsigned char)sep)) {
        size_t end = pos + 9;
        if (end > buffer_len) {
            return 0;
        }
        for (size_t i = pos; i < end; ++i) {
            if (!isdigit((unsigned char)buffer[i])) {
                return 0;
            }
        }
        size_t window_start = pos > 16 ? pos - 16 : 0;
        if (!pdrx_window_contains_label(buffer, window_start, pos, "SSN") &&
            !pdrx_window_contains_label(buffer, window_start, pos, "SOCIAL SECURITY")) {
            return 0;
        }
        int group = (buffer[pos + 3] - '0') * 10 + (buffer[pos + 4] - '0');
        int serial = (buffer[pos + 5] - '0') * 1000 +
                     (buffer[pos + 6] - '0') * 100 +
                     (buffer[pos + 7] - '0') * 10 +
                     (buffer[pos + 8] - '0');
        if (!pdrx_ssn_groups_valid(area, group, serial)) {
            return 0;
        }
        if (!pdrx_is_boundary_after(buffer, buffer_len, end)) {
            return 0;
        }
        *match_len = 9;
        return 1;
    }

    return 0;
}

static int pdrx_match_partial_ssn(const char *buffer,
                                  size_t buffer_len,
                                  size_t pos,
                                  size_t *match_len) {
    if (pos + 4 > buffer_len) {
        return 0;
    }
    if (pos > 0) {
        char prev = buffer[pos - 1];
        if (isdigit((unsigned char)prev)) {
            return 0;
        }
        if (prev == '-') {
            if (pos < 7) {
                return 0;
            }
            if (!((buffer[pos - 7] == 'X' || buffer[pos - 7] == 'x' || buffer[pos - 7] == '*') &&
                  (buffer[pos - 6] == 'X' || buffer[pos - 6] == 'x' || buffer[pos - 6] == '*') &&
                  (buffer[pos - 5] == 'X' || buffer[pos - 5] == 'x' || buffer[pos - 5] == '*') &&
                  buffer[pos - 4] == '-' &&
                  (buffer[pos - 3] == 'X' || buffer[pos - 3] == 'x' || buffer[pos - 3] == '*') &&
                  (buffer[pos - 2] == 'X' || buffer[pos - 2] == 'x' || buffer[pos - 2] == '*') &&
                  buffer[pos - 1] == '-')) {
                return 0;
            }
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!isdigit((unsigned char)buffer[pos + i])) {
            return 0;
        }
    }
    if (!pdrx_is_boundary_after(buffer, buffer_len, pos + 4)) {
        return 0;
    }
    size_t window_start = pos > 20 ? pos - 20 : 0;
    if (!pdrx_window_contains_label(buffer, window_start, pos, "SSN") &&
        !pdrx_window_contains_label(buffer, window_start, pos, "SOCIAL SECURITY")) {
        return 0;
    }
    *match_len = 4;
    return 1;
}

static int pdrx_luhn_check(const int *digits, size_t count) {
    int sum = 0;
    int double_digit = 0;
    for (size_t i = count; i-- > 0;) {
        int value = digits[i];
        if (double_digit) {
            value *= 2;
            if (value > 9) {
                value -= 9;
            }
        }
        sum += value;
        double_digit = !double_digit;
    }
    return sum % 10 == 0;
}

static int pdrx_match_canada_sin(const char *buffer,
                                 size_t buffer_len,
                                 size_t pos,
                                 size_t *match_len) {
    if (!isdigit((unsigned char)buffer[pos])) {
        return 0;
    }
    if (!pdrx_is_boundary_before(buffer, buffer_len, pos)) {
        return 0;
    }
    int digits[9];
    size_t digit_index = 0;
    size_t idx = pos;
    while (idx < buffer_len && digit_index < 9) {
        if (isdigit((unsigned char)buffer[idx])) {
            digits[digit_index++] = buffer[idx] - '0';
            idx++;
            continue;
        }
        if (buffer[idx] == ' ') {
            idx++;
            continue;
        }
        return 0;
    }
    if (digit_index != 9) {
        return 0;
    }
    size_t end = idx;
    if (!pdrx_is_boundary_after(buffer, buffer_len, end)) {
        return 0;
    }
    if (!pdrx_luhn_check(digits, 9)) {
        return 0;
    }
    *match_len = end - pos;
    return 1;
}

static int pdrx_is_valid_nino_letter(char ch, int position) {
    char upper = (char)toupper((unsigned char)ch);
    if (!isalpha((unsigned char)upper)) {
        return 0;
    }
    if (upper == 'D' || upper == 'F' || upper == 'I' || upper == 'Q' || upper == 'U' || upper == 'V') {
        return 0;
    }
    if (position == 1 && upper == 'O') {
        return 0;
    }
    return 1;
}

static int pdrx_match_uk_nino(const char *buffer,
                              size_t buffer_len,
                              size_t pos,
                              size_t *match_len) {
    if (!pdrx_is_boundary_before(buffer, buffer_len, pos)) {
        return 0;
    }
    if (pos + 8 > buffer_len) {
        return 0;
    }
    if (!pdrx_is_valid_nino_letter(buffer[pos], 0) ||
        !pdrx_is_valid_nino_letter(buffer[pos + 1], 1)) {
        return 0;
    }
    size_t idx = pos + 2;
    for (int group = 0; group < 3; ++group) {
        if (idx < buffer_len && buffer[idx] == ' ') {
            idx++;
        }
        if (idx + 2 > buffer_len) {
            return 0;
        }
        if (!isdigit((unsigned char)buffer[idx]) || !isdigit((unsigned char)buffer[idx + 1])) {
            return 0;
        }
        idx += 2;
    }
    if (idx < buffer_len && buffer[idx] == ' ') {
        idx++;
    }
    if (idx >= buffer_len) {
        return 0;
    }
    char suffix = buffer[idx];
    char upper = (char)toupper((unsigned char)suffix);
    if (upper < 'A' || upper > 'D') {
        return 0;
    }
    idx++;
    if (!pdrx_is_boundary_after(buffer, buffer_len, idx)) {
        return 0;
    }
    *match_len = idx - pos;
    return 1;
}

static const int pdrx_verhoeff_d[10][10] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
    {1, 2, 3, 4, 0, 6, 7, 8, 9, 5},
    {2, 3, 4, 0, 1, 7, 8, 9, 5, 6},
    {3, 4, 0, 1, 2, 8, 9, 5, 6, 7},
    {4, 0, 1, 2, 3, 9, 5, 6, 7, 8},
    {5, 9, 8, 7, 6, 0, 4, 3, 2, 1},
    {6, 5, 9, 8, 7, 1, 0, 4, 3, 2},
    {7, 6, 5, 9, 8, 2, 1, 0, 4, 3},
    {8, 7, 6, 5, 9, 3, 2, 1, 0, 4},
    {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}
};

static const int pdrx_verhoeff_p[8][10] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
    {1, 5, 7, 6, 2, 8, 3, 0, 9, 4},
    {5, 8, 0, 3, 7, 9, 6, 1, 4, 2},
    {8, 9, 1, 6, 0, 4, 3, 5, 2, 7},
    {9, 4, 5, 3, 1, 2, 6, 8, 7, 0},
    {4, 2, 8, 6, 5, 7, 3, 9, 0, 1},
    {2, 7, 9, 3, 8, 0, 6, 4, 1, 5},
    {7, 0, 4, 6, 9, 1, 3, 2, 5, 8}
};

static int pdrx_verhoeff_check(const int *digits, size_t count) {
    int c = 0;
    for (size_t i = 0; i < count; ++i) {
        int digit = digits[count - 1 - i];
        c = pdrx_verhoeff_d[c][pdrx_verhoeff_p[i % 8][digit]];
    }
    return c == 0;
}

static int pdrx_match_india_aadhaar(const char *buffer,
                                    size_t buffer_len,
                                    size_t pos,
                                    size_t *match_len) {
    if (!isdigit((unsigned char)buffer[pos])) {
        return 0;
    }
    if (!pdrx_is_boundary_before(buffer, buffer_len, pos)) {
        return 0;
    }
    int digits[12];
    size_t digit_index = 0;
    size_t idx = pos;
    while (idx < buffer_len && digit_index < 12) {
        if (isdigit((unsigned char)buffer[idx])) {
            digits[digit_index++] = buffer[idx] - '0';
            idx++;
            continue;
        }
        if (buffer[idx] == ' ') {
            idx++;
            continue;
        }
        return 0;
    }
    if (digit_index != 12) {
        return 0;
    }
    size_t end = idx;
    if (!pdrx_is_boundary_after(buffer, buffer_len, end)) {
        return 0;
    }
    if (!pdrx_verhoeff_check(digits, 12)) {
        return 0;
    }
    *match_len = end - pos;
    return 1;
}

static int pdrx_match_pii(const char *buffer,
                          size_t buffer_len,
                          size_t pos,
                          size_t *match_len) {
    if (pdrx_match_us_ssn(buffer, buffer_len, pos, match_len)) {
        return 1;
    }
    if (pdrx_match_partial_ssn(buffer, buffer_len, pos, match_len)) {
        return 1;
    }
    if (pdrx_match_uk_nino(buffer, buffer_len, pos, match_len)) {
        return 1;
    }
    if (pdrx_match_canada_sin(buffer, buffer_len, pos, match_len)) {
        return 1;
    }
    if (pdrx_match_india_aadhaar(buffer, buffer_len, pos, match_len)) {
        return 1;
    }
    return 0;
}

static void pdrx_redact_span(char *buffer,
                             size_t offset,
                             size_t span_len,
                             pdrx_report_t *report) {
    memset(buffer + offset, 'X', span_len);
    report->match_count++;
    report->bytes_redacted += span_len;
}

static void pdrx_redact_buffer(char *buffer,
                               size_t process_len,
                               size_t buffer_len,
                               const pdrx_plan_t *plan,
                               pdrx_report_t *report) {
    if (process_len == 0 || plan->redaction_count == 0) {
        if (process_len == 0) {
            return;
        }
    }

    for (size_t i = 0; i < process_len; ++i) {
        size_t pii_len = 0;
        int matched_literal = 0;
        for (size_t p = 0; p < plan->redaction_count; ++p) {
            size_t pat_len = plan->pattern_lengths[p];
            if (pat_len == 0 || i + pat_len > buffer_len) {
                continue;
            }
            if (memcmp(buffer + i, plan->patterns[p], pat_len) == 0) {
                pdrx_redact_span(buffer, i, pat_len, report);
                i += pat_len - 1;
                matched_literal = 1;
                break;
            }
        }
        if (matched_literal) {
            continue;
        }
        if (pdrx_match_pii(buffer, buffer_len, i, &pii_len)) {
            pdrx_redact_span(buffer, i, pii_len, report);
            i += pii_len - 1;
        }
    }
}

pdrx_result_t pdrx_apply_file(const char *input_path,
                              const char *output_path,
                              const pdrx_plan_t *plan,
                              pdrx_report_t *report) {
    if (!input_path || !output_path || !plan || !report) {
        return PDRX_ERR_INVALID_ARGUMENT;
    }

    pdrx_report_init(report);

    int input_fd = open(input_path, O_RDONLY);
    if (input_fd < 0) {
        return errno == ENOENT ? PDRX_ERR_NOT_FOUND : PDRX_ERR_IO;
    }

    struct stat st;
    if (fstat(input_fd, &st) != 0) {
        close(input_fd);
        return PDRX_ERR_IO;
    }

    pdrx_result_t version_result = pdrx_scan_version(input_fd, report);
    if (version_result != PDRX_OK) {
        close(input_fd);
        return version_result;
    }

    if (lseek(input_fd, 0, SEEK_SET) == (off_t)-1) {
        close(input_fd);
        return PDRX_ERR_IO;
    }

    int output_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (output_fd < 0) {
        close(input_fd);
        return PDRX_ERR_IO;
    }

    size_t max_len = 0;
    for (size_t i = 0; i < plan->redaction_count; ++i) {
        if (plan->pattern_lengths[i] > max_len) {
            max_len = plan->pattern_lengths[i];
        }
    }
    if (PDRX_MAX_PII_LEN > max_len) {
        max_len = PDRX_MAX_PII_LEN;
    }
    size_t overlap = max_len > 0 ? max_len - 1 : 0;

    size_t chunk_size = 32768;
    char *buffer = malloc(chunk_size + overlap);
    if (!buffer) {
        close(input_fd);
        close(output_fd);
        return PDRX_ERR_IO;
    }

    size_t carry = 0;
    ssize_t bytes_read = 0;
    while ((bytes_read = read(input_fd, buffer + carry, chunk_size)) > 0) {
        size_t total = carry + (size_t)bytes_read;
        size_t process_len = total > overlap ? total - overlap : 0;

        if (process_len > 0) {
            pdrx_redact_buffer(buffer, process_len, total, plan, report);
            size_t written_total = 0;
            while (written_total < process_len) {
                ssize_t written = write(output_fd, buffer + written_total, process_len - written_total);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    free(buffer);
                    close(input_fd);
                    close(output_fd);
                    return PDRX_ERR_IO;
                }
                written_total += (size_t)written;
            }
        }

        carry = total - process_len;
        if (carry > 0) {
            memmove(buffer, buffer + process_len, carry);
        }
        report->bytes_scanned += (size_t)bytes_read;
    }

    if (bytes_read < 0) {
        free(buffer);
        close(input_fd);
        close(output_fd);
        return PDRX_ERR_IO;
    }

    if (carry > 0) {
        pdrx_redact_buffer(buffer, carry, carry, plan, report);
        size_t written_total = 0;
        while (written_total < carry) {
            ssize_t written = write(output_fd, buffer + written_total, carry - written_total);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                free(buffer);
                close(input_fd);
                close(output_fd);
                return PDRX_ERR_IO;
            }
            written_total += (size_t)written;
        }
    }

    free(buffer);
    close(input_fd);
    if (fsync(output_fd) != 0) {
        close(output_fd);
        return PDRX_ERR_IO;
    }
    close(output_fd);
    return PDRX_OK;
}

pdrx_result_t pdrx_report_to_json(const pdrx_report_t *report,
                                  const pdrx_plan_t *plan,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out) {
    if (!report || !plan || !buffer || buffer_len == 0) {
        return PDRX_ERR_INVALID_ARGUMENT;
    }

    int written = snprintf(buffer, buffer_len,
                           "{"
                           "\"redaction_status\":\"complete\","
                           "\"pdf_version\":\"%d.%d\","
                           "\"patterns\":%zu,"
                           "\"matches\":%zu,"
                           "\"bytes_redacted\":%zu,"
                           "\"bytes_scanned\":%zu"
                           "}",
                           report->pdf_version_major,
                           report->pdf_version_minor,
                           plan->redaction_count,
                           report->match_count,
                           report->bytes_redacted,
                           report->bytes_scanned);
    if (written < 0 || (size_t)written >= buffer_len) {
        return PDRX_ERR_BUFFER_TOO_SMALL;
    }
    if (written_out) {
        *written_out = (size_t)written;
    }
    return PDRX_OK;
}

const char *pdrx_result_str(pdrx_result_t result) {
    switch (result) {
        case PDRX_OK:
            return "ok";
        case PDRX_ERR_INVALID_ARGUMENT:
            return "invalid_argument";
        case PDRX_ERR_IO:
            return "io_error";
        case PDRX_ERR_PARSE:
            return "parse_error";
        case PDRX_ERR_BUFFER_TOO_SMALL:
            return "buffer_too_small";
        case PDRX_ERR_NOT_FOUND:
            return "not_found";
        default:
            return "unknown_error";
    }
}
