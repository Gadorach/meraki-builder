#!/usr/bin/env python3
"""Validate a meraki-redboot SPIM payload embedded in a complete firmware image."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

HEADER = struct.Struct("<8I")
MAGIC = 0x4D495053
DEFAULT_OFFSET = 0x40000
DEFAULT_REGION_BYTES = 0x2C0000
DEFAULT_ADDRESS = 0x81000000


def integer(value: str) -> int:
    return int(value, 0)


def validate(data: bytes, *, offset: int, region_bytes: int, expected_payload: bytes | None = None) -> dict:
    if offset < 0 or region_bytes < HEADER.size or offset + region_bytes > len(data):
        raise ValueError("payload region is outside the supplied image")
    header = data[offset : offset + HEADER.size]
    magic, load, size, entry, stored_crc, r0, r1, r2 = HEADER.unpack(header)
    if magic != MAGIC:
        raise ValueError(f"magic 0x{magic:08x} is not SPIM/MIPS 0x{MAGIC:08x}")
    if load != DEFAULT_ADDRESS or entry != DEFAULT_ADDRESS:
        raise ValueError(f"load/entry must both be 0x{DEFAULT_ADDRESS:08x}")
    max_payload = region_bytes - HEADER.size
    if not 0 < size <= max_payload:
        raise ValueError(f"declared payload size 0x{size:x} is outside 1..0x{max_payload:x}")
    if size % 32:
        raise ValueError(f"declared payload size 0x{size:x} is not 32-byte aligned")
    if (r0, r1, r2) != (0, 0, 0):
        raise ValueError("reserved SPIM header words must be zero")
    payload = data[offset + HEADER.size : offset + HEADER.size + size]
    zeroed = bytearray(header)
    struct.pack_into("<I", zeroed, 16, 0)
    calculated_crc = zlib.crc32(zeroed + payload) & 0xFFFFFFFF
    if stored_crc != calculated_crc:
        raise ValueError(
            f"stored CRC 0x{stored_crc:08x} does not match calculated CRC 0x{calculated_crc:08x}"
        )
    original_size = None
    padding = None
    if expected_payload is not None:
        original_size = len(expected_payload)
        padding = size - original_size
        if padding < 0 or padding > 31:
            raise ValueError(
                f"packed payload size {size} is not a valid 32-byte padded form of input size {original_size}"
            )
        if payload[:original_size] != expected_payload:
            raise ValueError("embedded kernel bytes do not match the expected compressed kernel")
        if payload[original_size:] != bytes(padding):
            raise ValueError("kernel alignment padding is not zero-filled")
    return {
        "format": "postmerkos.vcoreiii-payload-validation.v1",
        "offset": offset,
        "region_bytes": region_bytes,
        "magic": f"0x{magic:08x}",
        "load_address": f"0x{load:08x}",
        "entry_point": f"0x{entry:08x}",
        "payload_size": size,
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "stored_crc32": f"0x{stored_crc:08x}",
        "calculated_crc32": f"0x{calculated_crc:08x}",
        "alignment": 32,
        "original_size": original_size,
        "padding": padding,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--offset", type=integer, default=DEFAULT_OFFSET)
    parser.add_argument("--region-bytes", type=integer, default=DEFAULT_REGION_BYTES)
    parser.add_argument("--expected-payload", type=Path)
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    expected = args.expected_payload.read_bytes() if args.expected_payload else None
    try:
        result = validate(
            args.image.read_bytes(), offset=args.offset, region_bytes=args.region_bytes,
            expected_payload=expected,
        )
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"SPIM load/entry: {result['load_address']}")
    print(f"Kernel payload:   {result['payload_size']} bytes")
    print(f"Kernel padding:   {result['padding'] if result['padding'] is not None else 'not compared'}")
    print(f"Stored CRC-32:    {result['stored_crc32']}")
    print(f"Computed CRC-32:  {result['calculated_crc32']}")
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
