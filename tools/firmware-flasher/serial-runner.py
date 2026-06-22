#!/usr/bin/env python3
"""Drive a postmerkOS firmware update through the hardware serial console.

This helper belongs only to firmware-flasher.sh. It understands both the current
postmerkOS serial getty/menu and the older direct-shell login behavior.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import fcntl
import os
import re
import select
import sys
import termios
import time


def configure_serial(fd: int, baud: int) -> None:
    baud_map = {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
    }
    if baud not in baud_map:
        raise ValueError(f"unsupported baud rate: {baud}")
    attrs = termios.tcgetattr(fd)
    attrs[0] = termios.IGNBRK | termios.IXON | termios.IXOFF
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    attrs[4] = baud_map[baud]
    attrs[5] = baud_map[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)


def write_bytes(fd: int, payload: bytes) -> None:
    view = memoryview(payload)
    while view:
        try:
            written = os.write(fd, view)
            view = view[written:]
        except BlockingIOError:
            select.select([], [fd], [], 1.0)


def wait_serial(fd: int, pattern: re.Pattern[str], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    buffer = ""
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], min(0.25, deadline - time.monotonic()))
        if not readable:
            continue
        try:
            data = os.read(fd, 65536)
        except BlockingIOError:
            continue
        if not data:
            continue
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        buffer = (buffer + data.decode("utf-8", "replace"))[-65536:]
        if "PMOSUART/1 ERROR" in buffer:
            raise RuntimeError(buffer.split("PMOSUART/1 ERROR", 1)[-1].splitlines()[0].strip())
        if pattern.search(buffer):
            return buffer
    raise TimeoutError(f"timed out waiting for {pattern.pattern}")


def send_uart_object(fd: int, kind: str, path: str, chunk_size: int, timeout: float) -> None:
    name = os.path.basename(path)
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
        raise ValueError(f"unsafe UART object name: {name}")
    size = os.path.getsize(path)
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    hexdigest = digest.hexdigest()
    write_bytes(fd, f"PMOSUART/1 BEGIN {kind} {size} {hexdigest} {name}\r\n".encode())
    wait_serial(fd, re.compile(rf"PMOSUART/1 BEGIN-ACK {re.escape(kind)}(?:\r?\n|$)"), timeout)
    sequence = 0
    with open(path, "rb") as stream:
        while True:
            block = stream.read(chunk_size)
            if not block:
                break
            crc = binascii.crc32(block) & 0xFFFFFFFF
            payload = base64.b64encode(block).decode("ascii")
            frame = f"PMOSUART/1 DATA {kind} {sequence} {crc:08x} {payload}\r\n".encode()
            acknowledged = False
            for attempt in range(5):
                write_bytes(fd, frame)
                try:
                    wait_serial(fd, re.compile(rf"PMOSUART/1 ACK {re.escape(kind)} {sequence}(?:\r?\n|$)"), timeout)
                    acknowledged = True
                    break
                except TimeoutError:
                    print(f"\n[serial] retrying {kind} frame {sequence} ({attempt + 1}/5)", flush=True)
            if not acknowledged:
                raise TimeoutError(f"frame {sequence} was not acknowledged")
            sequence += 1
            if sequence % 128 == 0 or stream.tell() == size:
                print(f"\n[serial] {kind}: {stream.tell()}/{size} bytes ({stream.tell() * 100 // size}%)", flush=True)
    write_bytes(fd, f"PMOSUART/1 END {kind} {sequence}\r\n".encode())
    wait_serial(fd, re.compile(rf"PMOSUART/1 OBJECT-OK {re.escape(kind)} {size} {hexdigest}"), timeout)


def perform_uart_transfer(fd: int, firmware: str, manifest: str | None,
                          chunk_size: int, timeout: float) -> None:
    print("\n[serial] PMOSUART/1 receiver is ready; transferring firmware", flush=True)
    send_uart_object(fd, "firmware", firmware, chunk_size, timeout)
    if manifest:
        send_uart_object(fd, "manifest", manifest, chunk_size, timeout)
    write_bytes(fd, b"PMOSUART/1 DONE\r\n")
    wait_serial(fd, re.compile(r"PMOSUART/1 COMPLETE firmware="), timeout)
    print("\n[serial] UART objects reconstructed and verified on the switch", flush=True)

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--username", default="root")
    parser.add_argument("--password-file", required=True)
    parser.add_argument("--command", required=True)
    parser.add_argument("--operation", choices=("verify", "dry-run", "flash"), required=True)
    parser.add_argument("--mode", choices=("modern", "checksum", "legacy"), default="modern")
    parser.add_argument("--accept-untested", action="store_true")
    parser.add_argument("--accept-full-flash", action="store_true")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--uart-firmware")
    parser.add_argument("--uart-manifest")
    parser.add_argument("--uart-chunk", type=int, default=1024)
    args = parser.parse_args()
    if args.uart_chunk < 192 or args.uart_chunk > 3072:
        parser.error("--uart-chunk must be 192-3072 bytes")
    if args.uart_manifest and not args.uart_firmware:
        parser.error("--uart-manifest requires --uart-firmware")

    with open(args.password_file, "r", encoding="utf-8") as stream:
        password = stream.read()

    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        try:
            fcntl.ioctl(fd, termios.TIOCEXCL)
        except OSError:
            pass
        configure_serial(fd, args.baud)

        def send(text: str) -> None:
            payload = memoryview(text.encode("utf-8"))
            while payload:
                try:
                    written = os.write(fd, payload)
                    payload = payload[written:]
                except BlockingIOError:
                    select.select([], [fd], [], 1.0)

        send("\r")
        started = time.monotonic()
        last_nudge = started
        buffer = ""
        command_sent = False
        shell_probe_sent = False
        shell_requested = False
        compatibility_menu_handled = False
        password_sent = False
        reboot_seen = False
        last_error: str | None = None
        accepted_untested = False
        upgrade_confirmed = False
        full_flash_confirmed = False
        uart_transferred = False

        print(
            f"[serial] opened {args.device} at {args.baud} baud, 8N1, XON/XOFF",
            flush=True,
        )
        print("[serial] Ctrl+C aborts host monitoring; it cannot undo a flash already started", flush=True)

        while time.monotonic() - started < args.timeout:
            readable, _, _ = select.select([fd], [], [], 0.25)
            if readable:
                try:
                    data = os.read(fd, 65536)
                except BlockingIOError:
                    data = b""
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                    text = data.decode("utf-8", "replace")
                    buffer = (buffer + text)[-32768:]
                    lower = buffer.lower()

                    if command_sent and args.uart_firmware and not uart_transferred and "pmosuart/1 ready" in lower:
                        try:
                            perform_uart_transfer(fd, args.uart_firmware, args.uart_manifest,
                                                  args.uart_chunk, 20.0)
                        except (OSError, RuntimeError, TimeoutError, ValueError) as error:
                            print(f"\n[serial] UART transfer failed: {error}", file=sys.stderr, flush=True)
                            return 70
                        uart_transferred = True
                        buffer = ""
                        continue

                    if command_sent and "fwupdate|error|" in lower:
                        last_error = "the updater reported an error"
                    if command_sent and (
                        "linuxloader built" in lower or "linux version 3.18" in lower
                    ):
                        reboot_seen = True

                    if command_sent and "type accept-untested to continue:" in lower and not accepted_untested:
                        if not args.accept_untested:
                            print(
                                "\n[serial] candidate requires ACCEPT-UNTESTED, but host approval was not granted",
                                file=sys.stderr,
                                flush=True,
                            )
                            return 65
                        print("\n[serial] sending approved ACCEPT-UNTESTED acknowledgement", flush=True)
                        send("ACCEPT-UNTESTED\r")
                        accepted_untested = True
                        buffer = ""
                        continue

                    if command_sent and "type flash-all to continue:" in lower and not full_flash_confirmed:
                        if not args.accept_full_flash or args.operation != "flash":
                            print(
                                "\n[serial] full-flash authorization was not granted by the host",
                                file=sys.stderr,
                                flush=True,
                            )
                            return 69
                        print("\n[serial] sending locally approved FLASH-ALL confirmation", flush=True)
                        send("FLASH-ALL\r")
                        full_flash_confirmed = True
                        buffer = ""
                        continue

                    if command_sent and "type upgrade to continue:" in lower and not upgrade_confirmed:
                        if args.operation != "flash":
                            print("\n[serial] unexpected UPGRADE prompt during non-flash operation", file=sys.stderr)
                            return 66
                        print("\n[serial] sending locally approved UPGRADE confirmation", flush=True)
                        send("UPGRADE\r")
                        upgrade_confirmed = True
                        buffer = ""
                        continue

                    if command_sent:
                        match = re.search(r"__MFW_RC__:(\d+)", buffer)
                        if match and (args.operation != "flash" or not reboot_seen):
                            rc = int(match.group(1))
                            print(f"\n[serial] updater returned status {rc} before reboot", flush=True)
                            return rc

                    if command_sent and args.operation == "flash" and reboot_seen:
                        if re.search(r"(?i)(?:^|[\r\n]).*login:\s*$", buffer) or \
                           "starting dropbear sshd: ok" in lower or \
                           "press enter or type \"pmc\"" in lower:
                            print(
                                "\n[serial] reboot completed and postmerkOS reached its login/services stage",
                                flush=True,
                            )
                            return 1 if last_error else 0

                    if not command_sent:
                        # Current unauthenticated serial getty. Asking for "shell"
                        # bypasses menu traversal without weakening firmware policy.
                        if re.search(r"(?im)^pmc:\s*$", buffer):
                            print("\n[serial] current postmerkOS getty detected; requesting raw shell", flush=True)
                            send("shell\r")
                            shell_requested = True
                            shell_probe_sent = False
                            buffer = ""
                            continue

                        if re.search(r"(?i)login:\s*$", buffer):
                            send(args.username + "\r")
                            buffer = ""
                            continue

                        if "login incorrect" in lower or "authentication failure" in lower:
                            print("\n[serial] console authentication failed", file=sys.stderr)
                            return 68

                        if re.search(r"(?i)password:\s*$", buffer) and not password_sent:
                            send(password + "\r")
                            password_sent = True
                            buffer = ""
                            continue

                        # The login profile may show the serial-number password warning.
                        if "press enter to continue..." in lower:
                            send("\r")
                            buffer = ""
                            continue

                        # Do not permanently dismiss a hardware compatibility warning.
                        if re.search(r"(?im)^compatibility>\s*$", buffer) and not compatibility_menu_handled:
                            print("\n[serial] compatibility notice detected; selecting 'remind me next time'", flush=True)
                            send("4\r")
                            compatibility_menu_handled = True
                            buffer = ""
                            continue

                        # Authenticated SSH/serial logins enter the management menu.
                        if re.search(r"(?im)^postmerkOS>\s*$", buffer):
                            if not shell_requested:
                                print("\n[serial] management menu detected; selecting Shell", flush=True)
                                send("shell\r")
                                shell_requested = True
                                shell_probe_sent = False
                                buffer = ""
                                continue

                        if "permission denied." in lower and shell_requested:
                            print(
                                "\n[serial] the logged-in role is not permitted to enter the raw shell",
                                file=sys.stderr,
                            )
                            return 67

                        if re.search(r"(?:^|[\r\n])[^\r\n]*[#\$]\s*$", buffer):
                            if not shell_probe_sent:
                                send("printf '__MFW_SHELL_READY__\\n'\r")
                                shell_probe_sent = True
                                buffer = ""
                                continue

                        if "__MFW_SHELL_READY__" in buffer:
                            wrapped = f"{args.command}; rc=$?; echo __MFW_RC__:$rc"
                            print(f"\n[serial] launching: {args.command}\n", flush=True)
                            send(wrapped + "\r")
                            command_sent = True
                            buffer = ""
                            continue

            now = time.monotonic()
            if not command_sent and now - last_nudge >= 5:
                send("\r")
                last_nudge = now

        if not command_sent:
            print("\n[serial] timed out before a usable shell was reached", file=sys.stderr)
        elif args.operation == "flash":
            print("\n[serial] timed out before the post-flash reboot completed", file=sys.stderr)
        else:
            print("\n[serial] timed out waiting for the updater to return", file=sys.stderr)
        return 124
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
