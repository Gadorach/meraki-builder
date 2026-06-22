# OpenVTSS findings applied to postmerkOS tooling

The supplied OpenVTSS v0.6 reverse-engineering material was reviewed as a
platform-boundary check. It does not replace the currently required donor
modules, but it clarifies how build and recovery names must map to the runtime.

## MS42/MS42P naming

meraki-redboot recovery classifies MS42/MS42P as `jaguar1` because pre-kernel
SPI access uses the Jaguar1 software-mode register at `0x70000068`. The Linux
switching stack classifies the same hardware as `jaguar_dual`: two Jaguar1
switch devices joined by a VStaX fabric. These identifiers belong to different
layers and must not be normalized into one name.

The final release manifest therefore stores recovery-family data independently
from the runtime module profile. Flasher model selection uses `jaguar1`; boot
module selection remains `jaguar_dual` for MS42/MS42P.

## Runtime module order

The recovered linkage confirms this dependency direction:

```text
vtss_core.ko -> merakiclick.ko -> elts_meraki.ko / vc_click.ko
```

The existing runtime keeps `vtss_core` first and `vc_click` last. MS42/MS42P
continue using the dual-chip VTSS/Click objects and their 52-port board profile.
The donor image remains the authoritative binary source until open replacements
cover register access, MIIM/PHY, FDMA, interrupts, GPIO/LED/reset, dual-chip
VStaX topology, and the Click-facing controller contract.

## Integration rule

Bootloader/recovery improvements may validate image format, flash geometry,
model identity, and safe SPI access. They must not silently change Linux module
family selection, port count, Click graph assumptions, or the order in which the
proprietary modules are loaded.
