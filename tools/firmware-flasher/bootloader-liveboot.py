#!/usr/bin/env python3
"""Validate or boot a retail MS42/MS42P image entirely from RAM through PMOSLIVE."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import termios

from bootloader_protocol import (
    MODEL_FAMILY,
    ProtocolError,
    SerialLink,
    send_ram_payload,
    validate_bundle,
    validate_live_payload_binding,
)
from pmosrec_v3 import (
    BaudController,
    negotiate_fastest_baud,
    qualify_transport,
    send_liveboot_v3,
)

LIVE_MARKER_RE = re.compile(
    rb"PMOSLIVE3;SOC=jaguar1;FAMILY=2;PROTO=3;FLASH=0;LIVEBOOT=1;"
    rb"IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
    rb"FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END"
)
LIVE_READY_RE = re.compile(r"^PMOSLIVE READY 3 SOC=jaguar1 FAMILY=00000002 FLASH=0$")
LIVE_DESCRIPTOR_RE = re.compile(
    r"^PMOSLIVE DESCRIPTOR PMOSLIVE3;SOC=jaguar1;FAMILY=2;PROTO=3;FLASH=0;LIVEBOOT=1;"
    r"IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
    r"FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END$"
)
MENU_BYTE_RE = re.compile(r"\bBYTE:\s*0x([0-9a-fA-F]{8})\b")
MENU_SELECTION_RE = re.compile(r"\bSELECTED:\s*0x([0-9a-fA-F]{8})\b")


@dataclass(frozen=True)
class LivePayload:
    path: Path
    size: int
    sha256: str
    load_address: int
    entry_address: int


def baud_constant(baud: int) -> int:
    name = f"B{baud}"
    if not hasattr(termios, name):
        raise ProtocolError(f"termios does not support bootstrap baud {baud}")
    return getattr(termios, name)


def configure_serial(fd: int, baud: int) -> list:
    old = termios.tcgetattr(fd)
    attrs = termios.tcgetattr(fd)
    speed = baud_constant(baud)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = speed | termios.CLOCAL | termios.CREAD | termios.CS8
    if hasattr(termios, "CRTSCTS"):
        attrs[2] &= ~termios.CRTSCTS
    attrs[3] = 0
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return old


def require_hex_field(line: str, pattern: re.Pattern[str], expected: int, label: str) -> None:
    match = pattern.search(line)
    if not match:
        raise ProtocolError(f"{label} did not report its hexadecimal field: {line}")
    observed = int(match.group(1), 16)
    if observed != expected:
        raise ProtocolError(f"{label} reported 0x{observed:08x}; expected 0x{expected:08x}")


def inspect_live_payload(path: Path, descriptor_path: Path | None = None) -> LivePayload:
    data = path.read_bytes()
    if len(LIVE_MARKER_RE.findall(data)) != 1:
        raise ProtocolError("PMOSLIVE payload must contain exactly one valid flash-disabled descriptor")
    if any(marker in data for marker in (b"ERASEFLASH", b"FLASH-PREFLIGHT", b"PROGRESS ERASE", b"PROGRESS PROGRAM")):
        raise ProtocolError("PMOSLIVE payload contains a forbidden flash-write marker")
    if descriptor_path is None:
        descriptor_path = path.with_suffix(".descriptor.json")
    if not descriptor_path.is_file():
        raise ProtocolError(f"PMOSLIVE descriptor is missing: {descriptor_path}")
    try:
        metadata = json.loads(descriptor_path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"invalid PMOSLIVE descriptor: {exc}") from exc
    digest = hashlib.sha256(data).hexdigest()
    binary = metadata.get("binary", {})
    if metadata.get("format") != "postmerkos.uart-liveboot-payload.v1":
        raise ProtocolError("unsupported PMOSLIVE descriptor format")
    if metadata.get("soc_family") != "jaguar1" or metadata.get("soc_family_id") != 2:
        raise ProtocolError("PMOSLIVE descriptor is not for Jaguar1")
    if metadata.get("flash_access") != "none":
        raise ProtocolError("PMOSLIVE descriptor does not prohibit flash access")
    if metadata.get("load_address") != 0x86C00000 or metadata.get("entry_address") != 0x86C00000:
        raise ProtocolError("PMOSLIVE must be linked at 0x86c00000")
    if binary.get("filename") != path.name or binary.get("bytes") != len(data) or binary.get("sha256") != digest:
        raise ProtocolError("PMOSLIVE descriptor binary record mismatch")
    return LivePayload(path, len(data), digest, 0x86C00000, 0x86C00000)


def accept_live_ready(link: SerialLink, ready_line: str) -> None:
    if not LIVE_READY_RE.fullmatch(ready_line):
        raise ProtocolError(f"invalid PMOSLIVE ready line: {ready_line}")
    descriptor = link.wait_for(("PMOSLIVE DESCRIPTOR ",), 5.0)
    if not LIVE_DESCRIPTOR_RE.fullmatch(descriptor):
        raise ProtocolError(f"invalid PMOSLIVE descriptor line: {descriptor}")
    link.wait_for(("PMOSLIVE RAM-MAP ",), 5.0)
    link.wait_for(("PMOSLIVE UART-CAP ",), 5.0)
    link.wait_for(("PMOSLIVE COMMAND-READY 3",), 5.0)


def enter_liveboot(link: SerialLink, path: str, timeout: float,
                   payload: LivePayload | None, chunk_size: int,
                   frame_retries: int, ack_timeout: float) -> str:
    line = link.wait_for(("PMOSBOOT MENU-PROBE", "PMOSLIVE READY 3", "PMOSRAM READY 2"), timeout)
    if line.startswith("PMOSLIVE READY 3"):
        accept_live_ready(link, line)
        return "already-running"
    if line.startswith("PMOSBOOT MENU-PROBE"):
        link.write_all(b"\r")
        trigger = link.wait_for(("PMOSBOOT PASS-MENU-TRIGGER",), 4.0,
                                error_prefixes=("PMOSBOOT WARN-MENU-TIMEOUT",))
        require_hex_field(trigger, MENU_BYTE_RE, 0x0D, "menu trigger")
        link.wait_for(("PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT",), 4.0)
        link.wait_for(("PMOSBOOT MENU-READY",), 4.0)
        choice = b"3" if path in {"embedded", "auto"} else b"1"
        link.write_all(choice)
        selected = link.wait_for(("PMOSBOOT PASS-MENU-CHOICE",), 4.0)
        require_hex_field(selected, MENU_SELECTION_RE, int(choice), "menu selection")
        if choice == b"3":
            link.wait_for(("PMOSBOOT INFO-LIVEBOOT",), 5.0,
                          error_prefixes=("PMOSBOOT FAIL-LIVEBOOT",))
            link.wait_for(("PMOSBOOT PASS-LIVEBOOT-COPY",), 10.0)
            link.wait_for(("PMOSBOOT PASS-LIVEBOOT-EXEC",), 10.0)
            line = link.wait_for(("PMOSLIVE READY 3",), 10.0)
        else:
            line = link.wait_for(("PMOSRAM READY 2",), 10.0)
    if line.startswith("PMOSRAM READY 2"):
        if payload is None:
            raise ProtocolError("RAM-upload liveboot requires --payload")
        send_ram_payload(
            link, payload.path.read_bytes(), payload.load_address, payload.entry_address,
            chunk_size, frame_retries, ack_timeout,
        )
        line = link.wait_for(("PMOSLIVE READY 3",), 10.0)
    accept_live_ready(link, line)
    return "embedded" if path in {"embedded", "auto"} else "ram-upload"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("verify", "dry-run", "boot"), default="verify")
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--liveboot-path", choices=("embedded", "ram-upload", "auto"), default="embedded")
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--payload-descriptor", type=Path)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--target-model", choices=("MS42", "MS42P"), default="MS42P")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--manual-target-confirmation", action="store_true")
    parser.add_argument("--chunk-size", type=int, default=1024)
    parser.add_argument("--frame-retries", type=int, default=3)
    parser.add_argument("--ack-timeout", type=float, default=5.0)
    parser.add_argument("--ready-timeout", type=float, default=90.0)
    parser.add_argument("--boot-timeout", type=float, default=120.0)
    parser.add_argument("--skip-baud-negotiation", action="store_true")
    parser.add_argument("--diagnostic-baud-scan", action="store_true")
    parser.add_argument("--diagnostic-window-scan", action="store_true")
    parser.add_argument("--verbose-acks", action="store_true")
    args = parser.parse_args()

    if args.baud != 115200:
        raise ProtocolError("PMOSRAM/PMOSLIVE bootstrap must start at 115200 baud")
    manifest = args.manifest or Path(str(args.firmware) + ".manifest.json")
    bundle = validate_bundle(args.firmware, manifest, args.target_model,
                             force=args.force or args.operation != "boot")
    if MODEL_FAMILY[args.target_model] != "jaguar1":
        raise ProtocolError("PMOSLIVE currently supports Jaguar1 only")

    payload = inspect_live_payload(args.payload, args.payload_descriptor) if args.payload else None
    if payload is not None:
        validate_live_payload_binding(payload.size, payload.sha256, bundle)
    if args.liveboot_path == "ram-upload" and payload is None:
        raise ProtocolError("--liveboot-path ram-upload requires --payload")
    print(f"firmware: {args.firmware} ({args.firmware.stat().st_size} bytes)")
    print(f"manifest: {manifest} ({len(bundle.manifest_bytes)} bytes)")
    print(f"target: {bundle.model} / {bundle.family} / status={bundle.model_status}")
    print(f"liveboot path: {args.liveboot_path}")
    if payload:
        print(f"PMOSLIVE: {payload.path} ({payload.size} bytes, sha256 {payload.sha256})")
    if args.operation == "verify":
        print("PMOSLIVE bundle and payload validation completed without opening the serial port")
        return 0
    if not args.port:
        raise ProtocolError("--port is required for dry-run or boot")

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    old = configure_serial(fd, args.baud)
    link = SerialLink(fd)
    controller = BaudController(fd, args.baud)
    try:
        print("Reset or power-cycle the switch; waiting for the meraki-redboot menu...", flush=True)
        try:
            selected = enter_liveboot(
                link, args.liveboot_path, args.ready_timeout, payload,
                args.chunk_size, args.frame_retries, args.ack_timeout,
            )
        except ProtocolError as exc:
            if args.liveboot_path != "auto" or payload is None:
                raise
            print(f"embedded PMOSLIVE entry failed: {exc}", file=sys.stderr, flush=True)
            print(
                "Power-cycle now; automatic fallback will select menu option 1 and upload PMOSLIVE.",
                file=sys.stderr,
                flush=True,
            )
            link.buffer.clear()
            selected = enter_liveboot(
                link, "ram-upload", args.ready_timeout, payload,
                args.chunk_size, args.frame_retries, args.ack_timeout,
            )
        print(f"PMOSLIVE ready through {selected}", flush=True)
        baud = args.baud if args.skip_baud_negotiation else negotiate_fastest_baud(
            link, controller, diagnostic_scan=args.diagnostic_baud_scan
        )
        transport = qualify_transport(
            link, baud, verbose_acks=args.verbose_acks,
            diagnostic_window_scan=args.diagnostic_window_scan,
        )
        result = send_liveboot_v3(
            link, bundle, transport,
            dry_run=args.operation == "dry-run",
            force=args.force or args.operation == "dry-run",
            auto_confirm=not args.manual_target_confirmation,
            verbose_acks=args.verbose_acks,
            baud_controller=controller,
            boot_timeout=args.boot_timeout,
        )
        print(result)
        return 0
    finally:
        try:
            controller.set_rate(115200)
        except Exception:
            pass
        termios.tcsetattr(fd, termios.TCSANOW, old)
        os.close(fd)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ProtocolError as exc:
        print(f"PMOSLIVE error: {exc}", file=sys.stderr)
        raise SystemExit(1)
