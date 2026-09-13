#!/usr/bin/env python3
"""Fix unicode characters and update relative links to point to the official Plan-V4D repo."""

import re
import os

REPO_URL = "https://github.com/kallaballa/Plan-V4D"
BRANCH = "rollback"
INPUT_FILE = os.path.join(os.path.dirname(__file__), "user-generated.html")

UNICODE_FIXES = {
    "\u2014": "--",        # em dash
    "\u2013": "-",         # en dash
    "\u201c": '"',         # left double quotation mark
    "\u201d": '"',         # right double quotation mark
    "\u2018": "'",         # left single quotation mark
    "\u2019": "'",         # right single quotation mark
    "\u2026": "...",       # horizontal ellipsis
    "\u00a0": " ",         # non-breaking space
    "\u00d7": "x",         # multiplication sign
    "\u2265": ">=",        # greater-than or equal to
    "\u2260": "!=",        # not equal to
    "\u2248": "~=",        # almost equal to
    "\u2190": "<-",        # leftwards arrow
    "\u2192": "->",        # rightwards arrow
    "\u00a7": "&sect;",    # section sign (keep as HTML entity)
    "\u26a0": "[!]",       # warning sign
    "\U0001f4cc": "",      # pushpin
    "\ufe0f": "",          # variation selector
    "\u2500": "-",         # box drawings light horizontal
    "\u2502": "|",         # box drawings light vertical
    "\u250c": "+",         # box drawings light down and right
    "\u2510": "+",         # box drawings light down and left
    "\u2514": "+",         # box drawings light up and right
    "\u2518": "+",         # box drawings light up and left
    "\u251c": "+",         # box drawings light vertical and right
    "\u2524": "+",         # box drawings light vertical and left
}

def fix_unicode(text: str) -> str:
    for old, new in UNICODE_FIXES.items():
        text = text.replace(old, new)
    return text

def update_relative_links(text: str) -> str:
    base = f"{REPO_URL}/blob/{BRANCH}"

    patterns = [
        (r'href="modules/([^"]+)"', f'href="{base}/modules/\\1"'),
        (r'href="obs/([^"]*)"', f'href="{base}/obs/\\1"'),
        (r'href="LICENSE"', f'href="{base}/LICENSE"'),
    ]

    for pattern, replacement in patterns:
        text = re.sub(pattern, replacement, text)

    return text

def main():
    with open(INPUT_FILE, "r", encoding="utf-8") as f:
        content = f.read()

    content = fix_unicode(content)
    content = update_relative_links(content)

    with open(INPUT_FILE, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"Updated {INPUT_FILE}")

if __name__ == "__main__":
    main()
