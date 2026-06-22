# Building VCore-III switch firmware

Run `make base` for a console-focused image or `make web` for the optional web
interface. The supported host path remains Ubuntu 22.04 through Distrobox when
building from an Arch-derived workstation.

The build prepares:

- the pinned kernel/OpenWrt source used for Linux 3.18 and headers;
- the latest `Gadorach/meraki-redboot` `main` revision by default;
- meraki-redboot's checksum-pinned GCC 4.7.3/binutils toolchain and 256 KiB boot
  region;
- the Luton26 and Jaguar1 recovery stages embedded by that loader;
- the watchmysys donor image, retained only for proprietary `.ko` extraction;
- Buildroot 2023.02.4, generated overlays, optional UI, SquashFS, and the
  complete 16 MiB NOR image.

## Loader source selection

```sh
make sources                     # fetches origin/main
LOADER_REF=main make loader       # explicit moving main branch
LOADER_REF=<commit> make loader   # optional exact revision
```

`main` is authoritative and is fetched from `origin` on every source preparation.
The compatibility value `LOADER_REF=latest` is treated as an alias for `main`, never
as a release tag. The resolved commit and VERSION are persisted in `artifacts/`,
and cached loader output is rebuilt when it no longer matches that revision.

The builder never applies patches, creates commits, or rewrites files in the
meraki-redboot checkout. Required loader or recovery changes must be committed to
`Gadorach/meraki-redboot` itself. If the selected source lacks a required contract,
the build stops with a contract error. The same immutable-upstream policy applies
to `Gadorach/postmerkos-ui`, whose default source is `origin/ms42p-dev`.

`LOADER_VARIANT=development` is the default for this project integration. It
retains the loader's warn-and-continue compatibility policy where continuation
is safe. Use `LOADER_VARIANT=strict` only after completing hardware acceptance. The
source project also exposes `permissive` for diagnostics; release images should
normally use `development` during bring-up or `strict` after validation. The
builder keeps the UART recovery menu enabled for every selected policy.

## Kernel image generation

The selected meraki-redboot checkout supplies `tools/mkvcoreiii_payload.py`.
The post-image stage uses it to pad `vmlinuz.bin` to 32 bytes, generate the
32-byte SPIM header, calculate CRC-32, and enforce a maximum payload of
`0x002bffe0`. A different hand-built header is not accepted.

## Donor boundary

The donor image is still downloaded from watchmysys when authorized. Its
purpose is currently limited to extracting the complete Vitesse/Click module
matrix and required runtime files. The build does not copy the donor loader or
use it to construct the boot region.


## Buildroot reuse and WebSocket feature safety

The builder records whether the last successful rootfs was a base or web image.
Changing that mode triggers a full Buildroot clean so files from disabled
packages cannot remain in `output/target`. The synchronized local configd source
and its WebSocket feature selection are fingerprinted separately; a changed
fingerprint runs `configd-dirclean` before the next rootfs build.

A web image contains `/etc/postmerkos/features/web-ui`. At boot, `S15configd`
requires a WebSocket-enabled daemon whenever that marker exists. Final image
validation also requires the enabled feature marker and a `libwebsockets`
dependency, preventing a UI artifact from being published with a console-only
configd binary.

For a deliberate full rebuild:

```sh
CLEAN_BUILDROOT=1 make web
```

`CLEAN_BUILDROOT` is preserved when the build enters Ubuntu 22.04 Distrobox.

## Useful commands

```sh
make doctor
make sources
make loader
make donor
make base
make web
make test-all
```
