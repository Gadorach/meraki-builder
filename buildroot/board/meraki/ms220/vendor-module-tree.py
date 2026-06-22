#!/usr/bin/env python3
"""Create and verify the platform-complete postmerkOS vendor module tree.

The firmware is intentionally platform agnostic: every supported VCore-III
family is embedded in one SquashFS image and S08kmods selects one family after
identifying the board at boot. This tool keeps every build stage on one
canonical contract and also proves that *all* donor .ko files survived staging.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tempfile
from pathlib import Path

REQUIRED_IN_TREE = "postmerkos-required-modules.txt"
ALL_IN_TREE = "postmerkos-all-modules.txt"
HASH_MANIFEST = "postmerkos-modules.sha256"
GENERATED = {REQUIRED_IN_TREE, ALL_IN_TREE, HASH_MANIFEST}


class ContractError(RuntimeError):
    pass


def read_contract(path: Path) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        value = raw.split("#", 1)[0].strip()
        if not value:
            continue
        candidate = Path(value)
        if candidate.is_absolute() or ".." in candidate.parts or value.startswith("./"):
            raise ContractError(f"unsafe required-module path at {path}:{number}: {value}")
        if any(ch.isspace() for ch in value):
            raise ContractError(f"whitespace is not permitted in module paths: {value}")
        if not value.endswith(".ko"):
            raise ContractError(f"required entry is not a kernel object: {value}")
        if value in seen:
            raise ContractError(f"duplicate required-module entry: {value}")
        seen.add(value)
        result.append(value)
    if not result:
        raise ContractError(f"required-module contract is empty: {path}")
    return result


def atomic_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def scan_tree(tree: Path) -> list[str]:
    """Return regular .ko paths without following links.

    os.scandir is used instead of recursive pathlib globbing because some
    container/overlay filesystems have exhibited pathological rglob behaviour
    while validating freshly staged trees.
    """
    modules: list[str] = []
    pending = [tree]
    while pending:
        directory = pending.pop()
        try:
            entries = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as exc:
            raise ContractError(f"cannot scan module tree directory {directory}: {exc}") from exc
        for entry in entries:
            path = Path(entry.path)
            relative = path.relative_to(tree).as_posix()
            if entry.is_symlink():
                raise ContractError(f"module tree contains a symlink: {relative}")
            if entry.is_dir(follow_symlinks=False):
                pending.append(path)
                continue
            if not entry.is_file(follow_symlinks=False):
                raise ContractError(f"module tree contains a special file: {relative}")
            if entry.name.endswith(".ko"):
                if entry.stat(follow_symlinks=False).st_size <= 0:
                    raise ContractError(f"kernel object is empty: {relative}")
                modules.append(relative)
    modules.sort()
    if not modules:
        raise ContractError("module tree contains no .ko files")
    return modules


def validate_entry_safety(tree: Path) -> None:
    scan_tree(tree)


def actual_modules(tree: Path) -> list[str]:
    return scan_tree(tree)


def validate_required(tree: Path, required: list[str]) -> None:
    for relative in required:
        path = tree / relative
        if path.is_symlink() or not path.is_file() or path.stat().st_size <= 0:
            raise ContractError(f"required platform module is missing/invalid: {relative}")


def parse_list(path: Path, label: str, *, require_sorted: bool = True) -> list[str]:
    if not path.is_file() or path.stat().st_size <= 0:
        raise ContractError(f"{label} is missing or empty: {path.name}")
    values = [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(values) != len(set(values)):
        raise ContractError(f"{label} contains duplicate entries: {path.name}")
    if require_sorted and values != sorted(values):
        raise ContractError(f"{label} is not sorted: {path.name}")
    return values


def parse_hash_manifest(path: Path) -> dict[str, str]:
    if not path.is_file() or path.stat().st_size <= 0:
        raise ContractError(f"SHA-256 manifest is missing or empty: {path.name}")
    entries: dict[str, str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if len(line) < 67 or line[64:66] not in {"  ", " *"}:
            raise ContractError(f"invalid SHA-256 manifest line {number}")
        digest = line[:64].lower()
        relative = line[66:]
        if len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
            raise ContractError(f"invalid SHA-256 digest on line {number}")
        if not relative or relative in entries:
            raise ContractError(f"invalid/duplicate module path on manifest line {number}")
        entries[relative] = digest
    return entries


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def create(tree: Path, required_file: Path) -> None:
    required = read_contract(required_file)
    validate_entry_safety(tree)
    modules = actual_modules(tree)
    validate_required(tree, required)
    atomic_text(tree / REQUIRED_IN_TREE, "\n".join(required) + "\n")
    atomic_text(tree / ALL_IN_TREE, "\n".join(modules) + "\n")
    manifest = "".join(f"{sha256(tree / relative)}  {relative}\n" for relative in modules)
    atomic_text(tree / HASH_MANIFEST, manifest)
    verify(tree, required_file)


def verify(tree: Path, required_file: Path) -> None:
    if not tree.is_dir():
        raise ContractError(f"module tree is missing: {tree}")
    required = read_contract(required_file)
    validate_entry_safety(tree)
    modules = actual_modules(tree)
    validate_required(tree, required)

    embedded_required = parse_list(
        tree / REQUIRED_IN_TREE, "embedded required-module contract", require_sorted=False
    )
    if embedded_required != required:
        raise ContractError("embedded required-module contract differs from the build contract")

    embedded_all = parse_list(tree / ALL_IN_TREE, "embedded complete module inventory")
    if embedded_all != modules:
        missing = sorted(set(embedded_all) - set(modules))
        unlisted = sorted(set(modules) - set(embedded_all))
        raise ContractError(
            "complete module inventory differs from actual tree"
            f"; missing={missing[:3]} unlisted={unlisted[:3]}"
        )

    manifest = parse_hash_manifest(tree / HASH_MANIFEST)
    if sorted(manifest) != modules:
        raise ContractError("SHA-256 manifest does not enumerate every and only .ko file")
    for relative in modules:
        observed = sha256(tree / relative)
        if observed != manifest[relative]:
            raise ContractError(f"SHA-256 mismatch: {relative}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("tree", type=Path)
    parser.add_argument(
        "--required-file",
        type=Path,
        default=Path(__file__).with_name("vendor-modules.required"),
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    try:
        tree = args.tree.resolve(strict=True)
        required_file = args.required_file.resolve(strict=True)
        if args.action == "create":
            create(tree, required_file)
        else:
            verify(tree, required_file)
        if not args.quiet:
            count = len(actual_modules(tree))
            families = "luton26, jaguar, jaguar_dual"
            print(f"verified platform-complete module tree: {count} objects ({families})")
        return 0
    except (ContractError, OSError) as exc:
        print(f"vendor-module-tree: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
