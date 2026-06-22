# Project history

PostmerkOS began as a Buildroot replacement userspace for Meraki Vitesse switches using the vendor Linux kernel, Click graph, and binary switch modules. The project added reproducible NOR assembly, JFFS2 persistence, local SSH management, an optional browser interface, a configuration daemon, safe update transports, and hardware recovery tools.

The current architecture uses configd as a shared role-aware management core, a hierarchical serial/SSH console as the primary UI, and an optional authenticated web frontend. Hardware capability records and compatibility reports extend the firmware beyond a single MS42P target while retaining direct recovery paths.

## Pre-kernel UART recovery architecture

The recovery design evolved into two versioned protocols. LinuxLoader uses an
acknowledged RAM-upload protocol with bounded receive states, CRC-32 frames,
whole-object SHA-256, DRAM range checks, and cache maintenance. The uploaded
recovery program uses a separate manifest-bound package protocol and
SoC-specific payloads. Host and target validate exact model compatibility,
loader identity, image layout, payload records, flash geometry, and JEDEC IDs
before a nonce-gated complete-NOR write. Verify and dry-run are distinct
non-destructive operations.
