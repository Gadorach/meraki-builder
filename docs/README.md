# postmerkOS documentation database

This index is the authoritative navigation map for the current repository revision. Active guides describe current behavior. Completed incidents, superseded designs, and archived release notes are isolated under **History**; incomplete hardware investigations are isolated under **Research**.

## Start here

- [Installation overview](getting-started/installation.md)
- [First boot and initial access](getting-started/first-boot.md)
- [Hardware compatibility](hardware/compatibility.md)
- [Hardware flashing](installation/hardware-flashing.md)
- [Recovery](installation/recovery.md)

## Administration and operation

- [Console](user-guide/console.md)
- [Web interface and responsive layout](user-guide/web-interface.md)
- [System Information inventory](user-guide/system-information.md)
- [Ports, VLANs, STP, and PoE](user-guide/ports-and-poe.md)
- [Management network, SSH, and time](user-guide/network-ssh-time.md)
- [System identity and local discovery](user-guide/system-identity-and-discovery.md)
- [Accounts, roles, sessions, and SSH keys](user-guide/accounts-roles.md)
- [Backup and restore](user-guide/backup-restore.md)
- [Firmware updates](user-guide/firmware-updates.md)
- [Reset button and chassis status LED](user-guide/reset-button-and-status-led.md)
- [SNMP and Prometheus telemetry](user-guide/telemetry.md)
- [Service management](user-guide/services.md)

## Building and release validation

- [Switch builds](building/switches.md)
- [Authoritative upstream source policy](building/upstream-source-policy.md)
- [Artifacts and release manifests](building/artifacts.md)
- [Validation targets and release gates](building/validation.md)
- [MX80 build](building/mx80.md)
- [MX84 retained assets and build status](building/mx84.md)

## Architecture and development

- [System overview](architecture/system-overview.md)
- [Flash layout and persistence](architecture/flash-layout.md)
- [Configd transaction and management architecture](architecture/configd.md)
- [Configuration schema](architecture/configuration-schema.md)
- [Security, roles, sessions, and SSH-key transactions](architecture/security-and-sessions.md)
- [Firmware updater](architecture/firmware-updater.md)
- [Click graph](architecture/click-system.md)
- [Pre-kernel UART recovery](architecture/pre-kernel-uart-recovery.md)
- [PMOSREC v3 adaptive UART transport](architecture/pmosrec-v3-adaptive-uart.md)
- [Pre-boot UART hardware preflight](architecture/pre-boot-uart-hardware-preflight.md)
- [Recovery flat-binary entry contract](architecture/recovery-flat-binary-entry.md)
- [MSCC software-SPI chip select](architecture/mscc-software-spi-chip-select.md)
- [Adding a model](development/adding-model.md)
- [Compatibility testing](development/compatibility-testing.md)
- [Runtime stabilization hardware testing](development/runtime-stabilization-testing.md)
- [PMOSLIVE firmware flasher and standard MIPS/U-Boot handoff](development/pmoslive-firmware-flasher-integration-2026-07-19.md)

## Component reference

- [Runtime package and host-tool index](reference/component-index.md)

## Hardware and research

- [Vitesse switch hardware access](hardware/vitesse-switches.md)
- [Cooling](hardware/cooling.md)
- [Research index](research/README.md)
- [OpenVTSS runtime boundaries](research/openvtss-runtime-boundaries.md)
- [MS42P port-map and SFP runtime evidence](research/ms42p-port-map-and-sfp-evidence.md)
- [Telemetry counter validation](research/telemetry-counter-validation.md)

## History

- [Complete project-history index](history/README.md)
- [Project history](history/project-history.md)
- [Change history](history/changelog.md)
- [Resolved issues](history/resolved-issues.md)
- [Legacy build systems](history/legacy-build-systems.md)
- [2026-06-29 release notes](history/release-notes-2026-06-29.md)
- [2026-06-29 validation snapshot](history/validation-snapshot-2026-06-29.md)
- [2026-06-27 merge repair](history/merge-repair-2026-06-27.md)
- [2026-06-20 WebSocket build-cache and local-socket correction](history/websocket-build-cache-and-local-socket-fix-2026-06-20.md)
- [UART package-header handoff correction](history/uart-recovery-package-header-handoff.md)

## Documentation maintenance

Repository-wide documentation policy is defined in [`DOCUMENTATION-RULES.md`](../DOCUMENTATION-RULES.md). Run `make test-docs` after editing documentation.
