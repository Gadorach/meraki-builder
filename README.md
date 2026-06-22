# postmerkOS

postmerkOS is an independent, locally managed firmware environment for selected Cisco Meraki switches. It replaces cloud-dependent management with an interactive SSH/serial console, a persistent configuration service, and an optional browser interface while retaining the vendor Linux/Click switching platform required by the hardware.

## Current capabilities

- Hierarchical `pmc` management console over SSH and hardware serial
- Optional authenticated web interface
- Administrator, operator, and viewer roles
- Port, VLAN, STP, LACP, multicast, PoE, and management-network configuration
- Persistent JSON configuration stored in JFFS2
- Local, browser, TFTP, HTTP/HTTPS, SFTP, Linux UART, and pre-kernel UART recovery workflows
- Release/version validation, update history, recovery reset, and external configuration backup
- DHCP or static management addressing
- SSH, chrony/NTP, UTC offset, and compact DST-rule configuration
- Capability-driven support for MS22, MS42, MS220, and MS320 Vitesse switch families
- Separate integrated MX80 build path and retained untested MX84 board assets

## Compatibility

Compatibility is release-specific. A release manifest promotes only exact models validated for that artifact; recognized but unvalidated models require explicit acknowledgement, and known-incompatible architecture or flash geometry remains blocked. One VCore-III image carries all supported Luton26, Jaguar1, and Jaguar Dual module families and selects the exact profile at boot.

See [Hardware compatibility](docs/hardware/compatibility.md) for the complete model table.

## Getting started

1. Read the [safety and installation overview](docs/getting-started/installation.md).
2. Make at least two verified backups of the original SPI flash.
3. Build an image with `make base` or `make web`, or use a validated release image.
4. Flash the complete image using the [hardware flashing guide](docs/installation/hardware-flashing.md).
5. Connect using serial, SSH, or the optional web interface and follow the [first-boot guide](docs/getting-started/first-boot.md).

VCore-III builds fetch the latest `Gadorach/meraki-redboot` `main` revision, compile its 256 KiB boot region and embedded family recovery stages from source, and use that checkout's canonical SPIM payload packer. The watchmysys donor remains only for proprietary Vitesse/Click module extraction. Release generation fails unless source provenance, boot-menu capability, SPIM alignment/CRC, recovery descriptors, model allow-lists, and flash geometry all match the final image.

Build help is available with:

```sh
make help
```

Buildroot output is safe to reuse across normal source edits. The build tracks
base-versus-web mode and fingerprints the local configd package, automatically
cleaning or invalidating stale output when required. Use `CLEAN_BUILDROOT=1 make
web` only when an explicit full Buildroot rebuild is desired; this setting is
forwarded through the supported Distrobox path.

## Safety

- Disconnect switch power before attaching or using an SPI programmer.
- Disconnect Ethernet cables and remove SFP modules during hardware flashing.
- UART is **3.3 V only**. Never connect the UART VCC pin.
- Cross-connect TX and RX between the switch and adapter.
- Never write flash until repeated backups have matching checksums.
- Hardware modification voids the manufacturer warranty and may permanently damage the device.

## Documentation

The [documentation index](docs/README.md) links installation, user, build, architecture, development, recovery, research, and project-history material.

This project is provided without warranty. Keep a direct hardware recovery method available while testing unconfirmed models or firmware-update changes.

## PMOSREC v3 adaptive pre-kernel recovery

The full-image UART recovery path keeps meraki-redboot and `PMOSRAM2` at
115200 baud, then tries 921600, 460800, and 230400 baud once each inside the
RAM-resident PMOSREC stage, fastest first. It qualifies bidirectional deterministic
CRC traffic, 4 KiB frames, flow-control-safe one-frame compact
acknowledgements, sparse reconstruction and LZ4 blocks before transferring the manifest and image. The complete reconstructed
16 MiB image is still SHA-256 verified before erase authorization. See
[`docs/architecture/pmosrec-v3-adaptive-uart.md`](docs/architecture/pmosrec-v3-adaptive-uart.md).
After a successful high-speed flash, the host detects `PMOSREC REBOOT NOW`,
returns the adapter to 115200 baud, and resumes normal boot monitoring.

### Authoritative upstream source policy

The build always refreshes `Gadorach/meraki-redboot` and
`Gadorach/postmerkos-ui` from `origin/ms42p-dev` by default. `meraki-builder` does
not apply patches, create repair commits, or rewrite either checkout. Loader,
recovery, and UI changes must be committed to their own repositories. The
builder records the exact selected commits and fails clearly when an upstream
contract is missing. See
[`docs/building/upstream-source-policy.md`](docs/building/upstream-source-policy.md).
