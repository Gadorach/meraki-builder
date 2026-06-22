# Host tools

Normal host utilities are isolated by tool. No helper script is shared between
multiple tools, and the `research/` subtree is intentionally excluded from the
normal deployment and validation surface.

| Directory | Purpose |
|---|---|
| `firmware-flasher/` | Default manifest-aware in-system updater frontend, with an explicit `--legacy` checksum-only compatibility path. |
| `serial-console/` | Manual 115200 8N1 XON/XOFF hardware console. |
| `backup-and-flash/` | CH341A/flashrom full-NOR backup, write, and readback verification. |
| `fwupdate-publish/` | Publish manifest-aware or checksum-only firmware artifacts to an HTTP(S) repository. |
| `fwupdate-smoke-test/` | Host syntax, manifest, publication, TFTP, and updater preflight tests. |
| `research/` | Reverse-engineering and extraction utilities; not reviewed or invoked by normal tools. |

Each tool computes the repository root from its own location and contains all
private helpers within its own directory. A tool may locate and offer another
public tool—for example, the hardware flasher may open `serial-console` after a
successful write—but it does not source code from it.
