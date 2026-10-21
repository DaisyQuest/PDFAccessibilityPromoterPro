# Accessibility capabilities and claims

Reviewed 2026-10-21. This file replaces the earlier transformation list, which
incorrectly described proposed repairs as behavior of this C project.

## Implemented in this repository

| Capability | Evidence | Limit |
| --- | --- | --- |
| Accessibility signal analysis | `pdfa_analyze_file` in `src/pdf_accessibility.c`; `include/pap/pdf_accessibility.h`; analyzer tests | Scans byte-level PDF tokens and selected values. It does not decode and resolve the full PDF object graph or establish semantic correctness. |
| Findings and reports | JSON/HTML report functions and `src/job_queue_analyze.c` | A report of missing indicators is not evidence that a repair was applied. A successful analysis exit is not PDF/UA conformance. |
| Analysis job lifecycle | The worker claims a job, analyzes it, saves reports and finalizes queue state | Completion means processing finished; the worker does not write new tags, headings, labels, table headers or PDF metadata. |

The scan includes indicators for catalog/pages, outlines, tags, language,
title, text alternatives, marked content, viewer preferences, RoleMap,
ParentTree, StructParents and MCIDs. Their presence does not prove valid
ownership, meaningful semantics or correct reading order. An absent raw token
can also be misleading when the relevant content is compressed. In the recorded
TEST-FORM analysis, the reported `missing_mcid` was contradicted by 89 MCIDs in
decoded content; the output PDF was byte-identical to the input.

## Implemented in the separate OpenAutoTag engine

[OpenAutoTag](https://github.com/DaisyQuest/OpenAutoTag) provides deterministic
semantic inference, native writing and bounded, verified repairs. Its
[claims register](https://github.com/DaisyQuest/OpenAutoTag/blob/main/docs/tagging-quality-claims.md)
and [technical design](https://github.com/DaisyQuest/OpenAutoTag/blob/main/docs/native-tagging-design.md)
define the supported scope. The C analysis worker does not automatically invoke
that engine.

The recorded OpenAutoTag delivery passes 171 production-baseline tests and
74 checks across 16 synthetic semantic cases. One TEST-FORM candidate passes
veraPDF 1.30.3 Greenfield with zero raw failures and zero independent font-audit
findings; 32 fields and 33 widgets were preserved. These are results for that
engine and candidate, not a general accuracy estimate. Human reader/keyboard
qualification and independent expert review of the synthetic references remain
pending. The candidate remains `review_required`.

## Proposed or unqualified work

The C accessibility analyzer does not currently implement the former list's
metadata injection, bookmark construction, heading/list/table reconstruction,
reading-order changes, alternate-description generation, artifact conversion,
form-label/tab-order repair, RoleMap population or broken-reference repair.
Those capabilities require a real PDF writer or explicit integration with a
qualified engine, with saved-output tests for each supported transformation.

Neither project's automated results alone justify claims of perfect tagging,
general PDF/UA or WCAG conformance, universal screen-reader support, or measured
99% accuracy. Large-file performance and repository-wide coverage targets in
`PROJECT_SPEC.md` remain targets unless backed by their own recorded tests.

Further OpenAutoTag work is proposed in its
[tagging accuracy roadmap](https://github.com/DaisyQuest/OpenAutoTag/blob/main/docs/tagging-accuracy-roadmap.md).
No runtime behavior in the C project changed during this documentation update.
