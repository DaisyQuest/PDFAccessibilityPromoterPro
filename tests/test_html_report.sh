#!/bin/sh
set -eu

REPORT_PATH="accessibility_report.html"

test -f "$REPORT_PATH"

grep -q "Accessibility Transformation Report" "$REPORT_PATH"

grep -q "Before PDF" "$REPORT_PATH"

grep -q "After PDF" "$REPORT_PATH"

grep -q "tests/fixtures/problem_document.pdf" "$REPORT_PATH"

grep -q "tests/fixtures/fixed_document.pdf" "$REPORT_PATH"

grep -q "PDF Accessibility Promoter Pro" "$REPORT_PATH"

grep -q "Transformations Applied" "$REPORT_PATH"

grep -q "problems_we_correct.md" "$REPORT_PATH"

echo "HTML report checks passed."
