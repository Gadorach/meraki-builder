# postmerkOS hardware policy

## Purpose

This package provides capability-driven chassis controls that are outside the normal Click port configuration path. It currently owns reset-button discovery and hold timing, LED ownership/progress output, and the optional PoE boot-prune policy.

## Installed components

- `postmerkos-hwprobe` inventories reset-button and LED control candidates and renders runtime capabilities.
- `postmerkos-buttond` is a diagnostic monitor for verified reset-input mappings. The default initialization path performs read-only discovery and does not start destructive button handling.
- `postmerkos-ledctl` arbitrates LED ownership and renders progress using verified port or status LEDs.
- `postmerkos-poe-prune` performs the one-time boot observation for ports configured with the `boot-prune` PoE policy.
- `S16postmerkos-hardware` initializes the runtime records and daemons.

## Hardware controls

Physical wiring and control methods are registered in read-only profiles under:

```text
/usr/share/postmerkos/hardware-controls/
```

The active, boot-validated result is written to:

```text
/run/postmerkos/hardware-controls.json
/run/postmerkos/led-capabilities.env
```

Persistent JFFS2 configuration stores policy only. It does not store GPIO numbers, event-device paths, or Click handlers, so a restored configuration cannot apply one model's wiring to another model.

## Reset-button safety

Reset discovery is read-only in this release. `destructive_enabled` is always false and no button daemon is started, even if a candidate mapping is found. An unidentified or unverified mapping leaves the daemon running in diagnostic mode and reports:

```text
run postmerkos-hwprobe reset-button --watch
```

A future release may enable destructive handling only after an exact model mapping is runtime-proven and separately reviewed.

## LED selection

`postmerkos-ledctl` selects the best verified output in this order:

1. Binary per-port LEDs through `/click/sw0_ctrl/poe_led_state` only on MS220-8/MS220-8P.
2. The dual green/orange chassis handlers using `STATE 0|1`.
3. Serial/UI progress only when no LED output is available.

A single status LED uses an accelerating pattern from a 2.0-second cycle at 0% to a 0.8-second cycle at 100%. LED ownership is restored to normal Click/platform behavior when an operation ends.

## Diagnostics

```sh
postmerkos-hwprobe reset-button
postmerkos-hwprobe reset-button --watch 30
postmerkos-hwprobe leds
postmerkos-hwprobe leds --watch 30
postmerkos-ledctl capabilities
postmerkos-ledctl test-status
```

Do not mark a guessed GPIO or event device as verified. Capture the unpressed and pressed observations on the actual model first.
