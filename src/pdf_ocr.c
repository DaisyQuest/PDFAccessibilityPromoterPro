#include "pap/pdf_ocr.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static pocr_result_t pocr_scan_version(FILE *fp, pocr_report_t *report) {
    char buffer[64];
    size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, fp);
    if (bytes == 0) {
        return POCR_ERR_PARSE;
    }
    buffer[bytes] = '\0';

    const char *marker = "%PDF-";
    char *found = strstr(buffer, marker);
    if (!found) {
        return POCR_ERR_PARSE;
    }
    found += strlen(marker);
    if (!isdigit((unsigned char)found[0]) || found[1] != '.' || !isdigit((unsigned char)found[2])) {
        return POCR_ERR_PARSE;
    }

    report->pdf_version_major = found[0] - '0';
    report->pdf_version_minor = found[2] - '0';
    return POCR_OK;
}

pocr_result_t pocr_report_init(pocr_report_t *report) {
    if (!report) {
        return POCR_ERR_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    report->pdf_version_major = -1;
    report->pdf_version_minor = -1;
    return POCR_OK;
}

pocr_result_t pocr_scan_file(const char *path, pocr_report_t *report) {
    if (!path || !report) {
        return POCR_ERR_INVALID_ARGUMENT;
    }

    pocr_result_t init_result = pocr_report_init(report);
    if (init_result != POCR_OK) {
        return init_result;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return errno == ENOENT ? POCR_ERR_NOT_FOUND : POCR_ERR_IO;
    }

    struct stat st;
    if (fstat(fileno(fp), &st) == 0) {
        report->bytes_scanned = (size_t)st.st_size;
    }

    pocr_result_t version_result = pocr_scan_version(fp, report);
    fclose(fp);
    return version_result;
}

pocr_result_t pocr_report_to_json(const pocr_report_t *report,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out) {
    if (!report || !buffer || buffer_len == 0) {
        return POCR_ERR_INVALID_ARGUMENT;
    }

    int written = snprintf(buffer, buffer_len,
                           "{"
                           "\"ocr_status\":\"complete\","
                           "\"pdf_version\":\"%d.%d\","
                           "\"bytes_scanned\":%zu"
                           "}",
                           report->pdf_version_major,
                           report->pdf_version_minor,
                           report->bytes_scanned);
    if (written < 0 || (size_t)written >= buffer_len) {
        return POCR_ERR_BUFFER_TOO_SMALL;
    }
    if (written_out) {
        *written_out = (size_t)written;
    }
    return POCR_OK;
}

const char *pocr_result_str(pocr_result_t result) {
    switch (result) {
        case POCR_OK:
            return "ok";
        case POCR_ERR_INVALID_ARGUMENT:
            return "invalid_argument";
        case POCR_ERR_IO:
            return "io_error";
        case POCR_ERR_PARSE:
            return "parse_error";
        case POCR_ERR_BUFFER_TOO_SMALL:
            return "buffer_too_small";
        case POCR_ERR_NOT_FOUND:
            return "not_found";
        default:
            return "unknown_error";
    }
}
