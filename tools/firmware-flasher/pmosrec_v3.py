#!/usr/bin/env python3
"""PMOSREC v3 adaptive UART transport, feature qualification, and package sender."""
from __future__ import annotations

from dataclasses import dataclass
import fcntl
import hashlib
import math
import os
from pathlib import Path
import re
import struct
import termios
import time
import zlib
from typing import Callable, Iterable, Sequence

from bootloader_protocol import ProtocolError, SerialLink, BundleInfo

PROTOCOL_VERSION = 3
FULL_IMAGE_SIZE = 16 * 1024 * 1024
OBJECT_IMAGE = 1
OBJECT_MANIFEST = 2
OBJECT_TEST = 3
REP_RAW = 0
REP_SPARSE = 1
REP_LZ4 = 2
REP_SPARSE_LZ4 = 3
REP_NAMES = {
    REP_RAW: "raw",
    REP_SPARSE: "sparse",
    REP_LZ4: "lz4",
    REP_SPARSE_LZ4: "sparse-lz4",
}
FRAME_FLAG_LZ4 = 1
FRAME_MAGIC = 0x33464B50
ACK_MAGIC = 0x334B4341
PACKAGE_MAGIC = b"PMOSPKG3"
PACKAGE_HEADER = struct.Struct("<8s13I32s32s16sI")
FRAME_HEADER = struct.Struct("<10I")
ACK_RECORD = struct.Struct("<7I")
ACK_CONFIRM_BYTE = b"\xa5"
ACK_CONFIRM_ATTEMPTS = 4
MAX_WINDOW = 16
BAUD_TEST_BYTES = 32 * 1024
BAUD_TEST_PASSES = 2
BOOT_BAUD = 115200
BOOT_BANNER_PREFIXES = (
    "LinuxLoader built",
    "init_pll ok",
    "Low level initialization complete",
    "PMOSRAM STAGE1 COPY",
    "Linux version",
)

# Linux asm-generic termios2 values. These are stable for the supported Linux hosts.
TCGETS2 = 0x802C542A
TCSETS2 = 0x402C542B
BOTHER = 0x1000
CBAUD = 0x100F
TERMIOS2 = struct.Struct("=IIIIB19BII")

# The normal path makes one descending pass over conventional UART rates and
# stops at the first passing candidate. The broad divisor scan is retained only
# for explicit diagnostics because every failed candidate requires target-side
# rollback and materially delays recovery.
DEFAULT_BAUD_CANDIDATES = (921600, 460800, 230400)
DIAGNOSTIC_BAUD_CANDIDATES = (
    115200, 153600, 230400, 250000, 256000, 307200, 460800, 500000,
    576000, 614400, 750000, 921600, 1_000_000, 1_152_000, 1_500_000,
    2_000_000, 2_500_000, 3_000_000, 3_500_000, 4_000_000, 5_000_000,
    6_000_000, 8_000_000, 10_000_000, 12_000_000,
)

# VCore-III recovery has no RTS/CTS flow control. A one-frame production window
# guarantees that the target can CRC/decode/copy a frame before the host starts
# the next one. Larger windows remain available only for explicit diagnostics
# and receive an inter-frame wire-idle guard.
DEFAULT_WINDOW_SIZE = 1
DIAGNOSTIC_WINDOW_CANDIDATES = (1, 2, 4, 8, 16)
MULTI_FRAME_GUARD_SECONDS = 0.003

BAUD_CANDIDATE_RE = re.compile(
    r"^PMOS3 BAUD-CANDIDATE REQUESTED=(\d+) ACTUAL=(\d+) DIV=(\d+) "
    r"ERROR_PPM=(\d+) CURRENT=(\d+) NONCE=([0-9a-fA-F]{8})$"
)
BAUD_PREPARED_RE = re.compile(
    r"^PMOS3 BAUD-PREPARED RATE=(\d+) DIV=(\d+) NONCE=([0-9a-fA-F]{8}) "
    r"REVERT=(\d+) TEST_MS=(\d+)$"
)
BAUD_SYNC_RE = re.compile(r"^PMOS3 BAUD-SYNC NONCE=([0-9a-fA-F]{8}) RATE=(\d+)$")
BAUD_T2H_RE = re.compile(
    r"^PMOS3 BAUD-T2H PASS=(\d+) SEED=([0-9a-fA-F]{8}) BYTES=(\d+) "
    r"CRC=([0-9a-fA-F]{8}) NONCE=([0-9a-fA-F]{8})$"
)


@dataclass(frozen=True)
class EncodedFrame:
    sequence: int
    offset: int
    decoded_length: int
    flags: int
    payload: bytes
    decoded_crc32: int

    def wire(self, object_id: int) -> bytes:
        wire_crc = zlib.crc32(self.payload) & 0xFFFFFFFF
        head = FRAME_HEADER.pack(
            FRAME_MAGIC,
            object_id,
            self.sequence,
            self.offset,
            self.decoded_length,
            len(self.payload),
            self.flags,
            wire_crc,
            self.decoded_crc32,
            0,
        )
        head = head[:-4] + struct.pack("<I", zlib.crc32(head[:-4]) & 0xFFFFFFFF)
        return head + self.payload


@dataclass(frozen=True)
class RepresentationPlan:
    mode: int
    frame_size: int
    frames: tuple[EncodedFrame, ...]

    @property
    def name(self) -> str:
        return REP_NAMES[self.mode]

    @property
    def wire_bytes(self) -> int:
        return sum(FRAME_HEADER.size + len(frame.payload) for frame in self.frames)


@dataclass(frozen=True)
class TransportSelection:
    baud: int
    frame_size: int
    window_size: int
    sparse_ok: bool
    lz4_ok: bool
    sparse_lz4_ok: bool
    representation: RepresentationPlan | None = None


class BaudController:
    def __init__(self, fd: int, initial_rate: int = 115200) -> None:
        self.fd = fd
        self.current_rate = initial_rate

    def _get_termios2(self) -> list[int]:
        raw = bytearray(TERMIOS2.size)
        fcntl.ioctl(self.fd, TCGETS2, raw, True)
        return list(TERMIOS2.unpack(raw))

    def set_rate(self, rate: int, *, flush: bool = True) -> None:
        if rate <= 0:
            raise ProtocolError(f"invalid UART rate {rate}")
        try:
            values = self._get_termios2()
            values[2] = (values[2] & ~CBAUD) | BOTHER | termios.CLOCAL | termios.CREAD | termios.CS8
            if hasattr(termios, "CRTSCTS"):
                values[2] &= ~termios.CRTSCTS
            values[-2] = rate
            values[-1] = rate
            raw = TERMIOS2.pack(*values)
            fcntl.ioctl(self.fd, TCSETS2, raw)
        except OSError as exc:
            # Standard termios remains useful on platforms that reject termios2.
            name = f"B{rate}"
            if not hasattr(termios, name):
                raise ProtocolError(f"host UART driver cannot configure {rate} baud: {exc}") from exc
            attrs = termios.tcgetattr(self.fd)
            speed = getattr(termios, name)
            attrs[4] = speed
            attrs[5] = speed
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        if flush:
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.current_rate = rate

    def can_set(self, rate: int) -> bool:
        old = self.current_rate
        try:
            self.set_rate(rate)
            self.set_rate(old)
            return True
        except ProtocolError:
            try:
                self.set_rate(old)
            except ProtocolError:
                pass
            return False


def xorshift_bytes(seed: int, length: int) -> bytes:
    state = seed & 0xFFFFFFFF
    output = bytearray(length)
    for index in range(length):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        output[index] = state & 0xFF
    return bytes(output)


def _encode_length(value: int) -> bytes:
    output = bytearray()
    while value >= 255:
        output.append(255)
        value -= 255
    output.append(value)
    return bytes(output)


def lz4_compress_block(data: bytes) -> bytes:
    """Small deterministic LZ4 block compressor; no external runtime dependency."""
    if not data:
        return b""
    table: dict[int, int] = {}
    out = bytearray()
    anchor = 0
    i = 0
    end = len(data)
    while i + 4 <= end:
        key = int.from_bytes(data[i : i + 4], "little")
        candidate = table.get(key)
        table[key] = i
        if candidate is None or i - candidate > 0xFFFF or data[candidate : candidate + 4] != data[i : i + 4]:
            i += 1
            continue
        match = 4
        while i + match < end and data[candidate + match] == data[i + match]:
            match += 1
        literals = i - anchor
        token = min(literals, 15) << 4 | min(match - 4, 15)
        out.append(token)
        if literals >= 15:
            out.extend(_encode_length(literals - 15))
        out.extend(data[anchor:i])
        out.extend((i - candidate).to_bytes(2, "little"))
        if match - 4 >= 15:
            out.extend(_encode_length(match - 4 - 15))
        old_i = i
        i += match
        for pos in range(old_i + 1, min(i, end - 3)):
            table[int.from_bytes(data[pos : pos + 4], "little")] = pos
        anchor = i
    literals = end - anchor
    out.append(min(literals, 15) << 4)
    if literals >= 15:
        out.extend(_encode_length(literals - 15))
    out.extend(data[anchor:])
    return bytes(out)


def lz4_decompress_block(data: bytes, capacity: int) -> bytes:
    si = 0
    out = bytearray()
    while si < len(data):
        token = data[si]
        si += 1
        literal_length = token >> 4
        if literal_length == 15:
            while True:
                if si >= len(data):
                    raise ProtocolError("truncated LZ4 literal length")
                value = data[si]
                si += 1
                literal_length += value
                if value != 255:
                    break
        if si + literal_length > len(data) or len(out) + literal_length > capacity:
            raise ProtocolError("invalid LZ4 literal run")
        out.extend(data[si : si + literal_length])
        si += literal_length
        if si == len(data):
            break
        if si + 2 > len(data):
            raise ProtocolError("truncated LZ4 match offset")
        offset = int.from_bytes(data[si : si + 2], "little")
        si += 2
        if offset == 0 or offset > len(out):
            raise ProtocolError("invalid LZ4 match offset")
        match_length = token & 0x0F
        if match_length == 15:
            while True:
                if si >= len(data):
                    raise ProtocolError("truncated LZ4 match length")
                value = data[si]
                si += 1
                match_length += value
                if value != 255:
                    break
        match_length += 4
        if len(out) + match_length > capacity:
            raise ProtocolError("LZ4 output exceeds block capacity")
        for _ in range(match_length):
            out.append(out[-offset])
    return bytes(out)


def make_plan(data: bytes, frame_size: int, mode: int) -> RepresentationPlan:
    sparse = mode in (REP_SPARSE, REP_SPARSE_LZ4)
    use_lz4 = mode in (REP_LZ4, REP_SPARSE_LZ4)
    frames: list[EncodedFrame] = []
    sequence = 0
    for offset in range(0, len(data), frame_size):
        decoded = data[offset : offset + frame_size]
        if sparse and decoded and all(value == 0xFF for value in decoded):
            continue
        flags = 0
        payload = decoded
        if use_lz4:
            compressed = lz4_compress_block(decoded)
            if len(compressed) < len(decoded):
                if lz4_decompress_block(compressed, frame_size) != decoded:
                    raise ProtocolError("internal LZ4 compressor self-test failed")
                payload = compressed
                flags |= FRAME_FLAG_LZ4
        frames.append(
            EncodedFrame(
                sequence=sequence,
                offset=offset,
                decoded_length=len(decoded),
                flags=flags,
                payload=payload,
                decoded_crc32=zlib.crc32(decoded) & 0xFFFFFFFF,
            )
        )
        sequence += 1
    if not frames:
        # Keep an all-FF object representable while retaining full-object digest verification.
        frames.append(
            EncodedFrame(0, 0, min(frame_size, len(data)), 0,
                         data[: min(frame_size, len(data))],
                         zlib.crc32(data[: min(frame_size, len(data))]) & 0xFFFFFFFF)
        )
    return RepresentationPlan(mode=mode, frame_size=frame_size, frames=tuple(frames))


def make_test_object(size: int = 64 * 1024) -> bytes:
    data = bytearray(b"\xFF" * size)
    chunk = 4096
    data[chunk : 2 * chunk] = b"\x00" * chunk
    data[2 * chunk : 3 * chunk] = (b"postmerkOS-PMOSREC3-" * 256)[:chunk]
    data[3 * chunk : 4 * chunk] = xorshift_bytes(0x13579BDF, chunk)
    for offset in range(4 * chunk, size, chunk):
        if (offset // chunk) % 3 == 0:
            data[offset : offset + chunk] = xorshift_bytes(0x2468ACE0 ^ offset, chunk)
        elif (offset // chunk) % 3 == 1:
            data[offset : offset + chunk] = bytes([offset // chunk & 0xFF]) * chunk
    return bytes(data)


class ProgressTracker:
    def __init__(self, total_wire: int, baud: int, label: str) -> None:
        self.total = max(total_wire, 1)
        self.baud = baud
        self.label = label
        self.started = time.monotonic()
        self.samples: list[tuple[float, int]] = []
        self.acknowledged = 0
        theoretical = total_wire * 10 / max(baud, 1)
        self.initial_seconds = theoretical * 1.08

    @staticmethod
    def _format_remaining(seconds: float) -> str:
        if seconds <= 0:
            return "~0 minutes remaining"
        if seconds < 120:
            return f"~{math.ceil(seconds)} seconds remaining"
        return f"~{math.ceil(seconds / 60)} minutes remaining"

    def update(self, acknowledged_wire: int, *, retransmitted: int = 0) -> None:
        now = time.monotonic()
        self.acknowledged = min(acknowledged_wire, self.total)
        self.samples.append((now, self.acknowledged))
        cutoff = now - 60.0
        self.samples = [sample for sample in self.samples if sample[0] >= cutoff]
        remaining = self.total - self.acknowledged
        elapsed = now - self.started
        if elapsed >= 10.0 and len(self.samples) >= 2:
            dt = self.samples[-1][0] - self.samples[0][0]
            db = self.samples[-1][1] - self.samples[0][1]
            speed = db / dt if dt > 0 and db > 0 else 0.0
            eta = remaining / speed if speed > 0 else self.initial_seconds * remaining / self.total
            speed_text = f" — {speed / 1024:.1f} KiB/s"
        else:
            eta = self.initial_seconds * remaining / self.total
            speed_text = ""
        percent = int(self.acknowledged * 100 / self.total)
        print(
            f"[flasher] {self.label}: {percent}% complete — "
            f"{self.acknowledged}/{self.total} wire bytes{speed_text} — "
            f"{self._format_remaining(eta)}",
            flush=True,
        )


def _read_ack(link: SerialLink, object_id: int, timeout: float) -> tuple[int, int, int, int]:
    """Read a compact ACK while recovering byte alignment from damaged records.

    Non-ACK target output is restored to SerialLink's line buffer on failure so
    FEATURE-FAIL and other terminal status records remain available to the
    command-level resynchronizer.
    """
    deadline = time.monotonic() + timeout
    magic_bytes = struct.pack("<I", ACK_MAGIC)
    buffered = bytearray()
    captured = bytearray()
    last_error = "compact ACK was not received"
    scanned = 0
    scan_limit = ACK_RECORD.size * ACK_CONFIRM_ATTEMPTS * 3

    while time.monotonic() < deadline and scanned < scan_limit:
        marker = buffered.find(magic_bytes)
        if marker < 0:
            if len(buffered) > len(magic_bytes) - 1:
                del buffered[: -(len(magic_bytes) - 1)]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                byte = link.read_exact(1, min(remaining, 0.75), echo=False)
                buffered.extend(byte)
                captured.extend(byte)
                scanned += 1
            except (ProtocolError, OSError) as exc:
                last_error = str(exc)
            continue

        if marker:
            del buffered[:marker]
        while len(buffered) < ACK_RECORD.size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                byte = link.read_exact(1, min(remaining, 0.75), echo=False)
                buffered.extend(byte)
                captured.extend(byte)
                scanned += 1
            except ProtocolError as exc:
                last_error = str(exc)
                break
        if len(buffered) < ACK_RECORD.size:
            continue

        raw = bytes(buffered[: ACK_RECORD.size])
        del buffered[: ACK_RECORD.size]
        magic, observed_object, base, count, retry_bitmap, status, crc = ACK_RECORD.unpack(raw)
        if zlib.crc32(raw[:-4]) & 0xFFFFFFFF != crc:
            last_error = "compact ACK CRC mismatch"
            continue
        if magic != ACK_MAGIC or observed_object != object_id:
            last_error = "compact ACK object or magic mismatch"
            continue
        # The target does not advance the window until this one-byte confirmation
        # arrives. A corrupt or lost ACK is therefore safely retransmitted.
        link.write_all(ACK_CONFIRM_BYTE)
        return base, count, retry_bitmap, status
    if captured and hasattr(link, "prepend_buffer"):
        link.prepend_buffer(bytes(captured))
    raise ProtocolError(last_error)


def _guard_multi_frame_window(link: SerialLink, baud: int) -> None:
    if hasattr(link, "drain_output"):
        link.drain_output()
    # tcdrain() prevents the sleep from overlapping queued USB-serial output.
    # Three milliseconds is intentionally conservative relative to the target's
    # CRC/decode/copy time and the small 16550-compatible receive FIFO.
    time.sleep(MULTI_FRAME_GUARD_SECONDS)


def send_frames(
    link: SerialLink,
    frames: Sequence[EncodedFrame],
    object_id: int,
    window_size: int,
    *,
    retries: int = 3,
    ack_timeout: float = 8.0,
    baud: int = 115200,
    label: str = "Object upload",
    verbose_acks: bool = False,
) -> int:
    if not frames:
        raise ProtocolError("cannot send an empty frame set")
    wires = [frame.wire(object_id) for frame in frames]
    cumulative_sizes: list[int] = []
    total = 0
    for wire in wires:
        total += len(wire)
        cumulative_sizes.append(total)
    tracker = ProgressTracker(total, baud, label)
    print(
        f"[flasher] {label}: 0% complete — 0/{total} wire bytes — "
        f"{tracker._format_remaining(tracker.initial_seconds)}",
        flush=True,
    )
    base = 0
    retransmitted_frames = 0
    while base < len(frames):
        count = min(window_size, len(frames) - base)
        for index in range(base, base + count):
            link.write_all(wires[index], timeout=max(10.0, len(wires[index]) * 12 / max(baud, 1)))
            if count > 1 and index + 1 < base + count:
                _guard_multi_frame_window(link, baud)
        ack_base, ack_count, bitmap, status = _read_ack(link, object_id, ack_timeout)
        if ack_base != base or ack_count != count:
            raise ProtocolError(f"unexpected compact ACK window: base={ack_base} count={ack_count}")
        print(
            f"PMOS3 ACK object={object_id:02x} base={base:04x} count={count:02x} "
            f"retry={bitmap:08x} status={status:08x}", flush=True,
        )
        if status & 0xFFFF:
            retransmitted_frames += 1
            print(
                f"[flasher] Target reported UART receive status 0x{status & 0xFFFF:04x} "
                f"for frame window {base:#x}; qualification will reject this configuration.",
                flush=True,
            )
        attempt = 0
        while bitmap:
            attempt += 1
            if attempt > retries:
                raise ProtocolError(f"frame window {base:#x} exceeded retry limit; bitmap={bitmap:#x}")
            for bit in range(count):
                if bitmap & (1 << bit):
                    link.write_all(wires[base + bit])
                    retransmitted_frames += 1
            ack_base, ack_count, bitmap, status = _read_ack(link, object_id, ack_timeout)
            if ack_base != base or ack_count != count:
                raise ProtocolError("unexpected compact retry ACK window")
            if verbose_acks:
                print(
                    f"PMOS3 ACK-RETRY object={object_id:02x} base={base:04x} "
                    f"retry={bitmap:08x} attempt={attempt}", flush=True,
                )
        tracker.update(cumulative_sizes[base + count - 1])
        base += count
    return retransmitted_frames


def _nonce() -> int:
    return int.from_bytes(os.urandom(4), "little")


def _wait_fallback(link: SerialLink, controller: BaudController, old_rate: int,
                   nonce: int, timeout: float = 10.0) -> None:
    try:
        controller.set_rate(old_rate)
    except ProtocolError as exc:
        raise ProtocolError(f"could not restore host UART to {old_rate}: {exc}") from exc
    expected = f"PMOS3 BAUD-FALLBACK RATE={old_rate} NONCE={nonce:08x}"
    ready = f"PMOS3 BAUD-FALLBACK-READY RATE={old_rate} NONCE={nonce:08x}"
    link.wait_for((expected,), timeout)
    link.wait_for((ready,), timeout)
    link.discard_buffer()


def test_baud_candidate(link: SerialLink, controller: BaudController, requested: int,
                        current_rate: int, tested_actuals: set[int] | None = None) -> tuple[bool, int]:
    nonce = _nonce()
    link.write_all(f"PMOS3 BAUD-OFFER {requested} 0x{nonce:08x}\n".encode("ascii"))
    line = link.wait_for(("PMOS3 BAUD-CANDIDATE ",), 3.0)
    match = BAUD_CANDIDATE_RE.fullmatch(line)
    if not match or int(match.group(6), 16) != nonce:
        raise ProtocolError(f"invalid baud candidate response: {line}")
    actual = int(match.group(2))
    divisor = int(match.group(3))
    if tested_actuals is not None and actual in tested_actuals:
        return False, actual
    if actual <= current_rate or actual * 100 <= current_rate * 102:
        return False, actual
    if not controller.can_set(actual):
        print(f"[flasher] Host UART rejected {actual} baud; skipping.", flush=True)
        return False, actual
    link.write_all(
        f"PMOS3 BAUD-PREPARE {actual} {divisor} 0x{nonce:08x} "
        f"{BAUD_TEST_BYTES} {BAUD_TEST_PASSES}\n".encode("ascii")
    )
    prepared = link.wait_for(("PMOS3 BAUD-PREPARED ",), 3.0)
    prepared_match = BAUD_PREPARED_RE.fullmatch(prepared)
    if not prepared_match or int(prepared_match.group(3), 16) != nonce:
        raise ProtocolError(f"invalid baud prepare response: {prepared}")
    old_rate = int(prepared_match.group(4))
    try:
        controller.set_rate(actual)
        sync = link.wait_for(("PMOS3 BAUD-SYNC ",), 2.5)
        sync_match = BAUD_SYNC_RE.fullmatch(sync)
        if not sync_match or int(sync_match.group(1), 16) != nonce or int(sync_match.group(2)) != actual:
            raise ProtocolError(f"invalid baud synchronization response: {sync}")
        link.write_all(f"PMOS3 BAUD-SYNC-ACK 0x{nonce:08x}\n".encode("ascii"))
        link.wait_for((f"PMOS3 BAUD-START NONCE={nonce:08x}",), 2.0)
        for pass_index in range(BAUD_TEST_PASSES):
            seed = (0x37A42C19 ^ nonce ^ pass_index * 0x9E3779B9) & 0xFFFFFFFF
            data = xorshift_bytes(seed, BAUD_TEST_BYTES)
            crc = zlib.crc32(data) & 0xFFFFFFFF
            link.write_all(
                f"PMOS3 BAUD-H2T {pass_index} 0x{seed:08x} {len(data)} "
                f"0x{crc:08x} 0x{nonce:08x}\n".encode("ascii")
            )
            link.write_all(data, timeout=max(10.0, len(data) * 12 / actual))
            link.wait_for((f"PMOS3 BAUD-H2T-OK PASS={pass_index} ",), 4.0)
            t2h_line = link.wait_for((f"PMOS3 BAUD-T2H PASS={pass_index} ",), 4.0)
            t2h = BAUD_T2H_RE.fullmatch(t2h_line)
            if not t2h or int(t2h.group(5), 16) != nonce:
                raise ProtocolError(f"invalid target-to-host baud test header: {t2h_line}")
            t2h_seed = int(t2h.group(2), 16)
            length = int(t2h.group(3))
            expected_crc = int(t2h.group(4), 16)
            received = link.read_exact(length, max(6.0, length * 12 / actual + 2.0), echo=False)
            observed = zlib.crc32(received) & 0xFFFFFFFF
            if observed != expected_crc or received != xorshift_bytes(t2h_seed, length):
                raise ProtocolError("target-to-host deterministic baud test failed")
            link.write_all(
                f"PMOS3 BAUD-T2H-ACK {pass_index} 0x{observed:08x} 0x{nonce:08x}\n".encode("ascii")
            )
        link.wait_for((f"PMOS3 BAUD-PASS RATE={actual} NONCE={nonce:08x}",), 4.0)
        link.write_all(f"PMOS3 BAUD-COMMIT 0x{nonce:08x}\n".encode("ascii"))
        try:
            link.wait_for((f"PMOS3 BAUD-COMMITTED RATE={actual} NONCE={nonce:08x}",), 3.0)
        except ProtocolError:
            # A lost commit response must not force a blind rollback. Probe at the
            # candidate rate; a valid response proves that the target committed.
            probe = _nonce()
            link.write_all(f"PMOS3 BAUD-OFFER {actual} 0x{probe:08x}\n".encode("ascii"))
            line = link.wait_for(("PMOS3 BAUD-CANDIDATE ",), 2.0)
            match = BAUD_CANDIDATE_RE.fullmatch(line)
            if not match or int(match.group(6), 16) != probe:
                raise
        print(f"[flasher] Baud qualification passed bidirectionally at {actual} baud.", flush=True)
        return True, actual
    except (ProtocolError, OSError) as exc:
        print(f"[flasher] Baud qualification failed at {actual}: {exc}; reverting to {old_rate}.", flush=True)
        # Both sides independently return to the last known-good rate. The
        # target emits beacons there until command parsing is safe again.
        _wait_fallback(link, controller, old_rate, nonce)
        return False, actual


def negotiate_fastest_baud(link: SerialLink, controller: BaudController,
                            *, diagnostic_scan: bool = False,
                            refine_percent: float = 2.0) -> int:
    current = controller.current_rate
    failed: set[int] = set()
    tested: set[int] = set()
    candidates = DIAGNOSTIC_BAUD_CANDIDATES if diagnostic_scan else DEFAULT_BAUD_CANDIDATES

    for requested in candidates:
        if requested <= current:
            continue
        try:
            ok, actual = test_baud_candidate(link, controller, requested, current, tested)
        except ProtocolError as exc:
            print(f"[flasher] Baud candidate {requested} could not be tested: {exc}", flush=True)
            continue
        if actual in tested:
            continue
        tested.add(actual)
        if ok:
            current = actual
            if not diagnostic_scan:
                break
        elif actual > current:
            failed.add(actual)

    if diagnostic_scan:
        while True:
            higher_failures = sorted(rate for rate in failed if rate > current)
            if not higher_failures:
                break
            upper = higher_failures[0]
            if (upper - current) * 100.0 / current <= refine_percent:
                break
            requested = (current + upper) // 2
            try:
                ok, actual = test_baud_candidate(link, controller, requested, current, tested)
            except ProtocolError:
                break
            if actual in tested or actual <= current:
                break
            tested.add(actual)
            if ok:
                current = actual
            else:
                failed.add(actual)

    policy = "diagnostic scan" if diagnostic_scan else "conservative one-pass scan"
    print(f"[flasher] Selected UART rate: {current} baud ({policy}).", flush=True)
    return current


def _synchronize_failed_feature(link: SerialLink, mode: int, timeout: float = 8.0) -> bool:
    """Wait until the target has left ACK-confirmation state before another command."""
    if not hasattr(link, "read_line"):
        return False
    deadline = time.monotonic() + timeout
    terminal = f"PMOS3 FEATURE-FAIL MODE={mode}"
    while time.monotonic() < deadline:
        try:
            line = link.read_line(deadline - time.monotonic())
        except ProtocolError:
            break
        if terminal in line or f"PMOS3 FEATURE-PASS MODE={mode}" in line:
            if hasattr(link, "discard_buffer"):
                link.discard_buffer()
            return True
        # Binary ACK retries may decode as garbage lines. Keep consuming until
        # the target reports its terminal feature result.
    return False


def feature_test(link: SerialLink, mode: int, frame_size: int, window_size: int,
                 baud: int, *, verbose_acks: bool = False) -> bool:
    data = make_test_object()
    plan = make_plan(data, frame_size, mode)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    link.write_all(
        f"PMOS3 FEATURE {mode} {frame_size} {window_size} {len(plan.frames)} "
        f"{len(data)} 0x{crc:08x}\n".encode("ascii")
    )
    try:
        link.wait_for(("PMOS3 FEATURE-READY ",), 3.0)
        retries = send_frames(
            link, plan.frames, OBJECT_TEST, window_size,
            baud=baud, label=f"{plan.name} transport qualification",
            verbose_acks=verbose_acks,
        )
        link.wait_for(
            (f"PMOS3 FEATURE-PASS MODE={mode}",), 5.0,
            error_prefixes=(f"PMOS3 FEATURE-FAIL MODE={mode}",),
        )
        if retries:
            print(
                f"[flasher] {REP_NAMES[mode]} qualification needed {retries} frame retries; "
                "rejecting this configuration for production use.",
                flush=True,
            )
            return False
        return True
    except ProtocolError as exc:
        message = str(exc)
        explicit_failure = message.startswith(f"PMOS3 FEATURE-FAIL MODE={mode}")
        synchronized = explicit_failure
        if not explicit_failure:
            synchronized = _synchronize_failed_feature(link, mode)
        print(f"[flasher] {REP_NAMES[mode]} transport qualification failed: {message}", flush=True)
        if "compact ACK" in message or "ACK-CONFIRM-TIMEOUT" in message:
            if not synchronized:
                raise ProtocolError(
                    f"transport stream did not resynchronize after {message}"
                ) from exc
            if mode == REP_RAW:
                raise ProtocolError(
                    f"fundamental raw transport ACK qualification failed: {message}"
                ) from exc
        return False


def qualify_transport(link: SerialLink, baud: int, *, verbose_acks: bool = False,
                      diagnostic_window_scan: bool = False) -> TransportSelection:
    frame_size = 4096
    window = 0
    frame_sizes = (4096, 1024)
    for candidate_frame_size in frame_sizes:
        frame_size = candidate_frame_size
        candidates = (
            DIAGNOSTIC_WINDOW_CANDIDATES
            if diagnostic_window_scan else (DEFAULT_WINDOW_SIZE,)
        )
        for candidate in candidates:
            if feature_test(link, REP_RAW, frame_size, candidate, baud, verbose_acks=verbose_acks):
                window = candidate
                continue
            # Window 1 is the required lossless baseline. Larger diagnostic
            # windows stop at the first failure and retain the last safe value.
            if candidate == DEFAULT_WINDOW_SIZE:
                window = 0
            break
        if window:
            break
    if window == 0:
        raise ProtocolError("no frame/window transport configuration passed qualification")
    sparse_ok = feature_test(link, REP_SPARSE, frame_size, window, baud, verbose_acks=verbose_acks)
    lz4_ok = feature_test(link, REP_LZ4, frame_size, window, baud, verbose_acks=verbose_acks)
    combined_ok = False
    if sparse_ok and lz4_ok:
        combined_ok = feature_test(link, REP_SPARSE_LZ4, frame_size, window, baud, verbose_acks=verbose_acks)
        if not combined_ok:
            # Individual modes remain safe and selectable.
            print("[flasher] Combined sparse-LZ4 test failed; individual sparse/LZ4 modes remain enabled.", flush=True)
    policy = "diagnostic ascending scan" if diagnostic_window_scan else "flow-control-safe production window"
    print(
        f"[flasher] Qualified transport: frame={frame_size} bytes, window={window} ({policy}), "
        f"sparse={'yes' if sparse_ok else 'no'}, lz4={'yes' if lz4_ok else 'no'}, "
        f"sparse-lz4={'yes' if combined_ok else 'no'}",
        flush=True,
    )
    return TransportSelection(baud, frame_size, window, sparse_ok, lz4_ok, combined_ok)


def choose_representation(image: Path, selection: TransportSelection) -> RepresentationPlan:
    data = image.read_bytes()
    if len(data) != FULL_IMAGE_SIZE:
        raise ProtocolError("PMOSREC v3 requires an exact 16 MiB full image")
    modes = [REP_RAW]
    if selection.sparse_ok:
        modes.append(REP_SPARSE)
    if selection.lz4_ok:
        modes.append(REP_LZ4)
    if selection.sparse_lz4_ok:
        modes.append(REP_SPARSE_LZ4)
    plans = [make_plan(data, selection.frame_size, mode) for mode in modes]
    for plan in plans:
        print(
            f"[flasher] {plan.name:10s}: {plan.wire_bytes} wire bytes in {len(plan.frames)} frames",
            flush=True,
        )
    selected = min(plans, key=lambda plan: plan.wire_bytes)
    print(f"[flasher] Selected firmware representation: {selected.name}", flush=True)
    return selected


def make_manifest_plan(manifest: bytes, frame_size: int) -> RepresentationPlan:
    return make_plan(manifest, frame_size, REP_RAW)


def make_package_header(bundle: BundleInfo, image_plan: RepresentationPlan,
                        manifest_plan: RepresentationPlan, window_size: int,
                        *, dry_run: bool, force: bool) -> bytes:
    flags = 1 | (2 if dry_run else 0) | (4 if force else 0)
    model = bundle.model.encode("ascii")
    if len(model) > 15:
        raise ProtocolError("target model does not fit the PMOSREC v3 header")
    family_id = 1 if bundle.family == "luton26" else 2
    fields = (
        PACKAGE_MAGIC,
        PROTOCOL_VERSION, flags, family_id, FULL_IMAGE_SIZE, len(bundle.manifest_bytes),
        image_plan.frame_size, window_size, image_plan.mode, len(image_plan.frames),
        len(manifest_plan.frames), image_plan.wire_bytes, bundle.manifest_crc32,
        bundle.image_crc32, bundle.image_sha256, bundle.manifest_sha256,
        model.ljust(16, b"\0"), 0,
    )
    raw = PACKAGE_HEADER.pack(*fields)
    return raw[:-4] + struct.pack("<I", zlib.crc32(raw[:-4]) & 0xFFFFFFFF)



FLASH_PROGRESS_RE = re.compile(r"^PMOSREC PROGRESS (ERASE|PROGRAM|VERIFY) ([0-9a-fA-F]{8})$")
FLASH_BEGIN_RE = re.compile(r"^PMOSREC PROGRESS (ERASE|PROGRAM|VERIFY)-BEGIN$")
FLASH_END_RE = re.compile(r"^PMOSREC PROGRESS (ERASE|PROGRAM|VERIFY)-END$")


def _format_eta_seconds(seconds: float) -> str:
    seconds = max(0.0, seconds)
    if seconds < 120.0:
        return f"~{math.ceil(seconds)} seconds remaining"
    return f"~{math.ceil(seconds / 60.0)} minutes remaining"


def wait_for_flash_success(link: SerialLink, operation_timeout: float) -> str:
    """Render target erase/program/readback progress without adding target UART traffic."""
    deadline = time.monotonic() + operation_timeout
    phase_start: dict[str, float] = {}
    labels = {"ERASE": "Erasing SPI NOR", "PROGRAM": "Programming SPI NOR",
              "VERIFY": "Readback verification"}
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ProtocolError("timed out waiting for SPI NOR flash completion")
        line = link.read_line(remaining)
        if line.startswith("PMOSREC RESULT ERROR"):
            raise ProtocolError(line)
        if line == "PMOSREC RESULT SUCCESS":
            return line
        begin = FLASH_BEGIN_RE.fullmatch(line)
        if begin:
            phase = begin.group(1)
            phase_start[phase] = time.monotonic()
            print(f"[flasher] {labels[phase]}: started", flush=True)
            continue
        progress = FLASH_PROGRESS_RE.fullmatch(line)
        if progress:
            phase = progress.group(1)
            address = int(progress.group(2), 16)
            percent = min(99, int(address * 100 / FULL_IMAGE_SIZE))
            started = phase_start.setdefault(phase, time.monotonic())
            elapsed = max(0.001, time.monotonic() - started)
            eta_text = "estimating remaining time"
            if address > 0:
                rate = address / elapsed
                eta_text = _format_eta_seconds((FULL_IMAGE_SIZE - address) / max(rate, 1.0))
            print(f"[flasher] {labels[phase]}: {percent}% complete — {eta_text}", flush=True)
            continue
        ended = FLASH_END_RE.fullmatch(line)
        if ended:
            phase = ended.group(1)
            print(f"[flasher] {labels[phase]}: 100% complete", flush=True)

def send_package_v3(link: SerialLink, bundle: BundleInfo, selection: TransportSelection,
                    *, dry_run: bool, force: bool, auto_confirm: bool,
                    verbose_acks: bool = False,
                    manual_input: Callable[[str], str] = input,
                    operation_timeout: float = 1800.0,
                    baud_controller: BaudController | None = None) -> str:
    image_plan = choose_representation(bundle.image, selection)
    manifest_plan = make_manifest_plan(bundle.manifest_bytes, selection.frame_size)
    header = make_package_header(
        bundle, image_plan, manifest_plan, selection.window_size,
        dry_run=dry_run, force=force,
    )
    link.write_all(b"PMOS3 PACKAGE\n")
    link.wait_for(("PMOS3 PACKAGE-READY",), 3.0)
    link.write_all(header)
    link.wait_for(("PMOS3 PACKAGE-HEADER-ACK ",), 5.0)

    # Manifest first, so parser/model/layout failures occur before the large image transfer.
    send_frames(
        link, manifest_plan.frames, OBJECT_MANIFEST, selection.window_size,
        baud=selection.baud, label="Manifest upload", verbose_acks=verbose_acks,
    )
    link.wait_for(("PMOS3 MANIFEST-OBJECT-VERIFIED",), 10.0)
    link.wait_for(("PMOS3 MANIFEST-ACCEPTED",), 10.0)

    send_frames(
        link, image_plan.frames, OBJECT_IMAGE, selection.window_size,
        baud=selection.baud, label="Firmware upload", verbose_acks=verbose_acks,
    )
    link.wait_for(("PMOS3 IMAGE-OBJECT-VERIFIED",), 30.0)
    link.wait_for(("PMOSPKG VERIFIED ",), 15.0)
    if dry_run:
        return link.wait_for(("PMOSREC RESULT DRY-RUN-OK",), 15.0)

    challenge_line = link.wait_for(("PMOSREC ERASE-CHALLENGE ",), 15.0)
    match = re.fullmatch(r"PMOSREC ERASE-CHALLENGE ([0-9a-fA-F]{8})", challenge_line)
    if not match:
        raise ProtocolError(f"invalid erase challenge: {challenge_line}")
    nonce = match.group(1).lower()
    link.wait_for(("PMOSREC CONFIRMATION-WAIT ",), 5.0)
    expected = f"ERASEFLASH {nonce}"
    if auto_confirm:
        print(
            f"[flasher] Target validated the complete bundle; automatically sending {expected} "
            "under the prior FLASH-ALL authorization.", flush=True,
        )
        link.write_all((expected + "\n").encode("ascii"))
        link.wait_for(("PMOSREC CONFIRMATION-ACK",), 5.0)
    else:
        while True:
            entered = manual_input(
                f"Enter the complete command and challenge ({expected}), or power-cycle to cancel: "
            ).strip()
            link.write_all((entered + "\n").encode("ascii"))
            response = link.wait_for(
                ("PMOSREC CONFIRMATION-ACK", "PMOSREC CONFIRMATION-REJECTED "), 10.0
            )
            if response == "PMOSREC CONFIRMATION-ACK":
                break
            print(f"[flasher] Incorrect confirmation. Required: {expected}. Retry is unlimited.", flush=True)

    result = wait_for_flash_success(link, operation_timeout)
    # Target owns the countdown and reset so reboot still occurs after host disconnect.
    for remaining in range(5, 0, -1):
        link.wait_for((f"PMOSREC REBOOT {remaining}",), 3.0)
    link.wait_for(("PMOSREC REBOOT NOW",), 3.0)

    # PMOSREC emits REBOOT NOW at the negotiated transport rate, then resets.
    # The permanent loader starts at 115200, so change the host TTY immediately
    # after consuming the final high-speed line. Clear only bytes buffered in
    # user space; BaudController.set_rate() flushes the kernel TTY queues.
    previous_rate = selection.baud
    monitor_baud = previous_rate
    boot_baud_ready = previous_rate == BOOT_BAUD
    if baud_controller is not None:
        previous_rate = baud_controller.current_rate
        monitor_baud = previous_rate
        try:
            baud_controller.set_rate(BOOT_BAUD, flush=True)
            buffer = getattr(link, "buffer", None)
            if isinstance(buffer, bytearray):
                buffer.clear()
            monitor_baud = BOOT_BAUD
            boot_baud_ready = True
            print(
                f"[flasher] Reboot handoff detected; UART switched from "
                f"{previous_rate} to {BOOT_BAUD} baud for boot monitoring.",
                flush=True,
            )
        except ProtocolError as exc:
            print(
                f"[flasher] WARNING: reboot was requested, but the host UART could not "
                f"return to {BOOT_BAUD} baud: {exc}",
                flush=True,
            )

    print("[flasher] Flash verified; target reset requested. Waiting for the next boot banner...", flush=True)
    try:
        boot = link.wait_for(BOOT_BANNER_PREFIXES, 60.0)
        print(f"[flasher] Reboot detected at {monitor_baud} baud: {boot}", flush=True)
    except ProtocolError:
        suffix = "" if boot_baud_ready else " (boot-baud handoff was unavailable)"
        print(
            f"[flasher] Reset countdown completed; no new boot banner was observed "
            f"within 60 seconds{suffix}.",
            flush=True,
        )
    return result
