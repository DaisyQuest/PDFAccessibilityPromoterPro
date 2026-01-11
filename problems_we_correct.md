# Problems We Correct

This document summarizes the PDF accessibility transformations performed by PDF Accessibility Promoter Pro. Each item describes the user-facing problem and the corrective action applied by the system.

## Document Metadata & Navigation

- **Missing document title** → Adds a stable, human-readable Title entry in the document catalog so assistive technologies announce the document properly.
- **Missing document language** → Injects the primary language (Lang) in the catalog and in page-level structures when absent.
- **Untagged document** → Builds a logical structure tree (StructTreeRoot) so content is semantically exposed to screen readers.
- **Broken or absent outline** → Constructs or repairs bookmarks that reflect the logical reading order for efficient navigation.

## Reading Order & Structure

- **Incorrect reading order** → Reorders structure elements and their marked-content references to match visual and logical flow.
- **Missing headings hierarchy** → Promotes text blocks into a consistent H1/H2/H3 hierarchy so sections are discoverable.
- **Improper list semantics** → Converts visually presented lists into List/LI/Lbl/LBody structure elements.
- **Paragraphs without semantic grouping** → Wraps runs of text into P elements to prevent screen readers from merging unrelated content.
- **Tables without semantics** → Maps cells to Table/TR/TH/TD structure elements and links headers with scope/ID references.

## Alternate Text & Non-Text Content

- **Images without alt text** → Adds AlternateText for figures or marks decorative figures as Artifact.
- **Charts without descriptions** → Injects long descriptions in associated metadata and links them from the figure element.
- **Background artifacts exposed as content** → Marks non-informational elements (headers, footers, lines) as Artifact.

## Form Fields & Interactive Elements

- **Unlabeled form fields** → Adds explicit labels and associates them with form fields for correct announcement.
- **Incorrect field order** → Aligns form field order with the structure tree and reading order.
- **Links without accessible names** → Adds Link elements with descriptive text or alt text.

## Output Consistency & Validation

- **Inconsistent tagging across pages** → Normalizes tag usage and role mappings so similar content uses consistent semantics.
- **Missing role mappings** → Populates the RoleMap to ensure custom tags map to standard PDF structure types.
- **Invalid structure references** → Repairs or removes broken MCID references and orphaned structure elements.

If you want us to expand or tailor this list for a specific workflow, we can append additional transformations alongside corresponding validation checks.
