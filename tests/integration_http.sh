#!/bin/sh
set -eu

ROOT_DIR="$(mktemp -d /tmp/pap_http_integration_XXXXXX)"
PORT=9120

cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$ROOT_DIR"
}

trap cleanup EXIT

./job_queue_cli init "$ROOT_DIR"
echo "pdf data" > "$ROOT_DIR/source.pdf"
echo "metadata" > "$ROOT_DIR/source.metadata"

./job_queue_http "$ROOT_DIR" "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!

for _ in 1 2 3 4 5; do
    if curl -s "http://127.0.0.1:${PORT}/health" | grep -q "ok"; then
        break
    fi
    sleep 1
done

submit_status="$(curl -s -o /dev/null -w "%{http_code}" \
    "http://127.0.0.1:${PORT}/submit?uuid=integration-job&pdf=source.pdf&metadata=source.metadata")"
test "$submit_status" = "200"

claim_output="$(curl -s "http://127.0.0.1:${PORT}/claim")"
echo "$claim_output" | grep -q "integration-job"

finalize_status="$(curl -s -o /dev/null -w "%{http_code}" \
    "http://127.0.0.1:${PORT}/finalize?uuid=integration-job&from=jobs&to=complete")"
test "$finalize_status" = "200"

status_output="$(curl -s "http://127.0.0.1:${PORT}/status?uuid=integration-job")"
echo "$status_output" | grep -q "state=complete"

retrieve_output="$(curl -s "http://127.0.0.1:${PORT}/retrieve?uuid=integration-job&state=complete&kind=metadata")"
echo "$retrieve_output" | grep -q "metadata"

echo "HTTP integration script completed."
