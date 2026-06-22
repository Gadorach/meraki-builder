#!/usr/bin/env python3
import hashlib, pathlib, struct, sys

def fail(message: str) -> None:
    raise SystemExit(f"MX80 image validation failed: {message}")

if len(sys.argv) != 2:
    fail("usage: validate.py IMAGE")
path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
if len(data) < 0x400001:
    fail("image is too small")
if data[0:4] != bytes.fromhex("8e73ed8a"):
    fail("Meraki image magic is missing")
header_size = struct.unpack(">I", data[4:8])[0]
if header_size != 0x400:
    fail(f"unexpected header length 0x{header_size:x}")
payload_size = struct.unpack(">I", data[8:12])[0]
if payload_size != len(data) - header_size:
    fail(f"header payload size {payload_size} does not match {len(data) - header_size}")
expected = data[12:32]
actual = hashlib.sha1(data[header_size:]).digest()
if expected != actual:
    fail("payload SHA-1 does not match the image header")
if data[0x4000:0x4004] != bytes.fromhex("d00dfeed"):
    fail("device tree magic is missing at 0x4000")
if data[0x20000:0x20004] != bytes.fromhex("27051956"):
    fail("uImage magic is missing at 0x20000")
if data[0x400000:0x400004] != bytes.fromhex("27051956"):
    fail("initramfs uImage magic is missing at 0x400000")
if len(data) > 0x1900000:
    fail(f"image exceeds the MX80 container limit: {len(data)} bytes")
print(f"MX80 image valid: {path} ({len(data)} bytes, sha256={hashlib.sha256(data).hexdigest()})")
