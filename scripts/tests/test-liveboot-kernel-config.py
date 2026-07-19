#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tool = root / "scripts" / "configure-liveboot-kernel.py"
with tempfile.TemporaryDirectory() as directory:
    config = Path(directory) / ".config"
    config.write_text(
        "# CONFIG_BLOCK is not set\n"
        "# CONFIG_BLK_DEV is not set\n"
        "CONFIG_BLK_DEV_INITRD=n\n"
        "# CONFIG_BLK_DEV_RAM is not set\n"
        "CONFIG_BLK_DEV_RAM_SIZE=4096\n"
        "CONFIG_SQUASHFS=m\n"
        "# CONFIG_SQUASHFS_XZ is not set\n"
        "CONFIG_UNRELATED=y\n",
        encoding="utf-8",
    )
    subprocess.run([str(tool), "--config", str(config)], check=True)
    subprocess.run([str(tool), "--config", str(config), "--verify"], check=True)
    result = config.read_text(encoding="utf-8")
    assert "CONFIG_UNRELATED=y" in result
    assert result.count("CONFIG_BLOCK=") == 1
    assert "CONFIG_BLOCK=y" in result
    assert result.count("CONFIG_BLK_DEV=") == 1
    assert "CONFIG_BLK_DEV=y" in result
    assert result.count("CONFIG_BLK_DEV_RAM_SIZE=") == 1
    assert "CONFIG_BLK_DEV_RAM_SIZE=16384" in result
    assert "CONFIG_SQUASHFS=y" in result
    assert "CONFIG_SQUASHFS_XZ=y" in result
print("PMOSLIVE kernel config tests passed")
