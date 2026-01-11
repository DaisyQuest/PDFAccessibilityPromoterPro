CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_XOPEN_SOURCE=700

INCLUDES = -Iinclude

LIB_SOURCES = src/job_queue.c
CLI_SOURCES = src/job_queue_cli.c
HTTP_SOURCES = src/job_queue_http.c
TEST_SOURCES = tests/test_job_queue.c
CLI_TEST_SOURCES = tests/test_job_queue_cli.c
HTTP_TEST_SOURCES = tests/test_job_queue_http.c

LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
CLI_OBJECTS = $(CLI_SOURCES:.c=.o)
HTTP_OBJECTS = $(HTTP_SOURCES:.c=.o)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)
CLI_TEST_OBJECTS = $(CLI_TEST_SOURCES:.c=.o)
HTTP_TEST_OBJECTS = $(HTTP_TEST_SOURCES:.c=.o)

TEST_BIN = tests/test_job_queue
CLI_TEST_BIN = tests/test_job_queue_cli
CLI_BIN = job_queue_cli
HTTP_TEST_BIN = tests/test_job_queue_http
HTTP_BIN = job_queue_http

.PHONY: all test clean

all: $(TEST_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(HTTP_BIN) $(HTTP_TEST_BIN)

$(TEST_BIN): $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(TEST_OBJECTS) -o $(TEST_BIN)

$(CLI_TEST_BIN): $(LIB_OBJECTS) $(CLI_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(CLI_TEST_OBJECTS) -o $(CLI_TEST_BIN)

$(CLI_BIN): $(LIB_OBJECTS) $(CLI_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(CLI_OBJECTS) -o $(CLI_BIN)

$(HTTP_BIN): $(LIB_OBJECTS) $(HTTP_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(HTTP_OBJECTS) -o $(HTTP_BIN)

$(HTTP_TEST_BIN): $(LIB_OBJECTS) $(HTTP_TEST_OBJECTS)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $(HTTP_TEST_OBJECTS) -o $(HTTP_TEST_BIN)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(TEST_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(HTTP_BIN) $(HTTP_TEST_BIN)
	./$(TEST_BIN)
	./$(CLI_TEST_BIN)
	./$(HTTP_TEST_BIN)

clean:
	rm -f $(LIB_OBJECTS) $(CLI_OBJECTS) $(HTTP_OBJECTS) $(TEST_OBJECTS) $(CLI_TEST_OBJECTS) \
		$(HTTP_TEST_OBJECTS) $(TEST_BIN) $(CLI_TEST_BIN) $(HTTP_TEST_BIN) $(CLI_BIN) $(HTTP_BIN)
