#include "pap/job_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage:\n");
    printf("  job_queue_cli init <root>\n");
    printf("  job_queue_cli submit <root> <uuid> <pdf> <metadata> [--priority]\n");
    printf("  job_queue_cli claim <root> [--prefer-priority]\n");
    printf("  job_queue_cli release <root> <uuid> <state>\n");
    printf("  job_queue_cli finalize <root> <uuid> <from_state> <to_state>\n");
    printf("  job_queue_cli move <root> <uuid> <from_state> <to_state>\n");
    printf("  job_queue_cli stats <root>\n");
}

static const char *state_to_string(jq_state_t state) {
    switch (state) {
        case JQ_STATE_JOBS:
            return "jobs";
        case JQ_STATE_PRIORITY:
            return "priority";
        case JQ_STATE_COMPLETE:
            return "complete";
        case JQ_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static int parse_state(const char *value, jq_state_t *state_out) {
    if (!value || !state_out) {
        return 0;
    }
    if (strcmp(value, "jobs") == 0) {
        *state_out = JQ_STATE_JOBS;
        return 1;
    }
    if (strcmp(value, "priority") == 0) {
        *state_out = JQ_STATE_PRIORITY;
        return 1;
    }
    if (strcmp(value, "complete") == 0) {
        *state_out = JQ_STATE_COMPLETE;
        return 1;
    }
    if (strcmp(value, "error") == 0) {
        *state_out = JQ_STATE_ERROR;
        return 1;
    }
    return 0;
}

static int exit_for_result(jq_result_t result) {
    switch (result) {
        case JQ_OK:
            return 0;
        case JQ_ERR_NOT_FOUND:
            return 2;
        case JQ_ERR_INVALID_ARGUMENT:
            fprintf(stderr, "invalid arguments\n");
            return 1;
        case JQ_ERR_IO:
        default:
            fprintf(stderr, "io error\n");
            return 1;
    }
}

static void print_state_stats(const char *label, const jq_state_stats_t *stats) {
    printf("%s:\n", label);
    printf("  pdf=%zu metadata=%zu report=%zu\n", stats->pdf_jobs, stats->metadata_jobs, stats->report_jobs);
    printf("  locked_pdf=%zu locked_metadata=%zu locked_report=%zu\n",
           stats->pdf_locked, stats->metadata_locked, stats->report_locked);
    printf("  orphan_pdf=%zu orphan_metadata=%zu orphan_report=%zu\n",
           stats->orphan_pdf, stats->orphan_metadata, stats->orphan_report);
    printf("  bytes_pdf=%llu bytes_metadata=%llu bytes_report=%llu\n",
           stats->pdf_bytes, stats->metadata_bytes, stats->report_bytes);
}

static int handle_stats(const char *root) {
    jq_stats_t stats;
    jq_result_t result = jq_collect_stats(root, &stats);
    if (result != JQ_OK) {
        return exit_for_result(result);
    }

    print_state_stats("jobs", &stats.states[JQ_STATE_JOBS]);
    print_state_stats("priority", &stats.states[JQ_STATE_PRIORITY]);
    print_state_stats("complete", &stats.states[JQ_STATE_COMPLETE]);
    print_state_stats("error", &stats.states[JQ_STATE_ERROR]);
    printf("totals: files=%zu locked=%zu orphans=%zu bytes=%llu oldest_mtime=%lld newest_mtime=%lld\n",
           stats.total_jobs, stats.total_locked, stats.total_orphans, stats.total_bytes,
           (long long)stats.oldest_mtime, (long long)stats.newest_mtime);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "init") == 0) {
        if (argc != 3) {
            print_usage();
            return 1;
        }
        return exit_for_result(jq_init(argv[2]));
    }

    if (strcmp(command, "submit") == 0) {
        if (argc < 6 || argc > 7) {
            print_usage();
            return 1;
        }
        int priority = 0;
        if (argc == 7) {
            if (strcmp(argv[6], "--priority") != 0) {
                print_usage();
                return 1;
            }
            priority = 1;
        }
        return exit_for_result(jq_submit(argv[2], argv[3], argv[4], argv[5], priority));
    }

    if (strcmp(command, "claim") == 0) {
        if (argc < 3 || argc > 4) {
            print_usage();
            return 1;
        }
        int prefer_priority = 0;
        if (argc == 4) {
            if (strcmp(argv[3], "--prefer-priority") != 0) {
                print_usage();
                return 1;
            }
            prefer_priority = 1;
        }
        char uuid[128];
        jq_state_t state = JQ_STATE_JOBS;
        jq_result_t result = jq_claim_next(argv[2], prefer_priority, uuid, sizeof(uuid), &state);
        if (result == JQ_OK) {
            printf("%s %s\n", uuid, state_to_string(state));
        }
        return exit_for_result(result);
    }

    if (strcmp(command, "release") == 0) {
        if (argc != 5) {
            print_usage();
            return 1;
        }
        jq_state_t state;
        if (!parse_state(argv[4], &state)) {
            print_usage();
            return 1;
        }
        return exit_for_result(jq_release(argv[2], argv[3], state));
    }

    if (strcmp(command, "finalize") == 0) {
        if (argc != 6) {
            print_usage();
            return 1;
        }
        jq_state_t from_state;
        jq_state_t to_state;
        if (!parse_state(argv[4], &from_state) || !parse_state(argv[5], &to_state)) {
            print_usage();
            return 1;
        }
        return exit_for_result(jq_finalize(argv[2], argv[3], from_state, to_state));
    }

    if (strcmp(command, "move") == 0) {
        if (argc != 6) {
            print_usage();
            return 1;
        }
        jq_state_t from_state;
        jq_state_t to_state;
        if (!parse_state(argv[4], &from_state) || !parse_state(argv[5], &to_state)) {
            print_usage();
            return 1;
        }
        return exit_for_result(jq_move(argv[2], argv[3], from_state, to_state));
    }

    if (strcmp(command, "stats") == 0) {
        if (argc != 3) {
            print_usage();
            return 1;
        }
        return handle_stats(argv[2]);
    }

    print_usage();
    return 1;
}
