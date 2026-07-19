#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts/kernel-build-contract.py"
CONFIG_POLICY = REPO / "scripts/configure-liveboot-kernel.py"
PATCH = REPO / "kernel/patches/0001-mips-vcoreiii-accept-uboot-bootargs.patch"

REQUIRED = {
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
    "CONFIG_CMDLINE_BOOL": "y",
    "CONFIG_CMDLINE_OVERRIDE": "n",
    "CONFIG_CMDLINE_FALLBACK": "y",
}


class KernelBuildContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="kernel-build-contract-")
        self.root = Path(self.temp.name)
        self.record = self.root / "contract.json"
        self.revision = self.root / "revision.txt"
        self.revision.write_text("d167da8b01e46e29bf3347b8952ce9063ba75d29\n")
        self.config = self.root / ".config"
        self.config.write_text("\n".join(
            f"# {key} is not set" if value == "n" else f"{key}={value}"
            for key, value in REQUIRED.items()
        ) + "\n")
        self.vmlinuz = self.root / "vmlinuz"
        self.vmlinuz.write_bytes(b"ELF fixture")
        self.vmlinuz_bin = self.root / "vmlinuz.bin"
        self.vmlinuz_bin.write_bytes(b"binary fixture")
        self.headers = self.root / "linux-3.18.123.tar.bz2"
        self.headers.write_bytes(b"headers fixture")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def command(self, operation: str) -> list[str]:
        command = [
            sys.executable, str(SCRIPT), operation,
            "--record", str(self.record),
            "--source-revision-file", str(self.revision),
            "--patch", str(PATCH),
            "--config-policy", str(CONFIG_POLICY),
            "--vmlinuz", str(self.vmlinuz),
            "--vmlinuz-bin", str(self.vmlinuz_bin),
            "--headers", str(self.headers),
        ]
        if operation == "write":
            command.extend(["--config", str(self.config)])
        return command

    def run_command(self, operation: str, success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(self.command(operation), text=True, capture_output=True, timeout=30)
        if success and result.returncode:
            self.fail(result.stderr or result.stdout)
        if not success and not result.returncode:
            self.fail("kernel build contract unexpectedly verified")
        return result

    def test_write_and_verify(self) -> None:
        self.run_command("write")
        self.run_command("verify")
        record = json.loads(self.record.read_text())
        self.assertEqual(record["format"], "postmerkos.kernel-build-contract.v1")
        self.assertEqual(
            record["boot_argument_contract"],
            "vcoreiii-standard-mips-argc-argv-envp-fallback-v1",
        )
        self.assertEqual(record["resolved_config"], dict(sorted(REQUIRED.items())))

    def test_artifact_mutation_is_rejected(self) -> None:
        self.run_command("write")
        self.vmlinuz_bin.write_bytes(b"mutated")
        result = self.run_command("verify", success=False)
        self.assertIn("artifacts mismatch", result.stderr)

    def test_policy_change_is_rejected(self) -> None:
        self.run_command("write")
        copy = self.root / "changed-policy.py"
        copy.write_bytes(CONFIG_POLICY.read_bytes() + b"\n# changed\n")
        command = self.command("verify")
        index = command.index(str(CONFIG_POLICY))
        command[index] = str(copy)
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("config_policy mismatch", result.stderr)

    def test_missing_required_config_is_rejected_at_write(self) -> None:
        self.config.write_text(self.config.read_text().replace("CONFIG_CMDLINE_FALLBACK=y\n", ""))
        result = self.run_command("write", success=False)
        self.assertIn("CONFIG_CMDLINE_FALLBACK=<missing>", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
