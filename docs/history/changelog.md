# 2026-07-19 PMOSLIVE standard MIPS/U-Boot kernel handoff

- Added a managed VCore-III Linux 3.18 patch that accepts the standard 32-bit MIPS `argc`/`argv`/`envp` boot convention and retains the compiled SPI-flash command line only as a no-arguments fallback.
- Added compatibility with U-Boot legacy `memsize`, `initrd_start`, and `initrd_size` environment records.
- Bound live capability to a content-verified kernel build contract so stale pre-patch kernels remain normally flashable but cannot be advertised for PMOSLIVE.
- Required kernel, initrd, RAM-root, and live-userspace evidence before the host reports a successful live boot.

# 2026-06-30 responsive interface, PMC parity, and local discovery

- Added phone-specific accordion port editing, six-port groups, atomic SFP pairs, outlined/labelled banks, bounded All Ports cloning, collapsed port names, and an eight-card responsive header.
- Rebuilt login as a bounded accessible card with remembered-session preference and structured connection status.
- Added default `postmerkos.local`, validated persistent hostnames, management-only mDNS, canonical timezones, and exact uplink metadata.
- Added PMC parity for system information, cloning, SSH keys, firmware repositories, identity/mDNS, timezones/manual time, and telemetry.
- Added a machine-readable management feature manifest and cross-interface parity test.

# 2026-06-29 OpenVTSS v0.8 evidence synchronization

- Added the hardware-proven representative MS42P copper bank boundaries and all four SFP logical/chip-local mappings to current architecture and compatibility documentation.
- Added operational guidance for interpreting retained 10 Gb/s mode while link is down and for treating module observations separately from compatibility certification.
- Isolated run identity, trace coverage, `symbol+8` probe registration, module ambiguity, and remaining PCS/LED/PoE research gaps under `docs/research/`.

# 2026-06-29 documentation and current-release consolidation

- Made the documentation database the authoritative current-behavior source.
- Reduced the root README to onboarding, safety, quick-start, and documentation links.
- Moved release snapshots, root-cause reports, corrections, and migration notes under `docs/history/`.
- Added repository-wide documentation rules and automated root/history hygiene checks.
- Added complete current guides for system inventory, telemetry, reset/LED behavior, sessions/SSH keys, validation, responsive UI, and local time control.

# 2026-06-29 responsive UI, system inventory, and clock control

- Added resilient identity, firmware, CPU/kernel, runtime, memory, filesystem, MTD, service, temperature, hardware-control, and compatibility inventory.
- Added bounded card layouts, twelve-port/SFP wrapping, narrow two-column banks, sticky-header seam correction, and mobile-specific interaction layouts.
- Added common timezone presets and validated manual local clock input through the shared configd operation.

# 2026-06-28 reset button and status indication

- Added exact-model MS42P active-low reset-button handling with release-to-arm, debounce, cancellable hold, fail-closed policy, and firmware/reset lock arbitration.
- Added verified GPIO22/GPIO23 chassis indication for firmware progress, error/rollback, success, and reset countdown.

# 2026-06-27 management-plane merge repair

- Integrated telemetry into configuration transactions, primed SNMP snapshots, bounded Prometheus clients, and management-interface-only binding.
- Added live session revalidation/revocation, strict transactional SSH-key handling, and asynchronous recoverable firmware validation.

# 2026-06-20 UART reboot baud handoff

- Treat `PMOSREC REBOOT NOW` as the deterministic transition from negotiated
  recovery speed back to the permanent 115200-baud loader console.
- Reconfigure and flush the host TTY immediately after the final high-speed
  reboot marker, then accept multiple known loader/kernel banners as reboot
  confirmation.
- Preserve a successful flash result even when post-reset console monitoring is
  unavailable, while reporting the boot-baud handoff failure explicitly.

# 2026-06-20 UART recovery ACK and negotiation corrections

- Split meraki-redboot text output from byte-transparent binary UART output so
  compact ACKs and target-to-host qualification data preserve every byte.
- Replace the normal flasher baud sweep with one fastest-first pass over 921600,
  460800, and 230400 baud; retain the broad scan only as an explicit diagnostic.
- Add ACK-magic resynchronization and terminal-state recovery before issuing the
  next PMOSREC command after a failed feature test.
- Correct overflowed target `ERROR_PPM` diagnostics.
- Restore `postmerkos-ui:ms42p-dev` as the documented and tested default branch.

# 2026-06-20 management startup, console, and verified PoE corrections

- Replace cross-process loader source variables with durable JSON provenance records consumed by the loader build.
- Normalize future-dated tracked files after authoritative Git checkout without changing upstream content.
- Build the production web UI with an explicit configd WebSocket endpoint and reject inherited host `VITE_*` contamination.
- Replace the raw network-bootstrap JSON serial dump with concise PASS/WARN/FAIL boot lines while retaining the complete response under `/run/postmerkos/network-bootstrap.json`.
- Move hardware-verified PoE GPIO mappings into the exact-model board profile and enable the original reset/enable sequence on all supported PoE variants.
- Add configd exit records, WebSocket `configd-ws`/protocol-2 health probing, bounded restart supervision, and retained runtime logs.
- Distinguish management-service failure from genuine role denial in PMC and login-shell startup.
- Add serial `status`, `logs`, `login`, `retry`, `reboot`, and `help` recovery commands with audit logging.

# 2026-06-20 authoritative upstream main policy

- Make `Gadorach/meraki-redboot:main` and `Gadorach/postmerkos-ui:ms42p-dev` the default authoritative sources.
- Treat the legacy `LOADER_REF=latest` value as an alias for `main`, never as the newest version tag.
- Remove all meraki-redboot patch files and automatic source-repair commits from meraki-builder.
- Reject dirty, stale, or contract-incompatible upstream checkouts instead of modifying them.
- Remove the automatic v0.7.0 archive fallback and require a refreshed Git source.
- Record both the requested and normalized source ref plus the exact selected commit in artifact provenance.
- Add regression coverage proving that repeated builder runs advance to newer upstream `main` commits while leaving both source trees unchanged.

# 2026-06-20 PMOSREC v3 adaptive UART transport

- Keep the meraki-redboot menu and PMOSRAM executable upload fixed at 115200 baud, then negotiate optimized transport only after PMOSREC starts in RAM.
- Add target-divisor-aware baud proposals, bidirectional deterministic qualification, autonomous rollback, and optional diagnostic midpoint refinement with a 2% cutoff.
- Add preferred 4 KiB frames, negotiated windows up to 16 frames, CRC-protected compact cumulative acknowledgements, and selective retransmission.
- Validate the manifest before the large image transfer and qualify raw, sparse, LZ4, and sparse-LZ4 representations before selecting the smallest verified wire image.
- Preserve per-frame CRC-32, object CRC-32/SHA-256, reconstructed full-image SHA-256, model/layout/JEDEC checks, and complete flash readback verification.
- Add immediate host-side progress and measured ETA, infinite manual erase-confirmation retries, authorized automatic live-challenge response, and a target-side five-second reboot after success.
- Add the `PMOSRECOVERY3`, `PREFLIGHT=4`, and `pmosrec-v3-adaptive-uart-sparse-lz4-v1` cache and release-manifest boundaries.

# 2026-06-20 bounded embedded release metadata

- Replace the fixed 4 KiB trailer copy of the complete release manifest with a compact `embedded-update-index-v1` record.
- Keep complete recovery, hardware-preflight, loader, and digest metadata in the in-rootfs and checksummed sidecar manifests.
- Prevent release-manifest growth from aborting post-image generation while preserving legacy image-only version/model discovery.
- Add regression coverage using an authoritative manifest larger than the trailer slot.

# 2026-06-19 integrated VCore-III stabilization

- Restore platform-agnostic inclusion and hash verification of common, Luton26, Jaguar1, Jaguar Dual, and auxiliary donor kernel modules.
- Generate exact runtime board identity under `/run/postmerkos/boardinfo`; unknown identity fails closed.
- Align exact model families and logical port counts across module loading, Click, configd, hardware policy, updater, and documentation.
- Add framed, acknowledged, CRC-32 and SHA-256 verified UART firmware transport with optional manifest and host flasher support.
- Preserve the full 8 MiB SquashFS region and use sidecar metadata when no safe trailer padding exists.
- Restore apply-before-persist configuration transactions, runtime rollback, known-good recovery, and desired/observed service reporting.
- Add capability schema v2, dual green/orange named-state LED ownership, read-only reset discovery, and fail-closed PoE GPIO policy.
- Preserve browser artifact names/manifests and clean only stale upload cache entries.
- Distinguish untested and known-incompatible firmware in both browser and text console; incompatible artifacts cannot be acknowledged.

## Lean authenticated management and CLI

- Add local `/etc/shadow` authentication through the existing system `crypt()` implementation without Linux-PAM.
- Keep uClibc locale and wide-character support disabled and remove the PAM/Flex dependency chain.
- Add authenticated firmware upload, bounded browser command execution, password updates, and configuration restore.
- Add the automatic hierarchical SSH/TTL postmerkOS console, chassis-aware port grouping, and raw-shell escape.
- Make the console/configuration core mandatory while keeping libwebsockets, uhttpd, and the browser stack optional.
- Preserve the existing authenticated WebSocket protocol when `INCLUDE_UI=1`.
- Use plain JSON configuration backup/restore; no backup crypto helper or extra backup-specific library is included.
- Add clickable/hoverable port graphics and a persistent Apply/Discard notification for unapplied browser changes.

## DHCP and web-service reliability fixes

- Wait up to 60 seconds for a DHCP lease during early network bootstrap before applying the link-local fallback.
- Support Click graphs that expose only `dhcpc_state_for_brain`, including dotted subnet masks.
- Print the selected management address, gateway, broadcast, and source on the serial console.
- Continue polling after fallback and rebind uhttpd whenever the applied management address changes.
- Verify configd TCP 4001 and uhttpd TCP 80 listeners before reporting successful startup.
- Add persistent runtime diagnostics in `/tmp/configd.log`, `/tmp/uhttpd.log`, and `/tmp/network-rebind.log`.
- Correct the MS42/MS42P access-policy initialization range from 1-9 to 1-8.

## Distrobox Buildroot hotfix

- Route the complete Arch/CachyOS build through Ubuntu 22.04 Distrobox instead of containerizing only the kernel stage.
- Protect direct `make rootfs` runs with the same Distrobox routing.
- Record the Buildroot host environment and clean output automatically when the distribution or host compiler changes.
- Document the GCC 16 versus binutils 2.38 failure and recovery procedure.

# MS42P build workflow changes

- Added a top-level Makefile and staged Bash build interface.
- Added interactive `make all`, explicit `make base`, and explicit `make web` workflows.
- Added CachyOS/Arch and Ubuntu/Debian dependency installation.
- Added an Ubuntu 22.04 distrobox route for the legacy kernel/OpenWrt toolchain.
- Pinned and automated the known Linux 3.18/OpenWrt source checkout.
- Added donor firmware discovery, download, size validation, extraction, and module validation.
- Made donor modules the default binary-only import instead of replacing the maintained `/etc` tree.
- Added optional fallback import of donor `/etc` beneath the repository overlay.
- Added optional cloning and building of `Gadorach/postmerkos-ui:ms42p-dev`.
- Added a portable Node.js fallback.
- Normalized the `status` and `findhdr` Buildroot package layouts.
- Replaced line-number-sensitive custom package patches with generated Kconfig registration.
- Added maintained configd/uhttpd init scripts as an optional web feature overlay.
- Integrated the released Click storm-control compatibility behavior directly into configd.
- Reworked the MS220 post-build script for quoting and deterministic source-controlled transformations.
- Replaced the incomplete mixed U-Boot/RedBoot post-image script with deterministic MS42P NOR assembly.
- Added image and rootfs validation, SHA-256 manifests, revision records, and effective-file manifests.

## Pre-kernel UART recovery protocol v2

- Separated host-only verification, target dry-run, and destructive flash
  operations.
- Added acknowledged, retryable binary framing with partial-write-safe host I/O.
- Added bounded probe, frame, object, confirmation, erase, and program states.
- Added target-specific Luton26 and Jaguar-class payload descriptors.
- Bound loader, payload, image, manifest, model, family, flash geometry, and
  JEDEC identity into the release contract.
- Added per-frame CRC-32, whole-object CRC-32 and SHA-256, cache maintenance,
  flash status checks, block-protection checks, and full readback verification.
- Integrated the source-built UART-capable loader and recovery payloads into the
  normal VCore-III build and aggregate host tests.

## 2026-06-19 integrated review remediation

- Corrected bootloader-recovery operation routing so host verification never
  opens serial, dry-run cannot issue flash commands, and full flash requires
  both host authorization and the target nonce challenge.
- Replaced fixed-delay recovery sequencing with protocol-v2 ready, header,
  frame, object-verification, challenge, progress, and terminal-result states.
- Added short-write-safe serial output, bounded retries, exact duplicate-frame
  acceptance, and completion-as-final-ACK handling.
- Added source-built loader attestation and family-specific recovery payload
  records to the firmware build and release manifest.
- Added target/model/SoC/layout/JEDEC/geometry checks and manifest enforcement
  before destructive SPI access.
- Added deterministic host simulations, source-contract tests, complete-image
  manifest fixtures, aggregate UI/configd contract coverage, and recovery
  payload structural builds.
- Refreshed the UI development dependency lockfile to a zero-advisory audit.
## 2026-06-20 UART recovery manifest digest binding

- Corrected PMOSREC JSON lookup so nested digest fields cannot shadow direct object members.
- Added the `direct-object-members-v1` recovery payload contract to loader, release-manifest, and flasher validation.
- Forced rebuild/rejection of recovery payloads that lack the scoped manifest parser.


## 2026-06-20 pre-boot UART SPI NOR hardware preflight

- Re-enable and verify the Jaguar1 SPI master while preserving `GENERAL_CTRL`.
- Move JEDEC/status/SFDP checks before firmware package transfer.
- Add `PMOSPFT1` scratch-sector backup, erase, program, readback, restoration, and bootloader CRC preservation checks.
- Add direct `--bootloader-preflight` operation, atomic JSON receipts, and hard host/target protection below `0x00040000`.
- Bind the hardware-preflight and SPI master-enable contracts into loader, payload, rootfs, and final artifact manifests.

## 2026-06-20 MSCC software-SPI chip-select correction

- Corrected `SW_SPI_CS` from inverted pin-level handling to the MSCC active-mask contract.
- Ported the known-good MSCC U-Boot CS0 activation, mode-0 transfer, and deactivation sequence.
- Added `PREFLIGHT=3` and the `spi-nor-scratch-rw-restore-loader-crc-v3` cache boundary.
- Added early chip-select contract diagnostics before the JEDEC probe.
