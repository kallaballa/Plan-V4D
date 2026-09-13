#!/usr/bin/env python3
"""
Generate web/plan-v4d.md from the three Plan-V4D markdown documentation files.
Outputs a single combined markdown file with a table of contents and
cross-document anchors for easy navigation.
"""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DOCS = {
    "Readme": REPO_ROOT / "README.md",
    "V4D Application Programming Guide": REPO_ROOT / "modules/v4d/doc/v4d-application-programming-guide.markdown",
#    "Plan-DSL Programming Guide": REPO_ROOT / "modules/plan/doc/plan-dsl-programming-guide.markdown",
#    "Plan-DSL Reference (ISA)": REPO_ROOT / "modules/plan/doc/plan-dsl-reference.markdown",
}

OUTPUT = REPO_ROOT / "web" / "plan-v4d.md"


def slugify(text):
    return re.sub(r"[^\w]+", "-", text.lower()).strip("-")


def parse_sections(md_text, doc_title=""):
    sections = []
    current = None
    lines = md_text.split("\n")
    doc_slug = slugify(doc_title)

    for line in lines:
        if line.startswith("## "):
            if current and not re.match(r"^table\s+of\s+contents$", current["title"], re.IGNORECASE):
                sections.append(current)
            title = line[3:].strip()
            if re.match(r"^table\s+of\s+contents$", title, re.IGNORECASE):
                current = None
                continue
            base_id = slugify(title)
            full_id = f"{doc_slug}-{base_id}" if doc_slug else base_id
            current = {
                "title": title,
                "id": full_id,
                "content": "",
                "children": [],
                "doc": doc_title,
            }
        elif line.startswith("### ") and current:
            child_title = line[4:].strip()
            child_id = slugify(child_title)
            full_child_id = f"{current['id']}-{child_id}"
            current["children"].append({
                "title": child_title,
                "id": full_child_id,
                "content": "",
            })
        elif current is not None:
            if current["children"]:
                current["children"][-1]["content"] += line + "\n"
            else:
                current["content"] += line + "\n"

    if current and not re.match(r"^table\s+of\s+contents$", current["title"], re.IGNORECASE):
        sections.append(current)

    return sections


def build_toc(all_sections):
    lines = ["# Table of Contents\n"]
    for sec in all_sections:
        lines.append(f"- [{sec['title']}](#{sec['id']})")
        for child in sec.get("children", []):
            lines.append(f"  - [{child['title']}](#{child['id']})")
    return "\n".join(lines) + "\n"


def main():
    print("Reading source markdown files...")
    all_sections = []

    for doc_title, doc_path in DOCS.items():
        if not doc_path.exists():
            print(f"WARNING: {doc_path} not found, skipping.")
            continue

        with open(doc_path, "r", encoding="utf-8") as f:
            md_text = f.read()

        print(f"Processing: {doc_title}")
        sections = parse_sections(md_text, doc_title)

        for sec in sections:
            sec_id = sec["id"]
            combined_parts = []
            if sec.get("content"):
                combined_parts.append(sec["content"].rstrip("\n"))

            for child in sec.get("children", []):
                child_content = child.get("content", "").rstrip("\n")
                combined_parts.append(f"### {child['title']}\n{child_content}")

            sec["combined"] = "\n\n".join(combined_parts)
            all_sections.append(sec)

    print(f"Total sections: {len(all_sections)}")

    parts = []
    parts.append("# Plan-V4D Documentation\n")
    parts.append(build_toc(all_sections))

    seen_docs = set()
    current_doc = None
    for sec in all_sections:
        doc = sec.get("doc", "")
        if doc != current_doc:
            if current_doc is not None:
                parts.append("\n---\n")
            current_doc = doc
            parts.append(f"\n## {doc}\n")

        parts.append(f"\n### {sec['title']}\n")
        parts.append(sec["combined"])
        parts.append("\n")

    markdown = "\n".join(parts)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write(markdown)

    print(f"Generated: {OUTPUT}")
    print(f"Size: {len(markdown):,} bytes")


if __name__ == "__main__":
    main()
