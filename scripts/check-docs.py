#!/usr/bin/env python3
"""Validate local Markdown links and active README hygiene."""
from __future__ import annotations
import argparse
import pathlib
import re
import sys
from urllib.parse import unquote

LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
SKIP_PREFIXES = ("http://", "https://", "mailto:", "ftp://", "data:")
HISTORY_PARTS = {"history", "research"}
HISTORICAL_CHANGE_NARRATIVE = re.compile(
    r"\b(?:previous implementation|earlier implementation|old implementation|"
    r"previous release|earlier release|old release|formerly|historically|"
    r"was fixed|now fixed|regression fix|bug fix|hotfix|workaround|"
    r"superseded implementation|migration-only instruction|transitional build note)\b",
    re.IGNORECASE,
)

def check(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    ignored = {"node_modules", "build", ".work", "artifacts", ".git"}
    for doc in sorted(root.rglob("*.md")):
        if set(doc.relative_to(root).parts) & ignored:
            continue
        text = doc.read_text(encoding="utf-8", errors="replace")
        for raw in LINK.findall(text):
            target = raw.strip().split(maxsplit=1)[0].strip("<>")
            if not target or target.startswith("#") or target.startswith(SKIP_PREFIXES):
                continue
            target = unquote(target.split("#", 1)[0].split("?", 1)[0])
            if not target:
                continue
            resolved = (doc.parent / target).resolve()
            try:
                resolved.relative_to(root.resolve())
            except ValueError:
                # Cross-repository or intentionally external filesystem reference.
                continue
            if not resolved.exists():
                errors.append(f"{doc.relative_to(root)}: broken link: {raw}")
        rel_parts = set(doc.relative_to(root).parts)
        if not (rel_parts & HISTORY_PARTS):
            for match in HISTORICAL_CHANGE_NARRATIVE.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                errors.append(
                    f"{doc.relative_to(root)}:{line}: historical change narrative in active documentation: "
                    f"{match.group(0)}"
                )
    return errors

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    errors: list[str] = []
    for root in args.roots:
        errors.extend(check(root))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Documentation links and active current-state wording are valid.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
