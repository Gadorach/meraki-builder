from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock

TOOL_DIR = Path(__file__).resolve().parents[1]


def load_module(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, TOOL_DIR / filename)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


bl = load_module("test_bootloader_liveboot", "bootloader-liveboot.py")
pv3 = load_module("test_pmosrec_v3_live", "pmosrec_v3.py")


class LivebootConsoleTests(unittest.TestCase):
    def ready_sequence(self) -> list[str]:
        return [
            "PMOSLIVE READY 3 SOC=jaguar1 FAMILY=00000002 FLASH=0",
            (
                "PMOSLIVE DESCRIPTOR PMOSLIVE3;SOC=jaguar1;FAMILY=2;PROTO=3;FLASH=0;LIVEBOOT=1;"
                "IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
                "FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END"
            ),
            "PMOSLIVE RAM-MAP KERNEL=81000000 IMAGE=81400000 MANIFEST=82400000 PAYLOAD=86c00000 ROOTFS=87000000",
            "PMOSLIVE UART-CAP BAUDTEST=1 FRAME_MAX=4096 WINDOW_MAX=16",
            "PMOSLIVE COMMAND-READY 3",
        ]

    def test_embedded_menu_sequence_selects_option_three(self) -> None:
        link = mock.Mock()
        link.wait_for.side_effect = [
            "PMOSBOOT MENU-PROBE TIMEOUT_MS=00000bb8",
            "PMOSBOOT PASS-MENU-TRIGGER: BYTE: 0x0000000D",
            "PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT",
            "PMOSBOOT MENU-READY TIMEOUT_MS=00001388",
            "PMOSBOOT PASS-MENU-CHOICE: SELECTED: 0x00000003",
            "PMOSBOOT INFO-LIVEBOOT: SOURCE: MENU-OPTION-3 | SOC: jaguar1",
            "PMOSBOOT PASS-LIVEBOOT-COPY",
            "PMOSBOOT PASS-LIVEBOOT-EXEC",
            *self.ready_sequence(),
        ]
        selected = bl.enter_liveboot(link, "embedded", 30.0, None, 1024, 3, 5.0)
        self.assertEqual(selected, "embedded")
        self.assertEqual(link.write_all.call_args_list[:2], [mock.call(b"\r"), mock.call(b"3")])

    def test_ram_upload_sequence_selects_option_one_and_executes_payload(self) -> None:
        link = mock.Mock()
        link.wait_for.side_effect = [
            "PMOSBOOT MENU-PROBE TIMEOUT_MS=00000bb8",
            "PMOSBOOT PASS-MENU-TRIGGER: BYTE: 0x0000000D",
            "PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT",
            "PMOSBOOT MENU-READY TIMEOUT_MS=00001388",
            "PMOSBOOT PASS-MENU-CHOICE: SELECTED: 0x00000001",
            "PMOSRAM READY 2",
            *self.ready_sequence(),
        ]
        payload = bl.LivePayload(Path("payload.bin"), 4, "0" * 64, 0x86C00000, 0x86C00000)
        with mock.patch.object(Path, "read_bytes", return_value=b"test"), \
             mock.patch.object(bl, "send_ram_payload") as send_payload:
            selected = bl.enter_liveboot(link, "ram-upload", 30.0, payload, 1024, 3, 5.0)
        self.assertEqual(selected, "ram-upload")
        self.assertEqual(link.write_all.call_args_list[:2], [mock.call(b"\r"), mock.call(b"1")])
        send_payload.assert_called_once()

    def test_legacy_two_option_menu_supports_ram_upload(self) -> None:
        link = mock.Mock()
        link.wait_for.side_effect = [
            "PMOSBOOT MENU-PROBE TIMEOUT_MS=00000bb8",
            "PMOSBOOT PASS-MENU-TRIGGER: BYTE: 0x0000000D",
            "PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY",
            "PMOSBOOT MENU-READY TIMEOUT_MS=00001388",
            "PMOSBOOT PASS-MENU-CHOICE: SELECTED: 0x00000001",
            "PMOSRAM READY 2",
            *self.ready_sequence(),
        ]
        payload = bl.LivePayload(Path("payload.bin"), 4, "0" * 64, 0x86C00000, 0x86C00000)
        with mock.patch.object(Path, "read_bytes", return_value=b"test"), \
             mock.patch.object(bl, "send_ram_payload") as send_payload:
            selected = bl.enter_liveboot(link, "ram-upload", 30.0, payload, 1024, 3, 5.0)
        self.assertEqual(selected, "ram-upload")
        self.assertEqual(link.write_all.call_args_list[:2], [mock.call(b"\r"), mock.call(b"1")])
        send_payload.assert_called_once()

    def test_auto_uses_ram_upload_without_reset_on_legacy_menu(self) -> None:
        link = mock.Mock()
        link.wait_for.side_effect = [
            "PMOSBOOT MENU-PROBE TIMEOUT_MS=00000bb8",
            "PMOSBOOT PASS-MENU-TRIGGER: BYTE: 0x0000000D",
            "PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY",
            "PMOSBOOT MENU-READY TIMEOUT_MS=00001388",
            "PMOSBOOT PASS-MENU-CHOICE: SELECTED: 0x00000001",
            "PMOSRAM READY 2",
            *self.ready_sequence(),
        ]
        payload = bl.LivePayload(Path("payload.bin"), 4, "0" * 64, 0x86C00000, 0x86C00000)
        with mock.patch.object(Path, "read_bytes", return_value=b"test"), \
             mock.patch.object(bl, "send_ram_payload") as send_payload:
            selected = bl.enter_liveboot(link, "auto", 30.0, payload, 1024, 3, 5.0)
        self.assertEqual(selected, "ram-upload")
        self.assertEqual(link.write_all.call_args_list[:2], [mock.call(b"\r"), mock.call(b"1")])
        send_payload.assert_called_once()

    def test_legacy_menu_rejects_embedded_path_with_clear_error(self) -> None:
        link = mock.Mock()
        link.wait_for.side_effect = [
            "PMOSBOOT MENU-PROBE TIMEOUT_MS=00000bb8",
            "PMOSBOOT PASS-MENU-TRIGGER: BYTE: 0x0000000D",
            "PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY",
            "PMOSBOOT MENU-READY TIMEOUT_MS=00001388",
        ]
        with self.assertRaisesRegex(bl.ProtocolError, "legacy two-option menu"):
            bl.enter_liveboot(link, "embedded", 30.0, None, 1024, 3, 5.0)
        self.assertEqual(link.write_all.call_args_list, [mock.call(b"\r")])

    def test_liveboot_handoff_restores_115200_and_recognizes_linux(self) -> None:
        link = mock.Mock()
        link.buffer = bytearray(b"stale")
        link.wait_for.side_effect = [
            "PMOS3 LIVEBOOT-READY",
            "PMOS3 LIVEBOOT-HEADER-ACK flags=00000002",
            "PMOS3 MANIFEST-OBJECT-VERIFIED",
            "PMOS3 MANIFEST-ACCEPTED",
            "PMOS3 IMAGE-OBJECT-VERIFIED",
            "PMOSLIVE SPIM-VERIFIED LOAD=81000000 ENTRY=81000000",
            "PMOSLIVE SQUASHFS-VERIFIED ADDRESS=87000000",
            "PMOSLIVE RESULT IMAGE-READY",
            "PMOSLIVE BOOT-CHALLENGE deadbeef",
            "PMOSLIVE CONFIRMATION-WAIT COMMAND=BOOTRAM",
            "PMOSLIVE CONFIRMATION-ACK",
            "PMOSLIVE BOOT-PLAN MEM=120M",
            "PMOSLIVE UART-RESTORE RATE=115200 FROM=921600",
            "PMOSLIVE UART-BASELINE-READY RATE=115200",
            "PMOSLIVE EXEC ENTRY=81000000",
        ]
        link.read_line.side_effect = [
            "[    0.000000] Linux version 3.18.123-test",
            "[    0.000000] VCOREIII PROM FWARGS a0=0000000d a1=a0000400 a2=a0000600 a3=00000000",
            "[    0.000000] VCOREIII PROM ARGV-ACCEPTED argc=12 envc=3",
            "[    0.000000] MIPS CMDLINE-SOURCE=firmware",
            "[    0.000000] Initial ramdisk at: 0x87000000 (8269824 bytes)",
            (
                "[    0.000000] Kernel command line: console=ttyS0,115200 mem=120M "
                "rd_start=0x87000000 rd_size=0x007e3000 root=/dev/ram0 "
                "rootfstype=squashfs postmerkos.live=1"
            ),
            "[    2.000000] VFS: Mounted root (squashfs filesystem) readonly on device 1:0.",
            "PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0",
            "PMOSLIVE PLATFORM-READY MODEL=MS42P SOURCE=pmoslive-command-line",
        ]
        selection = pv3.TransportSelection(921600, 4096, 1, True, True, True)
        plan = pv3.RepresentationPlan(pv3.REP_RAW, 4096, ())
        bundle = mock.Mock(image=Path("image.bin"), manifest_bytes=b"{}", model="MS42P")
        controller = mock.Mock()
        with mock.patch.object(pv3, "choose_representation", return_value=plan), \
             mock.patch.object(pv3, "make_manifest_plan", return_value=plan), \
             mock.patch.object(pv3, "make_live_package_header", return_value=b"header"), \
             mock.patch.object(pv3, "send_frames"):
            result = pv3.send_liveboot_v3(
                link, bundle, selection, dry_run=False, force=False,
                baud_controller=controller,
            )
        self.assertEqual(
            result,
            "PMOSLIVE PLATFORM-READY MODEL=MS42P SOURCE=pmoslive-command-line",
        )
        self.assertIn(mock.call(b"PMOS3 LIVEBOOT\n"), link.write_all.call_args_list)
        self.assertIn(mock.call(b"BOOTRAM deadbeef\n"), link.write_all.call_args_list)
        controller.set_rate.assert_called_once_with(115200, flush=True)
        self.assertEqual(link.buffer, bytearray())


    def test_liveboot_rejects_the_observed_flash_root_fallback(self) -> None:
        link = mock.Mock()
        link.read_line.side_effect = [
            "[    0.000000] Linux version 3.18.123-meraki-elemental",
            "[    0.000000] VCOREIII PROM ARGV-ABSENT builtin fallback eligible",
            "[    0.000000] Initrd not found or empty - disabling initrd",
            (
                "[    0.000000] Kernel command line: console=ttyS0,115200 "
                "root=/dev/mtdblock3 mem=134152192"
            ),
        ]
        with self.assertRaisesRegex(pv3.ProtocolError, "did not accept argc/argv/envp"):
            pv3.wait_for_liveboot_success(link, 30.0)

    def test_liveboot_rejects_flash_mtd_root_even_after_a_valid_cmdline_marker(self) -> None:
        link = mock.Mock()
        link.read_line.side_effect = [
            "VCOREIII PROM ARGV-ACCEPTED argc=12 envc=3",
            "MIPS CMDLINE-SOURCE=firmware",
            "Initial ramdisk at: 0x87000000 (8269824 bytes)",
            (
                "Kernel command line: mem=120M rd_start=0x87000000 rd_size=0x7e3000 "
                "root=/dev/ram0 rootfstype=squashfs postmerkos.live=1"
            ),
            "VFS: Mounted root (squashfs filesystem) readonly on device 31:3.",
        ]
        with self.assertRaisesRegex(pv3.ProtocolError, "SPI-flash SquashFS"):
            pv3.wait_for_liveboot_success(link, 30.0)

    def test_liveboot_rejects_incomplete_firmware_command_line(self) -> None:
        link = mock.Mock()
        link.read_line.side_effect = [
            "VCOREIII PROM ARGV-ACCEPTED argc=4 envc=0",
            "MIPS CMDLINE-SOURCE=firmware",
            "Initial ramdisk at: 0x87000000 (8269824 bytes)",
            "Kernel command line: console=ttyS0,115200 root=/dev/ram0",
        ]
        with self.assertRaisesRegex(pv3.ProtocolError, "live command line is missing"):
            pv3.wait_for_liveboot_success(link, 30.0)


    def test_liveboot_rejects_platform_mismatch(self) -> None:
        link = mock.Mock()
        link.read_line.side_effect = [
            "VCOREIII PROM ARGV-ACCEPTED argc=12 envc=3",
            "MIPS CMDLINE-SOURCE=firmware",
            "Initial ramdisk at: 0x87000000 (8269824 bytes)",
            (
                "Kernel command line: mem=120M rd_start=0x87000000 rd_size=0x7e3000 "
                "root=/dev/ram0 rootfstype=squashfs postmerkos.live=1"
            ),
            "VFS: Mounted root (squashfs filesystem) readonly on device 1:0.",
            "PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0",
            "PMOSLIVE PLATFORM-READY MODEL=MS42 SOURCE=pmoslive-command-line",
        ]
        with self.assertRaisesRegex(pv3.ProtocolError, "platform mismatch"):
            pv3.wait_for_liveboot_success(link, 30.0, "MS42P")

    def test_liveboot_requires_platform_after_userspace(self) -> None:
        link = mock.Mock()
        link.read_line.side_effect = [
            "VCOREIII PROM ARGV-ACCEPTED argc=12 envc=3",
            "MIPS CMDLINE-SOURCE=firmware",
            "Initial ramdisk at: 0x87000000 (8269824 bytes)",
            (
                "Kernel command line: mem=120M rd_start=0x87000000 rd_size=0x7e3000 "
                "root=/dev/ram0 rootfstype=squashfs postmerkos.live=1"
            ),
            "VFS: Mounted root (squashfs filesystem) readonly on device 1:0.",
            "PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0",
        ]
        with mock.patch.object(pv3.time, "monotonic", side_effect=[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 31.0]):
            with self.assertRaisesRegex(pv3.ProtocolError, "platform identity attestation"):
                pv3.wait_for_liveboot_success(link, 30.0, "MS42P")


if __name__ == "__main__":
    unittest.main()
