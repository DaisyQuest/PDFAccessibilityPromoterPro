#!/bin/sh
set -eu

DOC_PATH="problems_we_correct.md"

test -f "$DOC_PATH"

grep -q "# Problems We Correct" "$DOC_PATH"

grep -q "Document Metadata & Navigation" "$DOC_PATH"

grep -q "Reading Order & Structure" "$DOC_PATH"

grep -q "Alternate Text & Non-Text Content" "$DOC_PATH"

grep -q "Form Fields & Interactive Elements" "$DOC_PATH"

grep -q "Output Consistency & Validation" "$DOC_PATH"

grep -q "Missing document title" "$DOC_PATH"

grep -q "Images without alt text" "$DOC_PATH"

grep -q "Links without accessible names" "$DOC_PATH"

echo "Documentation checks passed."
