#!/usr/bin/env python3
"""Small read-only TFTP server used only by firmware-flasher.sh."""

from __future__ import annotations

import argparse
import socket
import struct
import threading
from pathlib import Path

OP_RRQ = 1
OP_DATA = 3
OP_ACK = 4
OP_ERROR = 5
OP_OACK = 6


def log(message: str) -> None:
    print(f"[TFTP] {message}", flush=True)


def send_error(sock: socket.socket, peer: tuple[str, int], code: int, message: str) -> None:
    packet = struct.pack("!HH", OP_ERROR, code) + message.encode("ascii", "replace") + b"\0"
    try:
        sock.sendto(packet, peer)
    except OSError:
        pass


def parse_rrq(data: bytes) -> tuple[str, str, dict[str, str]] | None:
    if len(data) < 4 or struct.unpack("!H", data[:2])[0] != OP_RRQ:
        return None
    fields = data[2:].split(b"\0")
    if len(fields) < 3:
        return None
    try:
        filename = fields[0].decode("utf-8", "strict")
    except UnicodeDecodeError:
        return None
    mode = fields[1].decode("ascii", "ignore").lower()
    options: dict[str, str] = {}
    rest = fields[2:]
    for index in range(0, len(rest) - 1, 2):
        if not rest[index]:
            break
        key = rest[index].decode("ascii", "ignore").lower()
        value = rest[index + 1].decode("ascii", "ignore")
        options[key] = value
    return filename, mode, options


def wait_for_ack(
    sock: socket.socket,
    peer: tuple[str, int],
    expected_block: int,
    packet: bytes,
    timeout: int,
    attempts: int = 6,
) -> bool:
    sock.settimeout(timeout)
    for _ in range(attempts):
        sock.sendto(packet, peer)
        try:
            while True:
                reply, sender = sock.recvfrom(65535)
                if sender != peer or len(reply) < 4:
                    continue
                opcode, block = struct.unpack("!HH", reply[:4])
                if opcode == OP_ACK and block == expected_block:
                    return True
                if opcode == OP_ERROR:
                    return False
        except socket.timeout:
            continue
    return False


def resolve_requested_file(root: Path, filename: str) -> Path | None:
    relative = Path(filename.lstrip("/"))
    if relative.is_absolute() or ".." in relative.parts:
        return None
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        return None
    return path


def serve_request(root: Path, bind_ip: str, data: bytes, peer: tuple[str, int]) -> None:
    parsed = parse_rrq(data)
    if not parsed:
        return
    filename, mode, requested = parsed
    if mode not in ("octet", "netascii"):
        return

    path = resolve_requested_file(root, filename)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tx:
        tx.bind((bind_ip, 0))
        if path is None:
            send_error(tx, peer, 2, "Access violation")
            return
        if not path.is_file():
            log(f"not found: {filename} requested by {peer[0]}")
            send_error(tx, peer, 1, "File not found")
            return

        block_size = 512
        timeout = 2
        accepted: dict[str, str] = {}
        if "blksize" in requested:
            try:
                candidate = int(requested["blksize"])
                if 8 <= candidate <= 65464:
                    block_size = candidate
                    accepted["blksize"] = str(candidate)
            except ValueError:
                pass
        if "timeout" in requested:
            try:
                candidate = int(requested["timeout"])
                if 1 <= candidate <= 10:
                    timeout = candidate
                    accepted["timeout"] = str(candidate)
            except ValueError:
                pass
        if "tsize" in requested:
            accepted["tsize"] = str(path.stat().st_size)

        log(f"serving {filename} ({path.stat().st_size} bytes) to {peer[0]}:{peer[1]}")
        if accepted:
            payload = b"".join(
                key.encode("ascii") + b"\0" + value.encode("ascii") + b"\0"
                for key, value in accepted.items()
            )
            if not wait_for_ack(tx, peer, 0, struct.pack("!H", OP_OACK) + payload, timeout):
                log(f"option negotiation failed for {filename}")
                return

        block = 1
        sent = 0
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(block_size)
                packet = struct.pack("!HH", OP_DATA, block) + chunk
                if not wait_for_ack(tx, peer, block, packet, timeout):
                    log(f"transfer timed out: {filename}, block {block}")
                    return
                sent += len(chunk)
                if len(chunk) < block_size:
                    break
                block = (block + 1) & 0xFFFF
        log(f"completed {filename}: {sent} bytes sent to {peer[0]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"TFTP root does not exist: {root}")
    if not 1 <= args.port <= 65535:
        parser.error("port must be from 1 through 65535")

    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.bind, args.port))
    log(f"read-only server listening on {args.bind}:{args.port}; root={root}")
    while True:
        data, peer = server.recvfrom(65535)
        if len(data) >= 2 and struct.unpack("!H", data[:2])[0] == OP_RRQ:
            threading.Thread(
                target=serve_request,
                args=(root, args.bind, data, peer),
                daemon=True,
            ).start()


if __name__ == "__main__":
    raise SystemExit(main())
