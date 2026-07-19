#!/usr/bin/env python3
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = root / "scripts/tests/fixtures/kernel-uboot-args"
patch = root / "kernel/patches/0001-mips-vcoreiii-accept-uboot-bootargs.patch"
script = root / "scripts/apply-kernel-patches.sh"

with tempfile.TemporaryDirectory() as directory:
    work = Path(directory) / "work"
    tree = work / "sources/switch-11-22-ms220"
    shutil.copytree(fixture, tree)
    subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
    subprocess.run(["git", "-C", str(tree), "config", "user.email", "test@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(tree), "config", "user.name", "test"], check=True)
    subprocess.run(["git", "-C", str(tree), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(tree), "commit", "-qm", "fixture"], check=True)

    env = os.environ.copy()
    env["MS42P_WORK_DIR"] = str(work)
    env["MS42P_ARTIFACTS_DIR"] = str(Path(directory) / "artifacts")
    subprocess.run([str(script)], check=True, env=env)
    # Applying twice must be idempotent.
    subprocess.run([str(script)], check=True, env=env)

    kernel = tree / "linux-3.18"
    kconfig = (kernel / "arch/mips/Kconfig.debug").read_text()
    setup = (kernel / "arch/mips/kernel/setup.c").read_text()
    prom = (kernel / "arch/mips/vcoreiii/vcoreiii_prom.c").read_text()
    defconfig = (kernel / "arch/mips/configs/msxx_defconfig").read_text()
    assert "config CMDLINE_FALLBACK" in kconfig
    assert "MIPS CMDLINE-SOURCE=firmware" in setup
    assert "MIPS CMDLINE-SOURCE=builtin-fallback" in setup
    assert "CONFIG_CMDLINE_FALLBACK=y" in defconfig
    assert "# CONFIG_CMDLINE_OVERRIDE is not set" in defconfig
    assert "VCOREIII PROM ARGV-ACCEPTED" in prom
    assert 'prom_find_environment("initrd_start")' in prom
    assert 'prom_find_environment("initrd_size")' in prom
    assert 'prom_find_environment("memsize")' in prom
    subprocess.run(["git", "-C", str(tree), "apply", "--reverse", "--check", str(patch)], check=True)

    subprocess.run([str(script), "--reverse"], check=True, env=env)
    assert not subprocess.check_output(
        ["git", "-C", str(tree), "status", "--porcelain"], text=True
    ).strip()

print("standard MIPS/U-Boot kernel boot-argument patch tests passed")
