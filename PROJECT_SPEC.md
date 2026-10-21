# Project Specification: PDF Accessibility Promoter Pro

## Current implementation status (2026-10-21)

The numbered sections below define requirements and targets, not a statement
that each capability has shipped. The current C accessibility path in
`src/pdf_accessibility.c` scans PDF bytes for structure/metadata indicators and
emits findings. `src/job_queue_analyze.c` writes JSON/HTML reports and finalizes
the analysis job; it does not rewrite the PDF's structure tree or form objects.
The token scan does not decode and resolve the complete PDF object graph, so
findings can require confirmation, particularly for compressed content.

Tag construction, qualified font/link/structure repairs and veraPDF checks are
implemented in the separate [OpenAutoTag repository](https://github.com/DaisyQuest/OpenAutoTag).
OpenAutoTag is not automatically invoked by this C analysis worker. Its measured
results do not establish this repository's tagging, 5GB processing, coverage or
production-readiness claims. See [capabilities and limitations](problems_we_correct.md)
and `PROJECT_SPEC_COMPLETION_CHECKLIST.md` for the distinction between completed
tasks and outstanding qualification.

## 1. Purpose
PDF Accessibility Promoter Pro is a high-performance system for processing very large PDF documents (5GB+) to improve accessibility. The system must prioritize performance and accuracy over elegance while minimizing external dependencies.

## 2. Goals
- **Primary goal:** Increase accessibility of PDF documents through reliable, high-throughput processing.
- **Secondary goals:**
  - Support very large files (5GB+).
  - Maintain high reliability and correctness.
  - Favor minimal external dependencies.
  - Support local and remote workers via a flexible HTTP-based API.
  - Achieve high test coverage (90%+ encouraged, strive for full branch coverage).

## 3. Non-Goals
- Full-featured PDF authoring.
- Browser-based GUI; instead, a simple Java Swing client manager is planned.
- Non-PDF document formats.

## 4. Architecture Overview

### 4.1 Components
1. **Job Queue Server**
   - Manages job files, job states, and worker coordination.
   - Provides HTTP API endpoints for job submission, status, retrieval, and worker coordination.
   - Supports a local worker mode and remote worker mode.

2. **Workers**
   - Consume jobs from the queue.
   - Perform PDF analysis and accessibility processing.
   - May run on the same host or on remote machines.
   - Must support streaming I/O for large PDFs.

3. **Client Manager**
   - Java Swing app for submitting jobs, monitoring status, and retrieving results.
   - Uses HTTP API.

### 4.2 Job File Structure
Job queue server root (`approot/`):
- `approot/jobs/`
- `approot/priority_jobs/`
- `approot/complete/`
- `approot/error/`

For each job UUID, we store:
- `uuid.pdf.job` — the PDF document or PDF reference.
- `uuid.metadata.job` — associated metadata.

### 4.3 Job Lifecycle
1. **Submission** → written to `jobs/` or `priority_jobs/`.
2. **Processing** → worker takes ownership with a lock or rename strategy.
3. **Completion** → moved to `complete/` on success.
4. **Failure** → moved to `error/` with error metadata.

## 5. File I/O and Performance Requirements
- Must support **rapid transfer** and **processing** of PDFs ≥ 5GB.
- Favor streaming I/O and low-level C for high throughput and deterministic memory usage.
- Parallelization strategy should be considered to increase throughput (e.g., chunked PDF parsing, multi-threaded pipeline).
- Avoid whole-file memory loads.

## 6. PDF Accessibility Processing Requirements
Minimum processing requirements:
- Parse document structure: catalog, pages, outlines, tagging.
- Identify accessibility issues, including missing tags, missing document language, missing text alternatives.
- Support extraction and annotation of structural elements.
- Produce a machine-readable report of improvements and findings.

## 7. HTTP API Requirements
Flexible, HTTP-based API to support:
- Job submission with file transfer (upload or reference).
- Job status polling.
- Job result retrieval.
- Worker coordination (claiming, heartbeats, releasing).

API must support:
- Streaming uploads/downloads.
- Chunked transfers for large PDFs.
- Authentication and authorization hooks (details TBD).

## 8. Dependency Constraints
- Minimize external dependencies.
- Prefer standard libraries in C or minimal, well-reviewed dependencies.
- Any dependency must be justified for performance, stability, and security.

## 9. Reliability and Safety
- Strong data integrity: no partial files written on success path.
- Use atomic operations for job state transitions (e.g., rename).
- Every job should be traceable via metadata logs.
- Robust error reporting and recoverability.

## 10. Testing Strategy
- **Extremely thorough tests required.**
- Strive for 100% branch coverage; 90%+ minimum acceptable.
- Unit, integration, and system tests.
- Test large file streaming logic with synthetic large files.
- Stress-test job queues and worker concurrency.
- Document all test cases in the spec or test plan.

## 11. Implementation Considerations
- **Language:** C is favored for core file I/O and parsing.
- **Parallelism:** POSIX threads or platform-specific threading.
- **Data formats:** metadata should be JSON (unless otherwise required).
- **Job locking:** prefer atomic rename or file locks.

## 12. Deliverables
- Job queue server.
- Worker process implementation.
- Java Swing client manager.
- Full test suite with coverage reporting.
- Documentation and operator guides.

## 13. Success Criteria
- Large PDFs (≥ 5GB) handled without failure.
- Documented throughput and benchmarks.
- 90%+ code coverage achieved with strong branch coverage.
- Consistent and reliable job lifecycle behavior.

