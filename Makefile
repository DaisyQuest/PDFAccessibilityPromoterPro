CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_XOPEN_SOURCE=700

INCLUDES = -Iinclude

LIB_SOURCES = src/job_queue.c src/pdf_accessibility.c
CLI_SOURCES = src/job_queue_cli.c
ANALYZE_SOURCES = src/job_queue_analyze.c
HTTP_SOURCES = src/job_queue_http.c
TEST_SOURCES = tests/test_job_queue.c
PDF_TEST_SOURCES = tests/test_pdf_accessibility.c
CLI_TEST_SOURCES = tests/test_job_queue_cli.c
ANALYZE_TEST_SOURCES = tests/test_job_queue_analyze.c
HTTP_TEST_SOURCES = tests/test_job_queue_http.c

LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
CLI_OBJECTS = $(CLI_SOURCES:.c=.o)
ANALYZE_OBJECTS = $(ANALYZE_SOURCES:.c=.o)
HTTP_OBJECTS = $(HTTP_SOURCES:.c=.o)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)
PDF_TEST_OBJECTS = $(PDF_TEST_SOURCES:.c=.o)
CLI_TEST_OBJECTS = $(CLI_TEST_SOURCES:.c=.o)
ANALYZE_TEST_OBJECTS = $(ANALYZE_TEST_SOURCES:.c=.o)
HTTP_TEST_OBJECTS = $(HTTP_TEST_SOURCES:.c=.o)

TEST_BIN = tests/test_job_queue
PDF_TEST_BIN = tests/test_pdf_accessibility
CLI_TEST_BIN = tests/test_job_queue_cli
CLI_BIN = job_queue_cli
ANALYZE_TEST_BIN = tests/test_job_queue_analyze
ANALYZE_BIN = job_queue_analyze
HTTP_TEST_BIN = tests/test_job_queue_http
HTTP_UNIT_TEST_BIN = tests/test_job_queue_http_unit
HTTP_BIN = job_queue_http

.PHONY: all test clean

all: $(TEST_BIN) $(PDF_TEST_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(ANALYZE_BIN) $(ANALYZE_TEST_BIN) $(HTTP_BIN) $(HTTP_TEST_BIN) $(HTTP_UNIT_TEST_BIN)

$(TEST_BIN): $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(TEST_OBJECTS) -o $(TEST_BIN)

$(PDF_TEST_BIN): $(LIB_OBJECTS) $(PDF_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(PDF_TEST_OBJECTS) -o $(PDF_TEST_BIN)

$(CLI_TEST_BIN): $(LIB_OBJECTS) $(CLI_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(CLI_TEST_OBJECTS) -o $(CLI_TEST_BIN)

$(CLI_BIN): $(LIB_OBJECTS) $(CLI_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(CLI_OBJECTS) -o $(CLI_BIN)

$(ANALYZE_BIN): $(LIB_OBJECTS) $(ANALYZE_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(ANALYZE_OBJECTS) -o $(ANALYZE_BIN)

$(ANALYZE_TEST_BIN): $(LIB_OBJECTS) $(ANALYZE_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(ANALYZE_TEST_OBJECTS) -o $(ANALYZE_TEST_BIN)

$(HTTP_BIN): $(LIB_OBJECTS) $(HTTP_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(HTTP_OBJECTS) -o $(HTTP_BIN)

$(HTTP_TEST_BIN): $(LIB_OBJECTS) $(HTTP_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(HTTP_TEST_OBJECTS) -o $(HTTP_TEST_BIN)

$(HTTP_UNIT_TEST_BIN): $(LIB_OBJECTS) $(HTTP_SOURCES)
	$(CC) $(CFLAGS) -Wno-unused-function $(INCLUDES) -DJQ_HTTP_TEST $(LIB_OBJECTS) $(HTTP_SOURCES) -o $(HTTP_UNIT_TEST_BIN)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(TEST_BIN) $(PDF_TEST_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(ANALYZE_BIN) $(ANALYZE_TEST_BIN) $(HTTP_BIN) $(HTTP_TEST_BIN) $(HTTP_UNIT_TEST_BIN)
	./$(TEST_BIN)
	./$(PDF_TEST_BIN)
	./$(CLI_TEST_BIN)
	./$(ANALYZE_TEST_BIN)
	./$(HTTP_TEST_BIN)
	./$(HTTP_UNIT_TEST_BIN)
	sh tests/integration_http.sh
	sh tests/test_docs.sh
	sh tests/test_html_report.sh
	sh tests/test_demo_scripts.sh

clean:
	rm -f $(LIB_OBJECTS) $(CLI_OBJECTS) $(ANALYZE_OBJECTS) $(HTTP_OBJECTS) $(TEST_OBJECTS) $(CLI_TEST_OBJECTS) \
		$(ANALYZE_TEST_OBJECTS) $(HTTP_TEST_OBJECTS) $(PDF_TEST_OBJECTS) $(TEST_BIN) $(PDF_TEST_BIN) $(CLI_TEST_BIN) $(ANALYZE_TEST_BIN) $(HTTP_TEST_BIN) $(CLI_BIN) $(ANALYZE_BIN) $(HTTP_BIN) \
		$(HTTP_UNIT_TEST_BIN)
