# fwupdate package

The package installs the postmerkOS updater, HTTP/TFTP/SFTP frontends,
finalizer, factory-reset helper, JSON manifest helper, and static flash helper.

## Artifact contracts

Transport frontends require firmware, `.sha256`, `.manifest.json`, and
`.manifest.json.sha256` by default. `--no-manifest` is the explicit compatibility
mode for repositories containing only the original firmware and checksum. The
checksum remains mandatory in both modes; version metadata must come from the
image, a supplied `--version`, or `--force` for a deliberately unversioned
recovery/development image.

`fwmanifest` uses JSON-C rather than text extraction:

```sh
fwmanifest validate image.manifest.json
fwmanifest get image.manifest.json artifact.sha256
fwmanifest model image.manifest.json MS42P
fwmanifest compare 2026.06.18-2 2026.06.18-10
```


## UART transport

`fwserialrx` and `fw_update_uart` implement the RAM-backed `PMOSUART/1` transport for networkless updates. Firmware and an optional release manifest are sent as bounded Base64 frames carrying sequence numbers and CRC-32. Every accepted frame is acknowledged; duplicate retransmission of the most recently accepted frame is safe. Whole-object byte count and SHA-256 are verified before the object is published under `/run/fwupdate/uploads` and passed to normal `fw_update` validation. UART never bypasses model, manifest, version, geometry, overlay, or full-flash acknowledgement policy.

## Flash scopes

The default `system` scope changes only SquashFS and the selected JFFS2 policy.
`--full-flash` accepts only an exact 16 MiB image, validates four distinct MTD
regions, backs up every region, and writes JFFS2, SquashFS, kernel, then loader.
The loader is deliberately last. `FLASH-ALL` or the separate
`--accept-full-flash` acknowledgement is required in addition to normal update
confirmation. A failed write triggers verified rollback of every region that
may have changed.

The full scope intentionally accepts a raw bootloader/kernel layout so future
experimental U-Boot images can be installed, but board and MTD geometry checks
remain mandatory. Production remains RedBoot/LinuxLoader unless a full image is
explicitly selected.

## Capacity policy

The complete 8 MiB SquashFS partition remains usable. In-image metadata is
added only when the filesystem naturally leaves at least 4 KiB unused;
otherwise the checksum-bound sidecar manifest is authoritative.
