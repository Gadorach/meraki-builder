# Build artifacts and validation

## VCore-III firmware set

A successful switch build publishes:

```text
<image>.bin
<image>.bin.sha256
<image>.bin.manifest.json
<image>.bin.manifest.json.sha256
loader1.bin
loader1.bin.sha256
loader1.bin.manifest.json
loader1.bin.source.json
meraki-redboot-version.txt
meraki-redboot-source-revision.txt
tools/mkvcoreiii_payload.py
recovery/recovery-luton26.bin
recovery/recovery-luton26.bin.sha256
recovery/recovery-luton26.descriptor.json
recovery/recovery-jaguar1.bin
recovery/recovery-jaguar1.bin.sha256
recovery/recovery-jaguar1.descriptor.json
```

`loader1.bin` is a 256 KiB boot region built from the selected
`Gadorach/meraki-redboot` source release. `loader1.bin.source.json` binds the
published binary to the repository, version, commit, and selected `strict`, `development`, or `permissive`
variant. The copied payload packer is the exact implementation used to create
the image's kernel header.

The complete image is exactly 16 MiB:

```text
0x000000-0x03ffff  source-built meraki-redboot
0x040000-0x2fffff  SPIM kernel region
0x300000-0xafffff  SquashFS region
0xb00000-0xffffff  JFFS2/config region
```

The SPIM region contains a 32-byte header followed by a zero-padded,
32-byte-aligned compressed kernel. The finalizer verifies load and entry
addresses, reserved words, payload length, hard slot boundary, CRC-32, and
payload SHA-256. The manifest records all of these values under
`artifact.kernel_payload`.

The release manifest also records:

- exact model compatibility states;
- meraki-redboot source version/revision, variant, toolchain, and policies;
- boot-menu options and structured diagnostic capability;
- loader-region digest and embedded recovery-stage digests;
- recovery descriptors, flash geometry, accepted JEDEC IDs, and delivery paths;
- per-region offsets and sizes.

A rootfs that fills the 8 MiB SquashFS region is copied byte-for-byte. When
natural padding leaves at least 4 KiB, the image receives a compact
`embedded-update-index-v1` trailer containing only version, exact-model states,
target family, and API/schema identifiers. The complete recovery, preflight,
loader, region, and digest contract remains in `/etc/postmerkos-release.json`
and the checksummed sidecar release manifest, which is authoritative. Growth of
the complete manifest can therefore never make the firmware image build fail.

## Validation targets

```sh
make test-image
make test-fwupdate
make test-docs
make validate
```

`make test-image` checks canonical SPIM packing, alignment, CRC, final release
metadata, and full-capacity SquashFS handling. `make test-fwupdate` exercises
normal updater transports, meraki-redboot menu selection, embedded recovery
validation, frame retry behavior, and host-only bundle verification.
