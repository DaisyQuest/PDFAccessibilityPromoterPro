#!/bin/sh
set -eu

if [ ! -x ./job_queue_cli ] || [ ! -x ./job_queue_ocr ] || [ ! -x ./job_queue_redact ] || [ ! -x ./job_queue_analyze ]; then
    echo "Build the job processor binaries before running this test." >&2
    exit 1
fi

ROOT_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$ROOT_DIR"
}
trap cleanup EXIT

PDF_PATH="$ROOT_DIR/sample.pdf"
REDACT_PDF_PATH="$ROOT_DIR/redact.pdf"

printf '%s\n' '%PDF-1.6' '1 0 obj' '<<>>' 'endobj' > "$PDF_PATH"
printf '%s\n' '%PDF-1.6' '1 0 obj' '<<>>' 'endobj' 'SECRET' > "$REDACT_PDF_PATH"

run_make() {
    target="$1"
    file="$2"
    output=$(make "$target" "$file")
    printf '%s\n' "$output"
}

extract_root() {
    printf '%s\n' "$1" | awk -F': ' '/^Output root: /{print $2}'
}

assert_contains() {
    file="$1"
    needle="$2"
    if ! grep -q "$needle" "$file"; then
        echo "Expected '$needle' in $file" >&2
        exit 1
    fi
}

assert_file() {
    if [ ! -f "$1" ]; then
        echo "Missing expected file: $1" >&2
        exit 1
    fi
}

run_and_check() {
    target="$1"
    file="$2"
    expected_key="$3"

    output=$(run_make "$target" "$file")
    root=$(extract_root "$output")
    if [ -z "$root" ]; then
        echo "Missing output root for $target" >&2
        exit 1
    fi

    metadata=$(printf '%s\n' "$output" | awk -F': ' '/^Metadata output: /{print $2}')
    assert_file "$metadata"
    assert_contains "$metadata" "$expected_key"

    if [ "$target" = "analyze" ]; then
        report=$(printf '%s\n' "$output" | awk -F': ' '/^Report output: /{print $2}')
        assert_file "$report"
    fi

    rm -rf "$root"
}

run_and_check ocr "$PDF_PATH" '"ocr_status":"complete"'
run_and_check redact "$REDACT_PDF_PATH" '"redaction_status":"complete"'
run_and_check analyze "$PDF_PATH" '"pdf_version":"1.6"'

output=$(run_make accessible "$PDF_PATH")
root=$(extract_root "$output")
if [ -z "$root" ]; then
    echo "Missing output root for accessible" >&2
    exit 1
fi
rm -rf "$root"

echo "Make target checks passed."
