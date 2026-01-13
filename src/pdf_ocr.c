#include "pap/pdf_ocr.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#define POCR_MAX_PROVIDERS 16
#define POCR_MARKER_SCAN_CHUNK 4096

typedef struct {
    const char *token;
    int weight;
} pocr_marker_t;

static pocr_provider_t pocr_providers[POCR_MAX_PROVIDERS];
static size_t pocr_provider_count_value = 0;
static int pocr_registry_initialized = 0;

static pocr_log_fn pocr_logger = NULL;
static void *pocr_logger_data = NULL;
static int pocr_log_level_initialized = 0;
static pocr_log_level_t pocr_env_log_level = POCR_LOG_WARN;

static const char *pocr_log_level_str_internal(pocr_log_level_t level) {
    switch (level) {
        case POCR_LOG_DEBUG:
            return "debug";
        case POCR_LOG_INFO:
            return "info";
        case POCR_LOG_WARN:
            return "warn";
        case POCR_LOG_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static pocr_log_level_t pocr_parse_log_level(const char *value) {
    if (!value || value[0] == '\0') {
        return POCR_LOG_WARN;
    }
    if (strcasecmp(value, "debug") == 0) {
        return POCR_LOG_DEBUG;
    }
    if (strcasecmp(value, "info") == 0) {
        return POCR_LOG_INFO;
    }
    if (strcasecmp(value, "warn") == 0 || strcasecmp(value, "warning") == 0) {
        return POCR_LOG_WARN;
    }
    if (strcasecmp(value, "error") == 0) {
        return POCR_LOG_ERROR;
    }
    return POCR_LOG_WARN;
}

static pocr_log_level_t pocr_get_env_log_level(void) {
    if (!pocr_log_level_initialized) {
        const char *value = getenv("PAP_OCR_LOG_LEVEL");
        pocr_env_log_level = pocr_parse_log_level(value);
        pocr_log_level_initialized = 1;
    }
    return pocr_env_log_level;
}

static void pocr_log_message(pocr_log_level_t level, const char *format, ...) {
    if (pocr_logger) {
        char buffer[512];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        pocr_logger(level, buffer, pocr_logger_data);
        return;
    }

    if (level < pocr_get_env_log_level()) {
        return;
    }

    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    fprintf(stderr, "[OCR][%s] %s\n", pocr_log_level_str_internal(level), buffer);
}

static int pocr_chars_equal_ci(char left, char right) {
    return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static size_t pocr_count_marker_hits_window(const char *buffer,
                                            size_t buffer_len,
                                            const char *needle,
                                            size_t needle_len,
                                            size_t carry_len) {
    if (!buffer || !needle || needle_len == 0 || buffer_len < needle_len) {
        return 0;
    }

    size_t hits = 0;
    for (size_t i = 0; i + needle_len <= buffer_len; ++i) {
        size_t j = 0;
        for (; j < needle_len; ++j) {
            if (!pocr_chars_equal_ci(buffer[i + j], needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            if (i + needle_len > carry_len) {
                hits++;
            }
            i += needle_len - 1;
        }
    }
    return hits;
}

static void pocr_scan_handwriting_markers(FILE *fp, pocr_report_t *report) {
    static const pocr_marker_t markers[] = {
        { "/Subtype/Ink", 45 },
        { "InkList", 30 },
        { "/Ink", 20 },
        { "/Sig", 25 },
        { "Signature", 25 },
        { "Handwriting", 35 },
        { "Handwritten", 35 },
        { "/FreeText", 15 },
        { "/Stamp", 10 },
        { "/Annot", 10 },
        { "/Annots", 10 }
    };

    if (!fp || !report) {
        return;
    }

    size_t max_marker_len = 0;
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        size_t len = strlen(markers[i].token);
        if (len > max_marker_len) {
            max_marker_len = len;
        }
    }

    if (max_marker_len == 0) {
        report->handwriting_marker_hits = 0;
        report->handwriting_confidence = 0;
        return;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        report->handwriting_marker_hits = 0;
        report->handwriting_confidence = 0;
        return;
    }

    char chunk[POCR_MARKER_SCAN_CHUNK];
    size_t window_cap = POCR_MARKER_SCAN_CHUNK + max_marker_len;
    char *window = malloc(window_cap);
    char *carry = NULL;
    size_t carry_len = 0;
    if (max_marker_len > 0) {
        carry = malloc(max_marker_len);
    }
    if (!window || (max_marker_len > 0 && !carry)) {
        free(window);
        free(carry);
        report->handwriting_marker_hits = 0;
        report->handwriting_confidence = 0;
        return;
    }
    size_t marker_hits[sizeof(markers) / sizeof(markers[0])];
    memset(marker_hits, 0, sizeof(marker_hits));

    size_t read_bytes = 0;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        size_t window_len = 0;
        if (carry_len > 0) {
            memcpy(window, carry, carry_len);
            window_len = carry_len;
        }
        memcpy(window + window_len, chunk, read_bytes);
        window_len += read_bytes;

        for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
            marker_hits[i] += pocr_count_marker_hits_window(window,
                                                            window_len,
                                                            markers[i].token,
                                                            strlen(markers[i].token),
                                                            carry_len);
        }

        if (window_len >= max_marker_len) {
            carry_len = max_marker_len - 1;
            memcpy(carry, window + window_len - carry_len, carry_len);
        } else {
            carry_len = window_len;
            memcpy(carry, window, carry_len);
        }
    }
    free(window);
    free(carry);

    size_t total_hits = 0;
    unsigned int score = 0;
    int has_ink = 0;
    int has_signature = 0;
    int has_text = 0;
    int has_annotation = 0;
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        total_hits += marker_hits[i];
        if (marker_hits[i] > 0) {
            score += (unsigned int)markers[i].weight;
        }
        if (marker_hits[i] > 1) {
            score += (unsigned int)(markers[i].weight / 2);
        }
        if (marker_hits[i] > 2) {
            score += (unsigned int)(markers[i].weight / 4);
        }

        if (marker_hits[i] > 0) {
            if (strcmp(markers[i].token, "/Subtype/Ink") == 0 ||
                strcmp(markers[i].token, "InkList") == 0 ||
                strcmp(markers[i].token, "/Ink") == 0) {
                has_ink = 1;
            } else if (strcmp(markers[i].token, "/Sig") == 0 ||
                       strcmp(markers[i].token, "Signature") == 0) {
                has_signature = 1;
            } else if (strcmp(markers[i].token, "Handwriting") == 0 ||
                       strcmp(markers[i].token, "Handwritten") == 0) {
                has_text = 1;
            } else if (strcmp(markers[i].token, "/Annot") == 0 ||
                       strcmp(markers[i].token, "/Annots") == 0 ||
                       strcmp(markers[i].token, "/FreeText") == 0 ||
                       strcmp(markers[i].token, "/Stamp") == 0) {
                has_annotation = 1;
            }
        }
    }

    if (total_hits == 0) {
        report->handwriting_marker_hits = 0;
        report->handwriting_confidence = 0;
        return;
    }

    if (has_ink && has_signature) {
        score += 10;
    }
    if (has_ink && has_text) {
        score += 10;
    }
    if (has_signature && has_text) {
        score += 5;
    }
    if (has_annotation && (has_ink || has_signature)) {
        score += 5;
    }

    if (score > 100) {
        score = 100;
    }

    report->handwriting_marker_hits = total_hits;
    report->handwriting_confidence = score;
}

static pocr_result_t pocr_register_provider_internal(const pocr_provider_t *provider, int log_errors) {
    if (!provider || !provider->name || provider->name[0] == '\0' || !provider->scan_file) {
        if (log_errors) {
            pocr_log_message(POCR_LOG_ERROR, "Invalid provider registration request.");
        }
        return POCR_ERR_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < pocr_provider_count_value; ++i) {
        if (strcmp(pocr_providers[i].name, provider->name) == 0) {
            if (log_errors) {
                pocr_log_message(POCR_LOG_WARN, "Provider '%s' already registered.", provider->name);
            }
            return POCR_ERR_PROVIDER_EXISTS;
        }
    }

    if (pocr_provider_count_value >= POCR_MAX_PROVIDERS) {
        if (log_errors) {
            pocr_log_message(POCR_LOG_ERROR, "Provider registry limit reached.");
        }
        return POCR_ERR_PROVIDER_LIMIT;
    }

    pocr_providers[pocr_provider_count_value] = *provider;
    pocr_provider_count_value++;
    if (log_errors) {
        pocr_log_message(POCR_LOG_INFO, "Registered OCR provider '%s'.", provider->name);
    }
    return POCR_OK;
}

static pocr_result_t pocr_builtin_scan(const char *path, pocr_report_t *report, void *user_data) {
    (void)user_data;
    if (!path || !report) {
        return POCR_ERR_INVALID_ARGUMENT;
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
    pocr_scan_handwriting_markers(fp, report);
    fclose(fp);
    return version_result;
}

static void pocr_init_registry(void) {
    if (pocr_registry_initialized) {
        return;
    }
    pocr_registry_initialized = 1;
    pocr_provider_t builtin = {
        .name = "builtin",
        .scan_file = pocr_builtin_scan,
        .user_data = NULL
    };
    (void)pocr_register_provider_internal(&builtin, 0);
}

pocr_result_t pocr_report_init(pocr_report_t *report) {
    if (!report) {
        return POCR_ERR_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    report->pdf_version_major = -1;
    report->pdf_version_minor = -1;
    report->provider_name = NULL;
    report->handwriting_marker_hits = 0;
    report->handwriting_confidence = 0;
    return POCR_OK;
}

pocr_result_t pocr_scan_file(const char *path, pocr_report_t *report) {
    return pocr_scan_file_with_provider(NULL, path, report);
}

pocr_result_t pocr_scan_file_with_provider(const char *provider_name,
                                           const char *path,
                                           pocr_report_t *report) {
    if (!path || !report) {
        return POCR_ERR_INVALID_ARGUMENT;
    }

    pocr_result_t init_result = pocr_report_init(report);
    if (init_result != POCR_OK) {
        return init_result;
    }

    pocr_init_registry();

    const pocr_provider_t *provider = NULL;
    if (provider_name) {
        provider = pocr_find_provider(provider_name);
        if (!provider) {
            pocr_log_message(POCR_LOG_ERROR, "OCR provider '%s' not found.", provider_name);
            return POCR_ERR_PROVIDER_NOT_FOUND;
        }
    } else {
        provider = pocr_default_provider();
    }

    if (!provider) {
        pocr_log_message(POCR_LOG_ERROR, "No OCR providers available.");
        return POCR_ERR_PROVIDER_NOT_FOUND;
    }

    report->provider_name = provider->name;
    pocr_log_message(POCR_LOG_INFO, "Starting OCR scan with provider '%s'.", provider->name);
    pocr_result_t result = provider->scan_file(path, report, provider->user_data);
    if (result != POCR_OK) {
        pocr_log_message(POCR_LOG_ERROR, "OCR scan failed with provider '%s': %s",
                         provider->name, pocr_result_str(result));
    } else {
        pocr_log_message(POCR_LOG_INFO, "OCR scan complete with provider '%s'.", provider->name);
    }
    return result;
}

pocr_result_t pocr_report_to_json(const pocr_report_t *report,
                                  char *buffer,
                                  size_t buffer_len,
                                  size_t *written_out) {
    if (!report || !buffer || buffer_len == 0) {
        return POCR_ERR_INVALID_ARGUMENT;
    }

    const char *provider = report->provider_name ? report->provider_name : "unknown";
    const char *handwriting_detected = report->handwriting_confidence > 0 ? "true" : "false";
    int written = snprintf(buffer, buffer_len,
                           "{"
                           "\"ocr_status\":\"complete\","
                           "\"ocr_provider\":\"%s\","
                           "\"handwriting_detected\":%s,"
                           "\"handwriting_confidence\":%u,"
                           "\"handwriting_markers\":%zu,"
                           "\"pdf_version\":\"%d.%d\","
                           "\"bytes_scanned\":%zu"
                           "}",
                           provider,
                           handwriting_detected,
                           report->handwriting_confidence,
                           report->handwriting_marker_hits,
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
        case POCR_ERR_PROVIDER_NOT_FOUND:
            return "provider_not_found";
        case POCR_ERR_PROVIDER_EXISTS:
            return "provider_exists";
        case POCR_ERR_PROVIDER_LIMIT:
            return "provider_limit";
        default:
            return "unknown_error";
    }
}

void pocr_set_logger(pocr_log_fn logger, void *user_data) {
    pocr_logger = logger;
    pocr_logger_data = user_data;
}

const char *pocr_log_level_str(pocr_log_level_t level) {
    return pocr_log_level_str_internal(level);
}

pocr_result_t pocr_register_provider(const pocr_provider_t *provider) {
    pocr_init_registry();
    return pocr_register_provider_internal(provider, 1);
}

const pocr_provider_t *pocr_find_provider(const char *name) {
    if (!name) {
        return NULL;
    }
    pocr_init_registry();
    for (size_t i = 0; i < pocr_provider_count_value; ++i) {
        if (strcmp(pocr_providers[i].name, name) == 0) {
            return &pocr_providers[i];
        }
    }
    return NULL;
}

const pocr_provider_t *pocr_default_provider(void) {
    pocr_init_registry();
    if (pocr_provider_count_value == 0) {
        return NULL;
    }
    return &pocr_providers[0];
}

size_t pocr_provider_capacity(void) {
    return POCR_MAX_PROVIDERS;
}

size_t pocr_provider_count(void) {
    pocr_init_registry();
    return pocr_provider_count_value;
}
