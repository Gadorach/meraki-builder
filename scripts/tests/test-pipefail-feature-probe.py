#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

# Place the wanted marker before enough additional strings to guarantee that
# grep -q would close the pipe while strings is still writing. The production
# form deliberately omits -q and must therefore complete successfully under
# pipefail.
with tempfile.TemporaryDirectory() as td:
    sample = Path(td) / "sample.bin"
    with sample.open("wb") as f:
        f.write(b"websocket: enabled\0")
        for i in range(100000):
            f.write(f"STRING_{i:06d}_abcdefghijklmnopqrstuvwxyz\0".encode())
    subprocess.run(
        [
            "bash",
            "-c",
            "set -o pipefail; strings \"$1\" | grep -Fx 'websocket: enabled' >/dev/null",
            "probe",
            str(sample),
        ],
        check=True,
    )

print("Pipefail-safe configd feature probe test passed")
