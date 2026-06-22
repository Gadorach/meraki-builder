#!/usr/bin/env python3
"""Validate or run meraki-redboot RAM-loader + PMOSREC v3 recovery."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import termios

from bootloader_protocol import (
    ProtocolError,
    EmbeddedRecoveryEntryError,
    SerialLink,
    inspect_payload,
    MODEL_FAMILY,
    send_ram_payload,
    validate_bundle,
    validate_recovery_payload,
)
from pmosrec_v3 import (
    BaudController,
    negotiate_fastest_baud,
    qualify_transport,
    send_package_v3,
)

READY_SOC = re.compile(r"\bSOC=(luton26|jaguar1)\b")
INFO_SOC = re.compile(r"\bSOC:\s*(luton26|jaguar1)\b")
RECOVERY_DESCRIPTOR = re.compile(
    r"^PMOSREC DESCRIPTOR PMOSRECOVERY3;SOC=(luton26|jaguar1);"
    r"FAMILY=([12]);SPI=([0-9a-fA-F]{8});PROTO=3;PREFLIGHT=4;"
    r"BAUDTEST=1;FRAME_MAX=4096;WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;"
    r"CONFIRM_RETRY=1;AUTO_CONFIRM=1;AUTO_REBOOT=1;END$"
)
MENU_BYTE = re.compile(r"\bBYTE:\s*0x([0-9a-fA-F]{8})\b")
MENU_SELECTION = re.compile(r"\bSELECTED:\s*0x([0-9a-fA-F]{8})\b")
UART_CAP = re.compile(r"^PMOSREC UART-CAP CLOCK=(\d+) DIV_MIN=1 DIV_MAX=65535 CURRENT=(\d+)$")


def baud_constant(baud: int) -> int:
    name = f"B{baud}"
    if not hasattr(termios, name):
        raise ProtocolError(f"termios does not support stable bootstrap baud rate {baud}")
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


def require_soc(line: str, expected: str, stage: str) -> None:
    match = READY_SOC.search(line)
    if not match:
        raise ProtocolError(f"{stage} did not report a parseable SoC family: {line}")
    if match.group(1) != expected:
        raise ProtocolError(f"{stage} reports {match.group(1)}, but the selected model requires {expected}")


def require_info_soc(line: str, expected: str) -> None:
    match = INFO_SOC.search(line)
    if not match:
        raise ProtocolError(f"embedded recovery launch did not report a parseable SoC family: {line}")
    if match.group(1) != expected:
        raise ProtocolError(
            f"embedded recovery launch reports {match.group(1)}, but the selected model requires {expected}"
        )


def require_hex_field(line: str, pattern: re.Pattern[str], expected: int, label: str) -> None:
    match = pattern.search(line)
    if not match:
        raise ProtocolError(f"{label} did not report the expected hexadecimal field: {line}")
    observed = int(match.group(1), 16)
    if observed != expected:
        raise ProtocolError(f"{label} reported 0x{observed:08x}; expected 0x{expected:08x}")


def wait_for_recovery_descriptor(link: SerialLink, expected_family: str) -> None:
    line = link.wait_for(("PMOSREC DESCRIPTOR ",), 5.0)
    match = RECOVERY_DESCRIPTOR.fullmatch(line)
    if not match:
        raise ProtocolError(f"recovery stage emitted an invalid PMOSREC v3 descriptor: {line}")
    family = match.group(1)
    family_id = int(match.group(2))
    spi_address = int(match.group(3), 16)
    expected_id = 1 if expected_family == "luton26" else 2
    expected_spi = 0x70000064 if expected_family == "luton26" else 0x70000068
    if family != expected_family or family_id != expected_id or spi_address != expected_spi:
        raise ProtocolError(
            f"recovery descriptor mismatch: family={family} id={family_id} spi=0x{spi_address:08x}"
        )
    cap = link.wait_for(("PMOSREC UART-CAP ",), 5.0)
    if not UART_CAP.fullmatch(cap):
        raise ProtocolError(f"invalid target UART capability record: {cap}")


def accept_recovery_ready(link: SerialLink, ready_line: str, expected_family: str,
                          stage: str) -> None:
    require_soc(ready_line, expected_family, stage)
    wait_for_recovery_descriptor(link, expected_family)
    flash_ready = link.wait_for(("PMOSREC FLASH-PREFLIGHT-OK",), 10.0)
    if "ID=" not in flash_ready:
        raise ProtocolError(f"recovery hardware preflight did not report a JEDEC ID: {flash_ready}")
    link.wait_for(("PMOSREC COMMAND-READY 3",), 5.0)


def wait_for_embedded_recovery(link: SerialLink, expected_family: str) -> None:
    error_prefixes = ("PMOSBOOT FAIL-RECOVERY", "PMOSBOOT WARN-MENU-TIMEOUT")
    info = link.wait_for(("PMOSBOOT INFO-RECOVERY",), 5.0, error_prefixes=error_prefixes)
    require_info_soc(info, expected_family)
    for marker in (
        "PMOSBOOT PASS-RECOVERY-SIZE",
        "PMOSBOOT PASS-RECOVERY-COPY",
        "PMOSBOOT PASS-RECOVERY-EXEC",
    ):
        link.wait_for((marker,), 10.0, error_prefixes=error_prefixes)


def enter_recovery(
    link: SerialLink,
    path: str,
    expected_family: str,
    timeout: float,
    payload: bytes | None,
    load: int,
    entry: int,
    chunk_size: int,
    frame_retries: int,
    ack_timeout: float,
) -> str:
    prefixes = ("PMOSBOOT MENU-PROBE", "PMOSREC READY 3", "PMOSRAM READY 2")
    line = link.wait_for(prefixes, timeout)
    if line.startswith("PMOSREC READY 3"):
        accept_recovery_ready(link, line, expected_family, "automatic embedded recovery")
        return "embedded"
    if line.startswith("PMOSBOOT MENU-PROBE"):
        link.write_all(b"\r")
        trigger = link.wait_for(
            ("PMOSBOOT PASS-MENU-TRIGGER",), 4.0,
            error_prefixes=("PMOSBOOT WARN-MENU-TIMEOUT",),
        )
        require_hex_field(trigger, MENU_BYTE, 0x0D, "menu trigger")
        link.wait_for(("PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY",), 4.0)
        link.wait_for(("PMOSBOOT MENU-READY",), 4.0)
        choice = b"1" if path == "ram-upload" else b"2"
        link.write_all(choice)
        selected = link.wait_for(("PMOSBOOT PASS-MENU-CHOICE",), 4.0)
        require_hex_field(selected, MENU_SELECTION, int(choice), "menu selection")
        if choice == b"2":
            wait_for_embedded_recovery(link, expected_family)
            try:
                line = link.wait_for(("PMOSREC READY 3",), 10.0)
            except ProtocolError as exc:
                if "timed out" not in str(exc):
                    raise
                raise EmbeddedRecoveryEntryError(
                    "embedded recovery did not enter PMOSREC v3 after PASS-RECOVERY-EXEC"
                ) from exc
        else:
            line = link.wait_for(("PMOSRAM READY 2",), 10.0)
    if line.startswith("PMOSREC READY 3"):
        accept_recovery_ready(link, line, expected_family, "embedded recovery")
        return "embedded"
    if not line.startswith("PMOSRAM READY 2"):
        raise ProtocolError(f"unexpected bootloader recovery state: {line}")
    require_soc(line, expected_family, "UART RAM loader")
    if path == "embedded":
        raise ProtocolError("target entered RAM loader while embedded recovery was requested")
    if payload is None:
        raise ProtocolError("external RAM-loader recovery requires --payload")
    send_ram_payload(link, payload, load, entry, chunk_size, frame_retries, ack_timeout)
    payload_ready = link.wait_for(("PMOSREC READY 3",), 10.0)
    accept_recovery_ready(link, payload_ready, expected_family, "uploaded recovery payload")
    return "ram-upload"


def write_preflight_receipt(path: Path, args: argparse.Namespace, selected_path: str,
                            descriptor, transport, result: str) -> None:
    receipt = {
        "format": "postmerkos.bootloader-preflight-receipt.v2",
        "result": "pass",
        "completed_utc": datetime.now(timezone.utc).isoformat(),
        "target_model": args.target_model,
        "soc_family": MODEL_FAMILY[args.target_model],
        "recovery_path": selected_path,
        "serial_device": args.port,
        "scratch_address": int(args.preflight_scratch, 0),
        "scratch_bytes": 64 * 1024,
        "pattern_seed": int(args.preflight_seed, 0),
        "restore_original": True,
        "recovery_payload_sha256": descriptor.sha256 if descriptor else None,
        "hardware_preflight_contract": "spi-nor-scratch-rw-restore-loader-crc-v4",
        "adaptive_transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
        "negotiated_baud": transport.baud,
        "frame_size": transport.frame_size,
        "window_size": transport.window_size,
        "sparse_qualified": transport.sparse_ok,
        "lz4_qualified": transport.lz4_ok,
        "target_result_line": result,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def tune_usb_serial_latency(port: str) -> tuple[Path, str] | None:
    """Best-effort FTDI latency reduction; absence or permissions never block recovery."""
    tty = Path(port).resolve().name
    candidates = (
        Path("/sys/bus/usb-serial/devices") / tty / "latency_timer",
        Path("/sys/class/tty") / tty / "device" / "latency_timer",
    )
    for candidate in candidates:
        if not candidate.exists():
            continue
        try:
            original = candidate.read_text(encoding="ascii").strip()
            if original != "1":
                candidate.write_text("1\n", encoding="ascii")
                print(f"[flasher] USB-serial latency timer: {original} ms -> 1 ms", flush=True)
            else:
                print("[flasher] USB-serial latency timer already set to 1 ms", flush=True)
            return candidate, original
        except OSError as exc:
            print(f"[flasher] Could not tune {candidate}: {exc}; continuing safely.", file=sys.stderr, flush=True)
            return None
    return None


def restore_usb_serial_latency(state: tuple[Path, str] | None) -> None:
    if state is None:
        return
    path, original = state
    try:
        if path.exists() and path.read_text(encoding="ascii").strip() != original:
            path.write_text(original + "\n", encoding="ascii")
            print(f"[flasher] Restored USB-serial latency timer to {original} ms", flush=True)
    except OSError as exc:
        print(f"[flasher] Could not restore {path}: {exc}", file=sys.stderr, flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("verify", "preflight", "dry-run", "flash"), default="verify")
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200, help="stable RAM-loader bootstrap baud; keep at 115200")
    parser.add_argument("--recovery-path", choices=("embedded", "ram-upload", "auto"), default="ram-upload")
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--payload-descriptor", type=Path)
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--target-model", required=True)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--load-address", default="0x81000000")
    parser.add_argument("--entry")
    parser.add_argument("--chunk-size", type=int, default=1024, help="stable RAM-loader payload chunk size")
    parser.add_argument("--frame-retries", type=int, default=3)
    parser.add_argument("--ack-timeout", type=float, default=5.0)
    parser.add_argument("--ready-timeout", type=float, default=90.0)
    parser.add_argument("--fallback-ready-timeout", type=float, default=120.0)
    parser.add_argument("--operation-timeout", type=float, default=1800.0)
    parser.add_argument("--manual-target-confirmation", action="store_true")
    parser.add_argument("--auto-confirm-erase", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--host-full-flash-authorized", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--verbose-acks", action="store_true")
    parser.add_argument("--skip-baud-negotiation", action="store_true")
    parser.add_argument(
        "--diagnostic-baud-scan", action="store_true",
        help="try the legacy broad divisor scan and midpoint refinement",
    )
    parser.add_argument(
        "--diagnostic-window-scan", action="store_true",
        help="test paced multi-frame windows after the safe one-frame baseline",
    )
    parser.add_argument("--preflight-scratch", default="0x00ff0000")
    parser.add_argument("--preflight-seed", default="0x504d4f53")
    parser.add_argument("--preflight-receipt", type=Path)
    args = parser.parse_args()

    if args.baud != 115200:
        raise ProtocolError("RAM-loader bootstrap must remain at the stable 115200 baud")
    if args.target_model not in MODEL_FAMILY:
        raise ProtocolError(f"unsupported exact target model: {args.target_model}")
    auto_confirm_authorized = args.host_full_flash_authorized or args.auto_confirm_erase
    if args.operation == "flash" and not args.manual_target_confirmation and not auto_confirm_authorized:
        raise ProtocolError(
            "automatic ERASEFLASH response requires prior host full-flash authorization; "
            "use the firmware-flasher wrapper or --manual-target-confirmation"
        )
    compatibility_override = args.force or args.operation in ("verify", "preflight", "dry-run")
    bundle = None
    manifest = None
    if args.operation != "preflight":
        if args.firmware is None:
            raise ProtocolError("--firmware is required except for preflight")
        manifest = args.manifest or Path(str(args.firmware) + ".manifest.json")
        bundle = validate_bundle(args.firmware, manifest, args.target_model, force=compatibility_override)

    payload_data: bytes | None = None
    descriptor = None
    if args.payload is not None:
        descriptor = inspect_payload(args.payload, args.payload_descriptor)
        if descriptor.family != MODEL_FAMILY[args.target_model]:
            raise ProtocolError("recovery payload family does not match the selected target")
        if bundle is not None:
            validate_recovery_payload(args.payload, descriptor, bundle)
        payload_data = args.payload.read_bytes()
        if len(payload_data) > 4 * 1024 * 1024:
            raise ProtocolError("recovery payload exceeds the loader's 4 MiB limit")
    if args.recovery_path == "ram-upload" and payload_data is None:
        raise ProtocolError("--recovery-path ram-upload requires --payload")

    load = int(args.load_address, 0)
    entry = int(args.entry, 0) if args.entry else load
    if bundle is not None:
        print(f"firmware: {args.firmware} ({args.firmware.stat().st_size} bytes)")
        print(f"manifest: {manifest} ({len(bundle.manifest_bytes)} bytes)")
        print(f"target: {bundle.model} / boot-family={bundle.family} / status={bundle.model_status}")
    else:
        print(f"target: {args.target_model} / boot-family={MODEL_FAMILY[args.target_model]}")
        print(f"preflight scratch: {int(args.preflight_scratch, 0):#010x} (64 KiB, restored after test)")
    print(f"recovery path: {args.recovery_path}")
    if descriptor:
        print(f"external payload: {args.payload} ({descriptor.family}, {descriptor.size} bytes, sha256 {descriptor.sha256})")
    if args.operation == "verify":
        print("verify completed without opening the serial port or sending data")
        return 0
    if not args.port:
        raise ProtocolError("--port is required for hardware operations")

    latency_state = tune_usb_serial_latency(args.port)
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    old = configure_serial(fd, args.baud)
    link = SerialLink(fd)
    controller = BaudController(fd, args.baud)
    try:
        print("Reset or power-cycle the switch now; waiting for the meraki-redboot recovery menu...")
        try:
            selected_path = enter_recovery(
                link, args.recovery_path, MODEL_FAMILY[args.target_model], args.ready_timeout,
                payload_data, load, entry, args.chunk_size, args.frame_retries, args.ack_timeout,
            )
        except EmbeddedRecoveryEntryError as exc:
            if args.recovery_path != "auto" or payload_data is None:
                raise
            print(f"embedded recovery failed: {exc}", file=sys.stderr, flush=True)
            print("Power-cycle now; automatic fallback will select RAM-loader option 1.", file=sys.stderr, flush=True)
            link.discard_buffer()
            selected_path = enter_recovery(
                link, "ram-upload", MODEL_FAMILY[args.target_model], args.fallback_ready_timeout,
                payload_data, load, entry, args.chunk_size, args.frame_retries, args.ack_timeout,
            )
        print(f"target PMOSREC v3 ready through {selected_path}", flush=True)

        baud = (
            args.baud if args.skip_baud_negotiation
            else negotiate_fastest_baud(
                link, controller, diagnostic_scan=args.diagnostic_baud_scan
            )
        )
        transport = qualify_transport(
            link, baud, verbose_acks=args.verbose_acks,
            diagnostic_window_scan=args.diagnostic_window_scan,
        )

        if args.operation == "preflight":
            link.write_all(
                f"PMOS3 PREFLIGHT {int(args.preflight_scratch, 0)} {int(args.preflight_seed, 0)}\n".encode("ascii")
            )
            link.wait_for(("PMOSPFT HEADER-ACK",), 5.0)
            result = link.wait_for(("PMOSREC RESULT PREFLIGHT-OK",), args.operation_timeout)
            print(result)
            if args.preflight_receipt:
                write_preflight_receipt(args.preflight_receipt, args, selected_path, descriptor, transport, result)
                print(f"preflight receipt: {args.preflight_receipt}")
            print("PMOSREC v3 adaptive UART and destructive NOR preflight completed successfully")
            return 0

        assert bundle is not None
        result = send_package_v3(
            link, bundle, transport,
            dry_run=args.operation == "dry-run",
            force=compatibility_override,
            auto_confirm=(not args.manual_target_confirmation and auto_confirm_authorized),
            verbose_acks=args.verbose_acks,
            operation_timeout=args.operation_timeout,
            baud_controller=controller,
        )
        print(result)
        print("pre-kernel recovery completed successfully")
        return 0
    finally:
        try:
            controller.set_rate(115200)
        except Exception:
            pass
        termios.tcsetattr(fd, termios.TCSANOW, old)
        os.close(fd)
        restore_usb_serial_latency(latency_state)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ProtocolError as exc:
        print(f"bootloader recovery error: {exc}", file=sys.stderr)
        raise SystemExit(1)
