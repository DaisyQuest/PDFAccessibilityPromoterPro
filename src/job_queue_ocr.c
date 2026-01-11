#include "pap/job_queue.h"
#include "pap/pdf_ocr.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPORT_JSON_INITIAL 1024

static void print_usage(void) {
    printf("Usage:\n");
    printf("  job_queue_ocr <root> [--prefer-priority]\n");
}

static int write_buffer_to_file(const char *path, const char *buffer, size_t length) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    if (length > 0 && fwrite(buffer, 1, length, fp) != length) {
        fclose(fp);
        return 0;
    }
    if (fclose(fp) != 0) {
        return 0;
    }
    return 1;
}

static int write_error_metadata(const char *path, const char *detail) {
    if (!path || !detail) {
        return 0;
    }
    char buffer[256];
    int written = snprintf(buffer, sizeof(buffer),
                           "{\"error\":\"ocr_failed\",\"detail\":\"%s\"}",
                           detail);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return 0;
    }
    return write_buffer_to_file(path, buffer, (size_t)written);
}

static int write_report_json(const pocr_report_t *report, const char *path) {
    size_t buffer_len = REPORT_JSON_INITIAL;
    char *buffer = NULL;
    for (int attempt = 0; attempt < 8; ++attempt) {
        char *resized = realloc(buffer, buffer_len);
        if (!resized) {
            free(buffer);
            return 0;
        }
        buffer = resized;
        size_t written = 0;
        pocr_result_t result = pocr_report_to_json(report, buffer, buffer_len, &written);
        if (result == POCR_OK) {
            int ok = write_buffer_to_file(path, buffer, written);
            free(buffer);
            return ok;
        }
        if (result != POCR_ERR_BUFFER_TOO_SMALL) {
            free(buffer);
            return 0;
        }
        buffer_len *= 2;
    }
    free(buffer);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *root = argv[1];
    int prefer_priority = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--prefer-priority") == 0) {
            prefer_priority = 1;
        } else {
            print_usage();
            return 1;
        }
    }

    char uuid[128];
    jq_state_t state = JQ_STATE_JOBS;
    jq_result_t claim_result = jq_claim_next(root, prefer_priority, uuid, sizeof(uuid), &state);
    if (claim_result == JQ_ERR_NOT_FOUND) {
        return 2;
    }
    if (claim_result != JQ_OK) {
        fprintf(stderr, "Failed to claim job.\n");
        return 1;
    }

    char pdf_locked[PATH_MAX];
    char metadata_locked[PATH_MAX];
    if (jq_job_paths_locked(root, uuid, state, pdf_locked, sizeof(pdf_locked),
                            metadata_locked, sizeof(metadata_locked)) != JQ_OK) {
        fprintf(stderr, "Failed to resolve locked job paths.\n");
        return 1;
    }

    const char *provider_name = getenv("PAP_OCR_PROVIDER");
    if (provider_name && provider_name[0] != '\0') {
        fprintf(stderr, "Using OCR provider '%s'.\n", provider_name);
    }

    pocr_report_t report;
    pocr_result_t scan_result = pocr_scan_file_with_provider(provider_name, pdf_locked, &report);
    if (scan_result != POCR_OK) {
        const char *detail = pocr_result_str(scan_result);
        write_error_metadata(metadata_locked, detail);
        (void)jq_finalize(root, uuid, state, JQ_STATE_ERROR);
        return 1;
    }

    if (!write_report_json(&report, metadata_locked)) {
        write_error_metadata(metadata_locked, "report_write_failed");
        (void)jq_finalize(root, uuid, state, JQ_STATE_ERROR);
        return 1;
    }

    if (jq_finalize(root, uuid, state, JQ_STATE_COMPLETE) != JQ_OK) {
        fprintf(stderr, "Failed to finalize job.\n");
        return 1;
    }

    return 0;
}
