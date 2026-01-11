#!/bin/sh
set -eu

if [ ! -x ./job_queue_http ] || [ ! -x ./job_queue_cli ]; then
    echo "Build the server binaries first (make)."
    exit 1
fi

ROOT_DIR="$(mktemp -d)"
PORT="8090"
UUID="123e4567-e89b-12d3-a456-426614174000"

cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$ROOT_DIR"
}
trap cleanup EXIT

./job_queue_cli init "$ROOT_DIR" >/dev/null
mkdir -p "$ROOT_DIR/inbox"
printf '%s\n' '%PDF-1.4 demo file' > "$ROOT_DIR/inbox/demo.pdf"
printf '%s\n' '{"title":"Demo PDF","source":"demo.sh"}' > "$ROOT_DIR/inbox/demo.metadata.json"

./job_queue_http "$ROOT_DIR" "$PORT" >"$ROOT_DIR/server.log" 2>&1 &
SERVER_PID=$!

sleep 0.2
BASE_URL="http://127.0.0.1:$PORT"

printf 'Health check: '
curl -sS "$BASE_URL/health"

printf 'Submit job: '
curl -sS "$BASE_URL/submit?uuid=$UUID&pdf=inbox/demo.pdf&metadata=inbox/demo.metadata.json"

printf 'Status (queued): '
curl -sS "$BASE_URL/status?uuid=$UUID"

printf 'Claim job: '
curl -sS "$BASE_URL/claim?prefer_priority=0"

printf 'Finalize job: '
curl -sS "$BASE_URL/finalize?uuid=$UUID&from=jobs&to=complete"

printf 'Status (complete): '
curl -sS "$BASE_URL/status?uuid=$UUID"

printf 'Retrieve metadata: '
curl -sS "$BASE_URL/retrieve?uuid=$UUID&state=complete&kind=metadata"

echo "Demo complete. Logs saved at $ROOT_DIR/server.log"
