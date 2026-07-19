#!/usr/bin/env python3
"""Write or verify the kernel artifact contract required by PMOSLIVE.

The record prevents a pre-PMOSLIVE kernel from being reused and then advertised
as live-boot capable merely because current manifest tooling is newer.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

FORMAT = "postmerkos.kernel-build-contract.v1"
BOOT_ARGUMENT_CONTRACT = "vcoreiii-standard-mips-argc-argv-envp-fallback-v1"
KERNEL_VERSION = "3.18.123"
REQUIRED_CONFIG = {
    "CONFIG_BLOCK": "y",
    "CONFIG_BLK_DEV": "y",
    "CONFIG_BLK_DEV_INITRD": "y",
    "CONFIG_BLK_DEV_RAM": "y",
    "CONFIG_BLK_DEV_RAM_COUNT": "1",
    "CONFIG_BLK_DEV_RAM_SIZE": "16384",
    "CONFIG_RD_XZ": "y",
    "CONFIG_SQUASHFS": "y",
    "CONFIG_SQUASHFS_XZ": "y",
    "CONFIG_XZ_DEC": "y",
    "CONFIG_DECOMPRESS_XZ": "y",
    "CONFIG_CMDLINE_BOOL": "y",
    "CONFIG_CMDLINE_OVERRIDE": "n",
    "CONFIG_CMDLINE_FALLBACK": "y",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact_record(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise SystemExit(f"kernel contract input is missing: {path}")
    return {
        "filename": path.name,
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def parse_config(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise SystemExit(f"kernel configuration is missing: {path}")
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if raw.startswith("CONFIG_") and "=" in raw:
            key, value = raw.split("=", 1)
            values[key] = value
        elif raw.startswith("# CONFIG_") and raw.endswith(" is not set"):
            values[raw[2:-11]] = "n"
    return values


def validate_required_config(values: dict[str, str]) -> None:
    errors = []
    for key, expected in REQUIRED_CONFIG.items():
        actual = values.get(key, "<missing>")
        if actual != expected:
            errors.append(f"{key}={actual} (expected {expected})")
    if errors:
        raise SystemExit("PMOSLIVE kernel configuration is incomplete:\n  " + "\n  ".join(errors))


def common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--source-revision-file", required=True, type=Path)
    parser.add_argument("--patch", required=True, type=Path)
    parser.add_argument("--config-policy", required=True, type=Path)
    parser.add_argument("--vmlinuz", required=True, type=Path)
    parser.add_argument("--vmlinuz-bin", required=True, type=Path)
    parser.add_argument("--headers", required=True, type=Path)


def expected_inputs(args: argparse.Namespace) -> dict[str, Any]:
    if not args.source_revision_file.is_file():
        raise SystemExit(f"kernel source revision record is missing: {args.source_revision_file}")
    source_revision = args.source_revision_file.read_text(encoding="utf-8").strip()
    if not source_revision:
        raise SystemExit("kernel source revision record is empty")
    for path in (args.patch, args.config_policy):
        if not path.is_file():
            raise SystemExit(f"kernel contract policy input is missing: {path}")
    return {
        "source_revision": source_revision,
        "managed_patch": {
            "filename": args.patch.name,
            "sha256": sha256(args.patch),
        },
        "config_policy": {
            "filename": args.config_policy.name,
            "sha256": sha256(args.config_policy),
            "required": dict(sorted(REQUIRED_CONFIG.items())),
        },
        "artifacts": {
            "vmlinuz": artifact_record(args.vmlinuz),
            "vmlinuz_bin": artifact_record(args.vmlinuz_bin),
            "headers": artifact_record(args.headers),
        },
    }


def write_record(args: argparse.Namespace) -> int:
    config = parse_config(args.config)
    validate_required_config(config)
    expected = expected_inputs(args)
    record = {
        "format": FORMAT,
        "kernel_version": KERNEL_VERSION,
        "boot_argument_contract": BOOT_ARGUMENT_CONTRACT,
        **expected,
        "resolved_config": {key: config.get(key, "<missing>") for key in sorted(REQUIRED_CONFIG)},
    }
    args.record.parent.mkdir(parents=True, exist_ok=True)
    args.record.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


def verify_record(args: argparse.Namespace) -> int:
    if not args.record.is_file():
        raise SystemExit(f"kernel build contract record is missing: {args.record}")
    try:
        record = json.loads(args.record.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"kernel build contract record is invalid: {exc}") from exc
    if record.get("format") != FORMAT:
        raise SystemExit("kernel build contract format mismatch")
    if record.get("kernel_version") != KERNEL_VERSION:
        raise SystemExit("kernel build contract version mismatch")
    if record.get("boot_argument_contract") != BOOT_ARGUMENT_CONTRACT:
        raise SystemExit("kernel boot-argument contract mismatch")
    if record.get("resolved_config") != dict(sorted(REQUIRED_CONFIG.items())):
        raise SystemExit("kernel build contract resolved configuration mismatch")
    expected = expected_inputs(args)
    for key in ("source_revision", "managed_patch", "config_policy", "artifacts"):
        if record.get(key) != expected[key]:
            raise SystemExit(f"kernel build contract {key} mismatch")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    writer = subparsers.add_parser("write")
    common_args(writer)
    writer.add_argument("--config", required=True, type=Path)
    verifier = subparsers.add_parser("verify")
    common_args(verifier)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return write_record(args) if args.command == "write" else verify_record(args)


if __name__ == "__main__":
    raise SystemExit(main())
