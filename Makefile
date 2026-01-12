CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_XOPEN_SOURCE=700

INCLUDES = -Iinclude

LIB_SOURCES = src/job_queue.c src/pdf_accessibility.c src/pdf_ocr.c src/pdf_redaction.c
CLI_SOURCES = src/job_queue_cli.c
ANALYZE_SOURCES = src/job_queue_analyze.c
OCR_SOURCES = src/job_queue_ocr.c
REDACT_SOURCES = src/job_queue_redact.c
HTTP_SOURCES = src/job_queue_http.c
TEST_SOURCES = tests/test_job_queue.c
PDF_TEST_SOURCES = tests/test_pdf_accessibility.c
OCR_TEST_SOURCES = tests/test_job_queue_ocr.c
REDACT_TEST_SOURCES = tests/test_job_queue_redact.c
PDF_OCR_TEST_SOURCES = tests/test_pdf_ocr.c
PDF_REDACT_TEST_SOURCES = tests/test_pdf_redaction.c
CLI_TEST_SOURCES = tests/test_job_queue_cli.c
ANALYZE_TEST_SOURCES = tests/test_job_queue_analyze.c
HTTP_TEST_SOURCES = tests/test_job_queue_http.c

LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
CLI_OBJECTS = $(CLI_SOURCES:.c=.o)
ANALYZE_OBJECTS = $(ANALYZE_SOURCES:.c=.o)
OCR_OBJECTS = $(OCR_SOURCES:.c=.o)
REDACT_OBJECTS = $(REDACT_SOURCES:.c=.o)
HTTP_OBJECTS = $(HTTP_SOURCES:.c=.o)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)
PDF_TEST_OBJECTS = $(PDF_TEST_SOURCES:.c=.o)
OCR_TEST_OBJECTS = $(OCR_TEST_SOURCES:.c=.o)
REDACT_TEST_OBJECTS = $(REDACT_TEST_SOURCES:.c=.o)
PDF_OCR_TEST_OBJECTS = $(PDF_OCR_TEST_SOURCES:.c=.o)
PDF_REDACT_TEST_OBJECTS = $(PDF_REDACT_TEST_SOURCES:.c=.o)
CLI_TEST_OBJECTS = $(CLI_TEST_SOURCES:.c=.o)
ANALYZE_TEST_OBJECTS = $(ANALYZE_TEST_SOURCES:.c=.o)
HTTP_TEST_OBJECTS = $(HTTP_TEST_SOURCES:.c=.o)

TEST_BIN = tests/test_job_queue
PDF_TEST_BIN = tests/test_pdf_accessibility
PDF_OCR_TEST_BIN = tests/test_pdf_ocr
PDF_REDACT_TEST_BIN = tests/test_pdf_redaction
CLI_TEST_BIN = tests/test_job_queue_cli
CLI_BIN = job_queue_cli
ANALYZE_TEST_BIN = tests/test_job_queue_analyze
ANALYZE_BIN = job_queue_analyze
OCR_TEST_BIN = tests/test_job_queue_ocr
OCR_BIN = job_queue_ocr
REDACT_TEST_BIN = tests/test_job_queue_redact
REDACT_BIN = job_queue_redact
HTTP_TEST_BIN = tests/test_job_queue_http
HTTP_UNIT_TEST_BIN = tests/test_job_queue_http_unit
HTTP_BIN = job_queue_http

MAKEFILE_PROCESSORS = ocr redact analyze accessible
FILE ?= $(word 2,$(MAKECMDGOALS))

ifneq ($(filter $(MAKEFILE_PROCESSORS),$(MAKECMDGOALS)),)
ifneq ($(strip $(FILE)),)
$(FILE):
	@:
endif
endif

.PHONY: all test clean $(MAKEFILE_PROCESSORS)

all: $(TEST_BIN) $(PDF_TEST_BIN) $(PDF_OCR_TEST_BIN) $(PDF_REDACT_TEST_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(ANALYZE_BIN) $(ANALYZE_TEST_BIN) $(OCR_BIN) $(OCR_TEST_BIN) $(REDACT_BIN) $(REDACT_TEST_BIN) $(HTTP_BIN) $(HTTP_TEST_BIN) $(HTTP_UNIT_TEST_BIN)

$(TEST_BIN): $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(TEST_OBJECTS) -o $(TEST_BIN)

$(PDF_TEST_BIN): $(LIB_OBJECTS) $(PDF_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(PDF_TEST_OBJECTS) -o $(PDF_TEST_BIN)

$(PDF_OCR_TEST_BIN): $(LIB_OBJECTS) $(PDF_OCR_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(PDF_OCR_TEST_OBJECTS) -o $(PDF_OCR_TEST_BIN)

$(PDF_REDACT_TEST_BIN): $(LIB_OBJECTS) $(PDF_REDACT_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(PDF_REDACT_TEST_OBJECTS) -o $(PDF_REDACT_TEST_BIN)

$(CLI_TEST_BIN): $(LIB_OBJECTS) $(CLI_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(CLI_TEST_OBJECTS) -o $(CLI_TEST_BIN)

$(CLI_BIN): $(LIB_OBJECTS) $(CLI_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(CLI_OBJECTS) -o $(CLI_BIN)

$(ANALYZE_BIN): $(LIB_OBJECTS) $(ANALYZE_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(ANALYZE_OBJECTS) -o $(ANALYZE_BIN)

$(ANALYZE_TEST_BIN): $(LIB_OBJECTS) $(ANALYZE_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(ANALYZE_TEST_OBJECTS) -o $(ANALYZE_TEST_BIN)

$(OCR_BIN): $(LIB_OBJECTS) $(OCR_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(OCR_OBJECTS) -o $(OCR_BIN)

$(OCR_TEST_BIN): $(LIB_OBJECTS) $(OCR_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(OCR_TEST_OBJECTS) -o $(OCR_TEST_BIN)

$(REDACT_BIN): $(LIB_OBJECTS) $(REDACT_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(REDACT_OBJECTS) -o $(REDACT_BIN)

$(REDACT_TEST_BIN): $(LIB_OBJECTS) $(REDACT_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(REDACT_TEST_OBJECTS) -o $(REDACT_TEST_BIN)

$(HTTP_BIN): $(LIB_OBJECTS) $(HTTP_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(HTTP_OBJECTS) -o $(HTTP_BIN)

$(HTTP_TEST_BIN): $(LIB_OBJECTS) $(HTTP_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(HTTP_TEST_OBJECTS) -o $(HTTP_TEST_BIN)

$(HTTP_UNIT_TEST_BIN): $(LIB_OBJECTS) $(HTTP_SOURCES)
	$(CC) $(CFLAGS) -Wno-unused-function $(INCLUDES) -DJQ_HTTP_TEST $(LIB_OBJECTS) $(HTTP_SOURCES) -o $(HTTP_UNIT_TEST_BIN)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(TEST_BIN) $(PDF_TEST_BIN) $(PDF_OCR_TEST_BIN) $(PDF_REDACT_TEST_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(ANALYZE_BIN) $(ANALYZE_TEST_BIN) $(OCR_BIN) $(OCR_TEST_BIN) $(REDACT_BIN) $(REDACT_TEST_BIN) $(HTTP_BIN) $(HTTP_TEST_BIN) $(HTTP_UNIT_TEST_BIN)
	./$(TEST_BIN)
	./$(PDF_TEST_BIN)
	./$(PDF_OCR_TEST_BIN)
	./$(PDF_REDACT_TEST_BIN)
	./$(CLI_TEST_BIN)
	./$(ANALYZE_TEST_BIN)
	./$(OCR_TEST_BIN)
	./$(REDACT_TEST_BIN)
	./$(HTTP_TEST_BIN)
	./$(HTTP_UNIT_TEST_BIN)
	sh tests/integration_http.sh
	sh tests/test_docs.sh
	sh tests/test_html_report.sh
	sh tests/test_demo_scripts.sh
	sh tests/test_make_targets.sh

define RUN_JOB_PROCESSOR
	@set -eu; \
	if [ -z "$(FILE)" ]; then \
		echo "Usage: make $(1) <pdf-file>"; \
		exit 2; \
	fi; \
	if [ ! -f "$(FILE)" ]; then \
		echo "PDF not found: $(FILE)"; \
		exit 2; \
	fi; \
	ROOT_DIR="$$(mktemp -d 2>/dev/null || mktemp -d -t pap_job)"; \
	UUID="$$(date +%s%N)"; \
	META_PATH="$$ROOT_DIR/job.metadata.json"; \
	$(2); \
	./job_queue_cli init "$$ROOT_DIR" >/dev/null; \
	./job_queue_cli submit "$$ROOT_DIR" "$$UUID" "$(FILE)" "$$META_PATH"; \
	./$(3) "$$ROOT_DIR"; \
	OUT_PDF="$$ROOT_DIR/complete/$$UUID.pdf.job"; \
	OUT_META="$$ROOT_DIR/complete/$$UUID.metadata.job"; \
	if [ ! -f "$$OUT_PDF" ] || [ ! -f "$$OUT_META" ]; then \
		echo "Output missing in $$ROOT_DIR"; \
		exit 1; \
	fi; \
	printf 'Output root: %s\n' "$$ROOT_DIR"; \
	printf 'PDF output: %s\n' "$$OUT_PDF"; \
	printf 'Metadata output: %s\n' "$$OUT_META"; \
	if [ -f "$$ROOT_DIR/complete/$$UUID.report.html" ]; then \
		printf 'Report output: %s\n' "$$ROOT_DIR/complete/$$UUID.report.html"; \
	fi
endef

ocr: $(OCR_BIN) $(CLI_BIN)
	$(call RUN_JOB_PROCESSOR,ocr,printf '%s' '{}' > "$$META_PATH",job_queue_ocr)

redact: $(REDACT_BIN) $(CLI_BIN)
	$(call RUN_JOB_PROCESSOR,redact,printf '%s' '{"redactions":["SECRET"]}' > "$$META_PATH",job_queue_redact)

analyze: $(ANALYZE_BIN) $(CLI_BIN)
	$(call RUN_JOB_PROCESSOR,analyze,printf '%s' '{}' > "$$META_PATH",job_queue_analyze)

accessible: analyze

clean:
	rm -f $(LIB_OBJECTS) $(CLI_OBJECTS) $(ANALYZE_OBJECTS) $(HTTP_OBJECTS) $(TEST_OBJECTS) $(CLI_TEST_OBJECTS) \
		$(ANALYZE_TEST_OBJECTS) $(OCR_OBJECTS) $(REDACT_OBJECTS) $(OCR_TEST_OBJECTS) $(REDACT_TEST_OBJECTS) $(PDF_OCR_TEST_OBJECTS) $(PDF_REDACT_TEST_OBJECTS) \
		$(HTTP_TEST_OBJECTS) $(PDF_TEST_OBJECTS) $(TEST_BIN) $(PDF_TEST_BIN) $(PDF_OCR_TEST_BIN) $(PDF_REDACT_TEST_BIN) $(CLI_TEST_BIN) $(ANALYZE_TEST_BIN) \
		$(OCR_TEST_BIN) $(REDACT_TEST_BIN) $(HTTP_TEST_BIN) $(CLI_BIN) $(ANALYZE_BIN) $(OCR_BIN) $(REDACT_BIN) $(HTTP_BIN) $(HTTP_UNIT_TEST_BIN)
