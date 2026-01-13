# PDFAccessibilityPromoterPro

## Run the job queue server

```sh
make job_queue_http
./job_queue_http <root> <port> [--bind <addr>] [--token <token>]
```

Example:

```sh
ROOT_DIR="$(mktemp -d)"
./job_queue_cli init "$ROOT_DIR"
./job_queue_http "$ROOT_DIR" 8080 --bind 127.0.0.1
```

## Monitor the server

Use the JSON metrics endpoint to inspect queue depth, locked jobs, orphaned files, and byte counts.

```sh
curl -s http://127.0.0.1:8080/metrics
```

If a token is configured, pass it via the `Authorization` header or `token` query parameter.

```sh
curl -s -H "Authorization: Bearer $JOB_QUEUE_TOKEN" http://127.0.0.1:8080/metrics
```

You can also collect stats directly from disk:

```sh
./job_queue_cli stats <root>
```
