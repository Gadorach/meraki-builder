#!/usr/bin/env python3
"""Copy a donor /lib/modules tree while replacing every symlink with file data.

Vendor firmware images occasionally represent family-specific objects as links.
Preserving those links into a standalone SquashFS can leave broken module paths,
so the build uses a closed, regular-file-only copy instead.
"""
from __future__ import annotations

import argparse
import os
import shutil
import stat
from pathlib import Path


def inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def resolve_link(path: Path, donor_root: Path) -> Path:
    target = os.readlink(path)
    candidate = donor_root / target.lstrip("/") if target.startswith("/") else path.parent / target
    resolved = candidate.resolve(strict=True)
    if not inside(resolved, donor_root):
        raise RuntimeError(f"module link escapes donor root: {path} -> {target}")
    return resolved


def copy_entry(source: Path, destination: Path, donor_root: Path, stack: tuple[Path, ...]) -> None:
    actual = resolve_link(source, donor_root) if source.is_symlink() else source
    actual_resolved = actual.resolve(strict=True)
    if actual_resolved in stack:
        raise RuntimeError(f"module link cycle involving {source}")
    mode = actual.stat().st_mode
    if stat.S_ISDIR(mode):
        destination.mkdir(parents=True, exist_ok=True)
        for child in sorted(actual.iterdir(), key=lambda item: item.name):
            copy_entry(child, destination / child.name, donor_root, stack + (actual_resolved,))
        return
    if not stat.S_ISREG(mode):
        raise RuntimeError(f"unsupported entry in donor module tree: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(actual, destination)
    os.chmod(destination, 0o644)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--donor-root", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    donor_root = args.donor_root.resolve(strict=True)
    source = args.source.resolve(strict=True)
    if not inside(source, donor_root):
        parser.error("source must be inside donor root")
    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    for entry in sorted(source.iterdir(), key=lambda item: item.name):
        copy_entry(entry, args.output / entry.name, donor_root, ())

    links = [path for path in args.output.rglob("*") if path.is_symlink()]
    if links:
        raise RuntimeError(f"materialized tree still contains symlinks: {links[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
