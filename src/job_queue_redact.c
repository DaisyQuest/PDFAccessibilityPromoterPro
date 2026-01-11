#include "pap/job_queue.h"
#include "pap/pdf_redaction.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPORT_JSON_INITIAL 1024
#define METADATA_MAX_SIZE 65536

static void print_usage(void) {
    printf("Usage:\n");
    printf("  job_queue_redact <root> [--prefer-priority]\n");
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
                           "{\"error\":\"redaction_failed\",\"detail\":\"%s\"}",
                           detail);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return 0;
    }
    return write_buffer_to_file(path, buffer, (size_t)written);
}

static int write_report_json(const pdrx_report_t *report, const pdrx_plan_t *plan, const char *path) {
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
        pdrx_result_t result = pdrx_report_to_json(report, plan, buffer, buffer_len, &written);
        if (result == PDRX_OK) {
            int ok = write_buffer_to_file(path, buffer, written);
            free(buffer);
            return ok;
        }
        if (result != PDRX_ERR_BUFFER_TOO_SMALL) {
            free(buffer);
            return 0;
        }
        buffer_len *= 2;
    }
    free(buffer);
    return 0;
}

static int read_metadata_plan(const char *path, pdrx_plan_t *plan) {
    if (!path || !plan) {
        return 0;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long size = ftell(fp);
    if (size < 0 || size > METADATA_MAX_SIZE) {
        fclose(fp);
        return 0;
    }
    rewind(fp);

    size_t length = (size_t)size;
    char *buffer = malloc(length + 1);
    if (!buffer) {
        fclose(fp);
        return 0;
    }
    size_t read_bytes = fread(buffer, 1, length, fp);
    fclose(fp);
    if (read_bytes != length) {
        free(buffer);
        return 0;
    }
    buffer[length] = '\0';

    pdrx_result_t parse_result = pdrx_plan_from_json(buffer, length, plan);
    free(buffer);
    return parse_result == PDRX_OK;
}

static pdrx_result_t replace_pdf_with_redacted(const char *pdf_locked, const pdrx_plan_t *plan, pdrx_report_t *report) {
    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.redact.tmp.XXXXXX", pdf_locked);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        return PDRX_ERR_INVALID_ARGUMENT;
    }
    int temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        return PDRX_ERR_IO;
    }
    close(temp_fd);

    pdrx_result_t redact_result = pdrx_apply_file(pdf_locked, temp_path, plan, report);
    if (redact_result != PDRX_OK) {
        unlink(temp_path);
        return redact_result;
    }

    if (rename(temp_path, pdf_locked) != 0) {
        unlink(temp_path);
        return PDRX_ERR_IO;
    }

    return PDRX_OK;
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

    pdrx_plan_t plan;
    if (!read_metadata_plan(metadata_locked, &plan)) {
        write_error_metadata(metadata_locked, "plan_parse_failed");
        (void)jq_finalize(root, uuid, state, JQ_STATE_ERROR);
        return 1;
    }

    pdrx_report_t report;
    pdrx_result_t redact_result = replace_pdf_with_redacted(pdf_locked, &plan, &report);
    if (redact_result != PDRX_OK) {
        write_error_metadata(metadata_locked, pdrx_result_str(redact_result));
        (void)jq_finalize(root, uuid, state, JQ_STATE_ERROR);
        return 1;
    }

    if (!write_report_json(&report, &plan, metadata_locked)) {
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
