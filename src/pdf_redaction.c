#include "pap/pdf_redaction.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void pdrx_redact_buffer(char *buffer,
                               size_t process_len,
                               const pdrx_plan_t *plan,
                               pdrx_report_t *report) {
    if (process_len == 0 || plan->redaction_count == 0) {
        return;
    }

    for (size_t i = 0; i < process_len; ++i) {
        for (size_t p = 0; p < plan->redaction_count; ++p) {
            size_t pat_len = plan->pattern_lengths[p];
            if (pat_len == 0 || i + pat_len > process_len) {
                continue;
            }
            if (memcmp(buffer + i, plan->patterns[p], pat_len) == 0) {
                memset(buffer + i, 'X', pat_len);
                report->match_count++;
                report->bytes_redacted += pat_len;
                i += pat_len - 1;
                break;
            }
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
            pdrx_redact_buffer(buffer, process_len, plan, report);
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
        pdrx_redact_buffer(buffer, carry, plan, report);
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
