#!/bin/sh
set -eu

if [ ! -x ./job_queue_http ] || [ ! -x ./job_queue_cli ]; then
    echo "Building job queue binaries..."
    make job_queue_http job_queue_cli
fi

ROOT_DIR="${1:-}"
PORT="${2:-8080}"
BIND_ADDR="${JOB_QUEUE_BIND_ADDR:-127.0.0.1}"
TOKEN="${JOB_QUEUE_TOKEN:-}"
TEMP_ROOT=0

if [ -z "$ROOT_DIR" ]; then
    ROOT_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t pap_panel)"
    TEMP_ROOT=1
else
    mkdir -p "$ROOT_DIR"
fi

cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ "$TEMP_ROOT" -eq 1 ] && [ "${KEEP_ROOT:-}" != "1" ]; then
        rm -rf "$ROOT_DIR"
    fi
}
trap cleanup EXIT INT TERM

./job_queue_cli init "$ROOT_DIR" >/dev/null

LOG_PATH="$ROOT_DIR/server.log"
if [ -n "$TOKEN" ]; then
    ./job_queue_http "$ROOT_DIR" "$PORT" --bind "$BIND_ADDR" --token "$TOKEN" >"$LOG_PATH" 2>&1 &
else
    ./job_queue_http "$ROOT_DIR" "$PORT" --bind "$BIND_ADDR" >"$LOG_PATH" 2>&1 &
fi
SERVER_PID=$!

sleep 0.2

HOST_ADDR="$BIND_ADDR"
if [ "$HOST_ADDR" = "0.0.0.0" ]; then
    HOST_ADDR="127.0.0.1"
fi

PANEL_URL="http://$HOST_ADDR:$PORT/panel"
if [ -n "$TOKEN" ]; then
    PANEL_URL="$PANEL_URL?token=$TOKEN"
fi

open_browser() {
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$PANEL_URL" >/dev/null 2>&1 || true
    elif command -v open >/dev/null 2>&1; then
        open "$PANEL_URL" >/dev/null 2>&1 || true
    elif command -v gio >/dev/null 2>&1; then
        gio open "$PANEL_URL" >/dev/null 2>&1 || true
    else
        return 1
    fi
}

printf 'Job queue root: %s\n' "$ROOT_DIR"
printf 'Server log: %s\n' "$LOG_PATH"
printf 'Monitoring panel: %s\n' "$PANEL_URL"

open_browser || printf 'Open the panel URL in your browser to continue.\n'

wait "$SERVER_PID"
