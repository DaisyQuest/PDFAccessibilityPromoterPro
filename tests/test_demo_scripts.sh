#!/bin/sh
set -eu

for script in demo.sh demo.bat demo.ps1; do
    test -f "$script"
done

grep -q "job_queue_http" demo.sh
grep -q "/health" demo.sh
grep -q "/submit" demo.sh
grep -q "/claim" demo.sh
grep -q "/finalize" demo.sh
grep -q "/retrieve" demo.sh

grep -q "job_queue_http" demo.bat
grep -q "/health" demo.bat
grep -q "/submit" demo.bat
grep -q "/claim" demo.bat
grep -q "/finalize" demo.bat
grep -q "/retrieve" demo.bat

grep -q "job_queue_http" demo.ps1
grep -q "/health" demo.ps1
grep -q "/submit" demo.ps1
grep -q "/claim" demo.ps1
grep -q "/finalize" demo.ps1
grep -q "/retrieve" demo.ps1

echo "Demo script checks passed."
