#!/usr/bin/env python3
"""Enable and verify the kernel features required by PMOSLIVE.

PMOSLIVE hands the normal XZ-compressed SquashFS to Linux as a legacy external
initrd. These options are safe for normal flash boots and therefore become part
of the retail kernel rather than a separate development-only kernel variant.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

REQUIRED = {
    # msxx_defconfig disables the parent block-device menu. Kconfig removes
    # BLK_DEV_RAM and its numeric children unless both parents are enabled
    # before olddefconfig resolves dependencies.
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
}


def current_values(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
        else:
            match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line)
            if match:
                values[match.group(1)] = "n"
    return values


def apply(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    keys = set(REQUIRED)
    kept: list[str] = []
    for line in text.splitlines():
        key = None
        if line.startswith("CONFIG_") and "=" in line:
            key = line.split("=", 1)[0]
        else:
            match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line)
            if match:
                key = match.group(1)
        if key not in keys:
            kept.append(line)
    kept.extend(["", "# PMOSLIVE legacy external SquashFS initrd support"])
    kept.extend(f"{key}={value}" for key, value in REQUIRED.items())
    path.write_text("\n".join(kept).rstrip() + "\n", encoding="utf-8")


def verify(path: Path) -> None:
    values = current_values(path.read_text(encoding="utf-8"))
    errors = [f"{key}={values.get(key, '<missing>')} (expected {value})"
              for key, value in REQUIRED.items() if values.get(key) != value]
    if errors:
        raise SystemExit("PMOSLIVE kernel configuration is incomplete:\n  " + "\n  ".join(errors))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if not args.config.is_file():
        raise SystemExit(f"kernel config is missing: {args.config}")
    if args.verify:
        verify(args.config)
    else:
        apply(args.config)
        verify(args.config)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
