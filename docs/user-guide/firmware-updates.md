# Firmware updates

Fwupdate verifies checksum, release metadata, exact-model compatibility, flash
geometry, version direction, overlay policy, and requested flash scope before
any destructive work.

## Compatibility states

- `validated`: normal update path for this release artifact.
- `confirmed`: accepted normal path based on recorded target confirmation.
- `untested`: requires the explicit `ACCEPT-UNTESTED` acknowledgement.
- `known-incompatible`: blocked and cannot be acknowledged.
- missing or unsupported metadata: blocked by the normal path.

Installing the same or an older version requires force. Force does not override
a `known-incompatible` model or invalid geometry.

## Sources

Local files, TFTP, HTTP/HTTPS, SFTP, browser upload, and Linux UART all feed the
same updater engine. Browser upload preserves the original artifact filename and
matching JSON manifest. Stale upload-cache entries are cleaned without deleting
an object already owned by a running update.

For a networkless update while Linux is running, select **Firmware Update →
Receive and install an image over UART**, run:

```sh
postmerkos-console firmware uart
```

or use:

```sh
./tools/firmware-flasher/firmware-flasher.sh --control serial --transport uart
```

Linux UART uses acknowledged Base64 frames with sequence numbers and CRC-32,
then verifies exact byte count and whole-object SHA-256. The image and manifest
are reconstructed in RAM and then passed to `fw_update`, so UART does not weaken
model, metadata, version, overlay, or full-flash checks.

## Lifecycle

```text
idle → receiving → verifying → ready → waiting-for-client-acknowledgement
     → starting → erasing → writing → verifying-flash
     → persisting-result → rebooting → complete/failed
```

Verification never begins a write. Live status and logs are under
`/run/fwupdate`; bounded history is under
`/config/postmerkos/update-history`. Interrupted operations are finalized as
interrupted or indeterminate, not as success.

The rootfs is single-bank. Power loss during erase or program can require
external recovery even when configuration preservation was selected.

## Pre-kernel recovery

When Linux cannot run but the UART-capable loader still starts, use the
[pre-kernel recovery procedure](../installation/recovery.md#pre-kernel-uart-recovery).
Its verify and dry-run modes are non-destructive; flash mode rewrites the entire
16 MiB device and requires separate host and target confirmations.
