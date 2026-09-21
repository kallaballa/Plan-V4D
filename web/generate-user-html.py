#!/usr/bin/env python3
"""
Generate user-generated.html from Plan-V4D markdown docs, fix unicode,
rewrite relative links to the official GitHub repo, and make the layout
fit the viewport width.
"""

import re
import os
import subprocess
import sys
from pathlib import Path

# ─── Configuration ────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = REPO_ROOT / "web"
OUTPUT_FILE = WEB_DIR / "user-generated.html"

REPO_URL = "https://github.com/kallaballa/Plan-V4D"
BRANCH = "rollback"

DOCS = [
    REPO_ROOT / "README.md",
    REPO_ROOT / "modules/v4d/doc/v4d-application-programming-guide.markdown",
    REPO_ROOT / "modules/plan/doc/plan-dsl-programming-guide.markdown",
    REPO_ROOT / "modules/plan/doc/plan-dsl-reference.markdown",
]

UNICODE_FIXES = {
    "\u2014": "--",
    "\u2013": "-",
    "\u201c": '"',
    "\u201d": '"',
    "\u2018": "'",
    "\u2019": "'",
    "\u2026": "...",
    "\u00a0": " ",
    "\u00d7": "x",
    "\u2265": ">=",
    "\u2260": "!=",
    "\u2248": "~=",
    "\u2190": "<-",
    "\u2192": "->",
    "\u00a7": "&sect;",
    "\u26a0": "[!]",
    "\U0001f4cc": "",
    "\ufe0f": "",
    "\u2500": "-",
    "\u2502": "|",
    "\u250c": "+",
    "\u2510": "+",
    "\u2514": "+",
    "\u2518": "+",
    "\u251c": "+",
    "\u2524": "+",
}

# ─── Helpers ──────────────────────────────────────────────────────────────────

def fix_unicode(text: str) -> str:
    for old, new in UNICODE_FIXES.items():
        text = text.replace(old, new)
    return text


def update_relative_links(text: str) -> str:
    base = f"{REPO_URL}/blob/{BRANCH}"
    patterns = [
        (r'href="modules/([^"]+)"', f'href="{base}/modules/\\1"'),
        (r'href="obs/([^"]*)"',      f'href="{base}/obs/\\1"'),
        (r'href="LICENSE"',          f'href="{base}/LICENSE"'),
    ]
    for pattern, replacement in patterns:
        text = re.sub(pattern, replacement, text)
    return text


def make_responsive(text: str) -> str:
    """Remove the fixed max-width / large side padding so the page fills the
    viewport, while keeping the existing mobile media query."""
    text = re.sub(r'body\s*\{[^}]*max-width:\s*36em;', 'body {', text)
    text = re.sub(
        r'padding-left:\s*50px;\s*padding-right:\s*50px;',
        'padding-left: 5%; padding-right: 5%;',
        text,
    )
    text = re.sub(
        r'padding:\s*12px;',
        'padding: 4vw;',
        text,
    )
    return text


def underline_links(text: str) -> str:
    """Ensure all links are underlined."""
    # Add underline to the main a selector
    text = re.sub(
        r'(a\s*\{\s*color:\s*#1a1a1a;)(\s*\})',
        r'\1\n      text-decoration: underline;\2',
        text,
    )
    # Also ensure visited links are underlined
    text = re.sub(
        r'(a:visited\s*\{\s*color:\s*#1a1a1a;)(\s*\})',
        r'\1\n      text-decoration: underline;\2',
        text,
    )
    return text


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    missing = [str(p) for p in DOCS if not p.exists()]
    if missing:
        print("ERROR: missing source file(s):")
        for p in missing:
            print(f"  {p}")
        sys.exit(1)

    print("Generating HTML with pandoc...")
    WEB_DIR.mkdir(parents=True, exist_ok=True)

    pandoc_cmd = [
        "pandoc",
        "--toc",
        "--standalone",
        "-o",
        str(OUTPUT_FILE),
    ] + [str(p) for p in DOCS]

    result = subprocess.run(pandoc_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("pandoc failed:")
        print(result.stderr)
        sys.exit(1)

    print(f"Reading {OUTPUT_FILE} ...")
    with open(OUTPUT_FILE, "r", encoding="utf-8") as f:
        content = f.read()

    print("Fixing unicode ...")
    content = fix_unicode(content)

    print("Rewriting relative links ...")
    content = update_relative_links(content)

    print("Making layout responsive ...")
    content = make_responsive(content)

    print("Underlining links ...")
    content = underline_links(content)

    print(f"Writing {OUTPUT_FILE} ...")
    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write(content)

    print("Done.")


if __name__ == "__main__":
    main()
