from __future__ import annotations

from contextlib import redirect_stdout
import hashlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib

import pmosrec_v3 as p3
from bootloader_protocol import ProtocolError


class FakeBinaryLink:
    def __init__(self, reads: list[bytes]) -> None:
        self.data = bytearray().join(reads)
        self.writes: list[bytes] = []
        self.buffer = bytearray()
        self.drains = 0

    def read_exact(self, length: int, timeout: float, *, echo: bool = False) -> bytes:
        if len(self.data) < length:
            raise ProtocolError("timed out reading binary bytes")
        output = bytes(self.data[:length])
        del self.data[:length]
        return output

    def write_all(self, data: bytes, timeout: float = 10.0) -> None:
        self.writes.append(bytes(data))

    def prepend_buffer(self, data: bytes) -> None:
        self.buffer[:0] = data

    def drain_output(self) -> None:
        self.drains += 1


class FakeLineLink:
    def __init__(self, lines: list[str]) -> None:
        self.lines = list(lines)

    def read_line(self, timeout: float) -> str:
        if not self.lines:
            raise ProtocolError("timed out")
        return self.lines.pop(0)




class FakeProtocolLink:
    def __init__(self, lines: list[str]) -> None:
        self.lines = list(lines)
        self.writes: list[bytes] = []
        self.buffer = bytearray()

    def write_all(self, data: bytes, timeout: float = 10.0) -> None:
        self.writes.append(bytes(data))

    def wait_for(self, prefixes: tuple[str, ...], timeout: float, **_kwargs) -> str:
        if not self.lines:
            raise ProtocolError(f"timed out waiting for: {prefixes}")
        line = self.lines.pop(0)
        if not line.startswith(prefixes):
            raise AssertionError((line, prefixes))
        return line


class PMOSRECv3Tests(unittest.TestCase):
    def test_lz4_roundtrips_compressible_random_and_short_blocks(self) -> None:
        samples = [
            b"A" * 4096,
            p3.xorshift_bytes(0x12345678, 4096),
            b"postmerkOS" * 13,
            bytes(range(256)) * 16,
        ]
        for sample in samples:
            encoded = p3.lz4_compress_block(sample)
            self.assertEqual(p3.lz4_decompress_block(encoded, len(sample)), sample)

    def test_lz4_rejects_corrupt_and_truncated_blocks(self) -> None:
        with self.assertRaises(ProtocolError):
            p3.lz4_decompress_block(b"\x10", 4096)
        with self.assertRaises(ProtocolError):
            p3.lz4_decompress_block(b"\x00\x00\x00", 4096)

    def test_sparse_plan_omits_ff_frames_and_reconstructs_exactly(self) -> None:
        data = b"\xff" * 4096 + b"A" * 4096 + b"\xff" * 4096 + b"B" * 123
        plan = p3.make_plan(data, 4096, p3.REP_SPARSE)
        self.assertEqual([frame.offset for frame in plan.frames], [4096, 12288])
        output = bytearray(b"\xff" * len(data))
        for frame in plan.frames:
            decoded = (
                p3.lz4_decompress_block(frame.payload, 4096)
                if frame.flags & p3.FRAME_FLAG_LZ4 else frame.payload
            )
            output[frame.offset : frame.offset + frame.decoded_length] = decoded
        self.assertEqual(bytes(output), data)

    def test_sparse_lz4_is_only_selected_when_combined_mode_qualified(self) -> None:
        selection = p3.TransportSelection(460800, 4096, 8, True, True, False)
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "image.bin"
            image.write_bytes((b"A" * 4096 + b"\xff" * 4096) * 2048)
            chosen = p3.choose_representation(image, selection)
        self.assertNotEqual(chosen.mode, p3.REP_SPARSE_LZ4)

    def test_frame_header_has_independent_header_wire_and_decoded_crcs(self) -> None:
        decoded = b"frame-data" * 100
        frame = p3.EncodedFrame(7, 0x4000, len(decoded), 0, decoded, zlib.crc32(decoded) & 0xFFFFFFFF)
        wire = frame.wire(p3.OBJECT_IMAGE)
        fields = p3.FRAME_HEADER.unpack(wire[: p3.FRAME_HEADER.size])
        self.assertEqual(fields[0], p3.FRAME_MAGIC)
        self.assertEqual(fields[1], p3.OBJECT_IMAGE)
        self.assertEqual(fields[2], 7)
        self.assertEqual(fields[7], zlib.crc32(decoded) & 0xFFFFFFFF)
        self.assertEqual(fields[8], zlib.crc32(decoded) & 0xFFFFFFFF)
        self.assertEqual(fields[9], zlib.crc32(wire[: p3.FRAME_HEADER.size - 4]) & 0xFFFFFFFF)

    def test_compact_ack_is_crc_checked_resynchronized_and_confirmed(self) -> None:
        body = struct.pack("<6I", p3.ACK_MAGIC, p3.OBJECT_IMAGE, 10, 4, 0, 0)
        good = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        # Reproduce the former target bug: every binary 0x0a was expanded to
        # CRLF, lengthening and misaligning the first ACK. The retransmitted
        # unmodified record must still be found by its magic and accepted.
        newline_expanded = good.replace(b"\x0a", b"\r\n")
        link = FakeBinaryLink([b"junk", newline_expanded, good])
        self.assertEqual(p3._read_ack(link, p3.OBJECT_IMAGE, 10.0), (10, 4, 0, 0))
        self.assertEqual(link.writes, [p3.ACK_CONFIRM_BYTE])

    def test_compact_ack_recovers_after_a_missing_byte(self) -> None:
        body = struct.pack("<6I", p3.ACK_MAGIC, p3.OBJECT_IMAGE, 8, 4, 0, 0)
        good = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        damaged = good[:11] + good[12:]
        link = FakeBinaryLink([damaged, good, good])
        self.assertEqual(p3._read_ack(link, p3.OBJECT_IMAGE, 10.0), (8, 4, 0, 0))


    def test_failed_ack_preserves_terminal_feature_status_for_resynchronization(self) -> None:
        terminal = b"PMOS3 FEATURE-FAIL MODE=0 FRAME-ERROR=3 EXPECTED-SEQ=1 UARTERR=00000002 DRAINED=4096\r\n"
        link = FakeBinaryLink([terminal])
        with self.assertRaises(ProtocolError):
            p3._read_ack(link, p3.OBJECT_TEST, 0.01)
        self.assertIn(b"PMOS3 FEATURE-FAIL MODE=0", link.buffer)

    def test_feature_resynchronizer_accepts_terminal_status_after_binary_noise(self) -> None:
        link = FakeLineLink(["\ufffd\ufffdPMOS3 FEATURE-FAIL MODE=0 FRAME-ERROR=3 DRAINED=4096"])
        self.assertTrue(p3._synchronize_failed_feature(link, 0, timeout=0.1))

    def test_default_transport_qualifies_only_flow_control_safe_window_one(self) -> None:
        calls: list[tuple[int, int]] = []

        def feature(_link, mode, frame_size, window_size, _baud, **_kwargs):
            calls.append((mode, window_size))
            return True

        with mock.patch.object(p3, "feature_test", side_effect=feature):
            selected = p3.qualify_transport(object(), 929828)
        self.assertEqual(selected.window_size, 1)
        self.assertEqual(calls[0], (p3.REP_RAW, 1))
        self.assertFalse(any(mode == p3.REP_RAW and window > 1 for mode, window in calls))

    def test_diagnostic_window_scan_is_ascending_and_keeps_last_safe_window(self) -> None:
        raw_windows: list[int] = []

        def feature(_link, mode, frame_size, window_size, _baud, **_kwargs):
            if mode == p3.REP_RAW:
                raw_windows.append(window_size)
                return window_size <= 4
            return True

        with mock.patch.object(p3, "feature_test", side_effect=feature):
            selected = p3.qualify_transport(object(), 929828, diagnostic_window_scan=True)
        self.assertEqual(raw_windows, [1, 2, 4, 8])
        self.assertEqual(selected.window_size, 4)

    def test_multi_frame_window_is_drained_and_guarded_between_frames(self) -> None:
        frames = (
            p3.EncodedFrame(0, 0, 4, 0, b"aaaa", zlib.crc32(b"aaaa") & 0xFFFFFFFF),
            p3.EncodedFrame(1, 4, 4, 0, b"bbbb", zlib.crc32(b"bbbb") & 0xFFFFFFFF),
        )
        link = FakeBinaryLink([])
        with mock.patch.object(p3, "_read_ack", return_value=(0, 2, 0, 0)), \
             mock.patch.object(p3.time, "sleep") as sleep:
            p3.send_frames(link, frames, p3.OBJECT_TEST, 2, baud=929828)
        self.assertEqual(link.drains, 1)
        sleep.assert_called_once_with(p3.MULTI_FRAME_GUARD_SECONDS)

    def test_package_header_is_v3_manifest_first_and_crc_bound(self) -> None:
        class Bundle:
            model = "MS42P"
            family = "jaguar1"
            manifest_bytes = b"{}"
            manifest_crc32 = zlib.crc32(manifest_bytes) & 0xFFFFFFFF
            image_crc32 = 0x12345678
            image_sha256 = hashlib.sha256(b"image").digest()
            manifest_sha256 = hashlib.sha256(manifest_bytes).digest()

        image_plan = p3.RepresentationPlan(p3.REP_RAW, 4096, (
            p3.EncodedFrame(0, 0, 1, 0, b"x", zlib.crc32(b"x") & 0xFFFFFFFF),
        ))
        manifest_plan = p3.make_manifest_plan(Bundle.manifest_bytes, 4096)
        raw = p3.make_package_header(Bundle, image_plan, manifest_plan, 8, dry_run=False, force=True)
        self.assertEqual(len(raw), 144)
        fields = p3.PACKAGE_HEADER.unpack(raw)
        self.assertEqual(fields[0], p3.PACKAGE_MAGIC)
        self.assertEqual(fields[1], 3)
        self.assertEqual(fields[6], 4096)
        self.assertEqual(fields[7], 8)
        self.assertEqual(fields[-1], zlib.crc32(raw[:-4]) & 0xFFFFFFFF)

    def test_flash_phase_progress_and_success_are_rendered(self) -> None:
        link = FakeLineLink([
            "PMOSREC PROGRESS ERASE-BEGIN",
            "PMOSREC PROGRESS ERASE 00100000",
            "PMOSREC PROGRESS ERASE-END",
            "PMOSREC PROGRESS PROGRAM-BEGIN",
            "PMOSREC PROGRESS PROGRAM 00800000",
            "PMOSREC PROGRESS PROGRAM-END",
            "PMOSREC PROGRESS VERIFY-BEGIN",
            "PMOSREC PROGRESS VERIFY 00f00000",
            "PMOSREC PROGRESS VERIFY-END",
            "PMOSREC RESULT SUCCESS",
        ])
        output = io.StringIO()
        with redirect_stdout(output):
            result = p3.wait_for_flash_success(link, 30.0)
        self.assertEqual(result, "PMOSREC RESULT SUCCESS")
        text = output.getvalue()
        self.assertIn("Erasing SPI NOR", text)
        self.assertIn("Programming SPI NOR", text)
        self.assertIn("Readback verification", text)
        self.assertIn("100% complete", text)

    def test_flash_phase_error_is_not_hidden(self) -> None:
        link = FakeLineLink(["PMOSREC RESULT ERROR PROGRAM-TIMEOUT AT=00040000"])
        with self.assertRaisesRegex(ProtocolError, "PROGRAM-TIMEOUT"):
            p3.wait_for_flash_success(link, 1.0)

    def test_default_baud_scan_is_descending_single_pass_and_stops_on_success(self) -> None:
        class Controller:
            current_rate = 115200

        results = {921600: (False, 929828), 460800: (True, 464914), 230400: (True, 228378)}

        def candidate(_link, _controller, requested, current, tested):
            return results[requested]

        with mock.patch.object(p3, "test_baud_candidate", side_effect=candidate) as called:
            selected = p3.negotiate_fastest_baud(object(), Controller())
        self.assertEqual(selected, 464914)
        self.assertEqual([call.args[2] for call in called.call_args_list], [921600, 460800])

    def test_diagnostic_baud_scan_retains_midpoint_refinement(self) -> None:
        class Controller:
            current_rate = 115200

        results = {153600: (True, 153600), 230400: (True, 230400), 460800: (True, 460800), 500000: (False, 500000)}

        def candidate(_link, _controller, requested, current, tested):
            return results.get(requested, (False, requested))

        with mock.patch.object(p3, "DIAGNOSTIC_BAUD_CANDIDATES", tuple(results)), \
             mock.patch.object(p3, "test_baud_candidate", side_effect=candidate) as called:
            selected = p3.negotiate_fastest_baud(
                object(), Controller(), diagnostic_scan=True, refine_percent=2.0
            )
        self.assertEqual(selected, 460800)
        self.assertTrue(any(call.args[2] == 480400 for call in called.call_args_list))

    def _package_fixture(self):
        class Bundle:
            model = "MS42P"
            family = "jaguar1"
            manifest_bytes = b"{}"
            manifest_crc32 = zlib.crc32(manifest_bytes) & 0xFFFFFFFF
            image_crc32 = 0x12345678
            image_sha256 = hashlib.sha256(b"image").digest()
            manifest_sha256 = hashlib.sha256(manifest_bytes).digest()
            image = Path("unused.bin")

        plan = p3.RepresentationPlan(p3.REP_RAW, 4096, (
            p3.EncodedFrame(0, 0, 1, 0, b"x", zlib.crc32(b"x") & 0xFFFFFFFF),
        ))
        selection = p3.TransportSelection(460800, 4096, 8, True, True, True, plan)
        return Bundle, plan, selection

    def test_auto_confirmation_sends_the_complete_live_challenge(self) -> None:
        Bundle, plan, selection = self._package_fixture()
        lines = [
            "PMOS3 PACKAGE-READY",
            "PMOS3 PACKAGE-HEADER-ACK MODEL=MS42P",
            "PMOS3 MANIFEST-OBJECT-VERIFIED",
            "PMOS3 MANIFEST-ACCEPTED",
            "PMOS3 IMAGE-OBJECT-VERIFIED",
            "PMOSPKG VERIFIED MODEL=MS42P",
            "PMOSREC ERASE-CHALLENGE 12620a82",
            "PMOSREC CONFIRMATION-WAIT ENTER=ERASEFLASH+12620a82",
            "PMOSREC CONFIRMATION-ACK",
            *(f"PMOSREC REBOOT {value}" for value in range(5, 0, -1)),
            "PMOSREC REBOOT NOW",
            "LinuxLoader built test",
        ]
        link = FakeProtocolLink(lines)
        with mock.patch.object(p3, "choose_representation", return_value=plan), \
             mock.patch.object(p3, "make_manifest_plan", return_value=plan), \
             mock.patch.object(p3, "send_frames", return_value=0), \
             mock.patch.object(p3, "wait_for_flash_success", return_value="PMOSREC RESULT SUCCESS"):
            result = p3.send_package_v3(
                link, Bundle, selection, dry_run=False, force=True, auto_confirm=True
            )
        self.assertEqual(result, "PMOSREC RESULT SUCCESS")
        self.assertIn(b"ERASEFLASH 12620a82\n", link.writes)


    def test_reboot_handoff_returns_host_uart_to_loader_baud(self) -> None:
        Bundle, plan, selection = self._package_fixture()
        selection = p3.TransportSelection(929828, 4096, 1, True, True, True, plan)
        lines = [
            "PMOS3 PACKAGE-READY",
            "PMOS3 PACKAGE-HEADER-ACK MODEL=MS42P",
            "PMOS3 MANIFEST-OBJECT-VERIFIED",
            "PMOS3 MANIFEST-ACCEPTED",
            "PMOS3 IMAGE-OBJECT-VERIFIED",
            "PMOSPKG VERIFIED MODEL=MS42P",
            "PMOSREC ERASE-CHALLENGE 12620a82",
            "PMOSREC CONFIRMATION-WAIT ENTER=ERASEFLASH+12620a82",
            "PMOSREC CONFIRMATION-ACK",
            *(f"PMOSREC REBOOT {value}" for value in range(5, 0, -1)),
            "PMOSREC REBOOT NOW",
            "init_pll ok",
        ]
        link = FakeProtocolLink(lines)
        link.buffer.extend(b"stale-high-speed-data")

        class Controller:
            current_rate = 929828

            def __init__(self) -> None:
                self.calls: list[tuple[int, bool]] = []

            def set_rate(self, rate: int, *, flush: bool = True) -> None:
                self.calls.append((rate, flush))
                self.current_rate = rate

        controller = Controller()
        output = io.StringIO()
        with redirect_stdout(output), \
             mock.patch.object(p3, "choose_representation", return_value=plan), \
             mock.patch.object(p3, "make_manifest_plan", return_value=plan), \
             mock.patch.object(p3, "send_frames", return_value=0), \
             mock.patch.object(p3, "wait_for_flash_success", return_value="PMOSREC RESULT SUCCESS"):
            result = p3.send_package_v3(
                link, Bundle, selection, dry_run=False, force=True, auto_confirm=True,
                baud_controller=controller,
            )
        self.assertEqual(result, "PMOSREC RESULT SUCCESS")
        self.assertEqual(controller.calls, [(p3.BOOT_BAUD, True)])
        self.assertEqual(link.buffer, bytearray())
        self.assertIn("Reboot handoff detected", output.getvalue())
        self.assertIn("Reboot detected at 115200 baud: init_pll ok", output.getvalue())

    def test_manual_confirmation_retries_without_cancelling(self) -> None:
        Bundle, plan, selection = self._package_fixture()
        lines = [
            "PMOS3 PACKAGE-READY",
            "PMOS3 PACKAGE-HEADER-ACK MODEL=MS42P",
            "PMOS3 MANIFEST-OBJECT-VERIFIED",
            "PMOS3 MANIFEST-ACCEPTED",
            "PMOS3 IMAGE-OBJECT-VERIFIED",
            "PMOSPKG VERIFIED MODEL=MS42P",
            "PMOSREC ERASE-CHALLENGE deadbeef",
            "PMOSREC CONFIRMATION-WAIT ENTER=ERASEFLASH+deadbeef",
            "PMOSREC CONFIRMATION-REJECTED EXPECTED=ERASEFLASH deadbeef",
            "PMOSREC CONFIRMATION-ACK",
            *(f"PMOSREC REBOOT {value}" for value in range(5, 0, -1)),
            "PMOSREC REBOOT NOW",
            "LinuxLoader built test",
        ]
        link = FakeProtocolLink(lines)
        entries = iter(("ERASEFLASH", "ERASEFLASH deadbeef"))
        with mock.patch.object(p3, "choose_representation", return_value=plan), \
             mock.patch.object(p3, "make_manifest_plan", return_value=plan), \
             mock.patch.object(p3, "send_frames", return_value=0), \
             mock.patch.object(p3, "wait_for_flash_success", return_value="PMOSREC RESULT SUCCESS"):
            p3.send_package_v3(
                link, Bundle, selection, dry_run=False, force=False, auto_confirm=False,
                manual_input=lambda _prompt: next(entries),
            )
        self.assertIn(b"ERASEFLASH\n", link.writes)
        self.assertIn(b"ERASEFLASH deadbeef\n", link.writes)

    def test_initial_eta_scales_with_negotiated_baud(self) -> None:
        slow = p3.ProgressTracker(1024 * 1024, 115200, "slow")
        fast = p3.ProgressTracker(1024 * 1024, 460800, "fast")
        self.assertAlmostEqual(slow.initial_seconds / fast.initial_seconds, 4.0, places=6)


    def test_source_contract_has_sync_commit_and_independent_fallback(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "pmosrec_v3.py").read_text()
        for token in (
            "BAUD-SYNC-ACK", "BAUD-COMMIT", "BAUD-COMMITTED",
            "BAUD-FALLBACK-READY", "refine_percent", "DEFAULT_BAUD_CANDIDATES",
            "DIAGNOSTIC_BAUD_CANDIDATES", "diagnostic_scan",
            "termios2", "sparse-lz4", "ACK_CONFIRM_BYTE", "BOOT_BAUD",
        ):
            self.assertIn(token, source)


if __name__ == "__main__":
    unittest.main()
