# Runtime stabilization hardware testing

Run these checks from serial with a verified SPI backup and recovery method.

## Identity and module selection

```sh
cat /run/postmerkos/boardinfo
/etc/init.d/S08kmods profile
lsmod
mount | grep ' /click '
cat /run/postmerkos/click-config.status
```

Confirm the exact model, expected Luton26/Jaguar1/Jaguar Dual family, correct logical port count, matching module vermagic/hash, and required Click handlers. Unknown identity must disable model-specific actions rather than borrowing another profile.

## Management transactions

Apply a valid port/network/service change and confirm desired and observed state match. Then deliberately target a missing required handler in a recovery environment and confirm the request fails, the prior configuration remains primary, and runtime rollback is attempted. Invalid external edits must restore the known-good configuration.

## Access and WebSocket

Confirm serial/SSH console lifecycle, role repeatability, noninteractive `postmerkosctl`, WebSocket protocol version 2, dynamic WS/WSS selection, authentication before subscriptions, and bounded unauthenticated/authenticated idle behavior.

## Reset discovery

```sh
postmerkos-hwprobe reset-button
postmerkos-hwprobe reset-button --watch 30
cat /run/postmerkos/hardware-controls.json
```

Discovery is read-only. Record input/GPIO/Click changes while pressing the button. Destructive reset remains disabled for every shipped profile; do not perform an erase test until a separately reviewed implementation exists.

## LED validation

```sh
postmerkos-hwprobe leds
postmerkos-ledctl capabilities
postmerkos-ledctl test-status
```

Only run `test-status` when the exact-model capability reports both `power_led_green` and `power_led_orange` writable. The protocol is `STATE n`; normal state is captured and restored. `led_mode` is manual-validation only. `poe_led_state` is an LED indication handler, not PoE power control, and is registered only for MS220-8/MS220-8P.

## UART updater

From the host:

```sh
./tools/firmware-flasher/firmware-flasher.sh --control serial --transport uart
```

Test an interrupted frame, retransmission, CRC rejection, complete image plus optional manifest, exact byte/SHA reconstruction, verification-only operation, system update, and—only with full recovery available—full-flash acknowledgement. Observe RAM use because the candidate is reconstructed under `/run/fwupdate/uploads`.

## Destructive hardware matrix

Record results separately for boot, Click forwarding, all copper/uplink ports, VLAN/STP/LACP, PD690xx PoE/af/at, status LEDs, read-only reset discovery, firmware system update, full flash, power loss during each erase/write region, and post-boot finalization. Host tests do not prove these behaviors.
