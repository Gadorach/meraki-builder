# Hardware compatibility

Compatibility is release-specific and exact-model based. The release manifest records each recognized model as `validated`, `confirmed`, `untested`, or `known-incompatible`. The running management API reads that manifest rather than claiming support from a hard-coded table.

| Exact model group | Driver family | Logical ports | Copper + uplink | PoE |
|---|---|---:|---:|---:|
| MS220-8 / MS220-8P | Luton26 | 10 | 8 + 2 | 0 / 8 |
| MS22 / MS22P | Luton26 | 26 | 24 + 2 | 0 / 24 |
| MS220-24 / MS220-24P | Luton26 | 26 | 24 + 2 | 0 / 24 |
| MS320-24 / MS320-24P | Jaguar1 | 28 | 24 + 4 | 0 / 24 |
| MS220-48, 48P, 48LP, 48FP | Jaguar Dual | 52 | 48 + 4 | model dependent |
| MS320-48, 48P, 48LP, 48FP | Jaguar Dual | 52 | 48 + 4 | model dependent |
| MS42 / MS42P | Jaguar Dual | 52 | 48 + 4 | 0 / 48 |

One platform-agnostic image carries the complete common, Luton26, Jaguar1, and Jaguar Dual kernel-object matrix plus every additional donor `.ko`. Early boot derives `/run/postmerkos/boardinfo` from the board EEPROM and loads only the matching family. Unknown identity or an unsupported exact model fails closed for model-specific hardware actions.

Untested artifacts require explicit acknowledgement. Known-incompatible artifacts are rejected. Keep direct SPI recovery available while validating a new exact model, and report boot, management, forwarding, PoE, LED, and update results separately.

MX80 remains a separate PowerPC build target. MX84 assets remain incomplete and are not part of the VCore-III image.


## Pre-kernel recovery payloads

Release artifacts include separate Luton26 and Jaguar-class recovery payloads.
The host and target both require the exact model to appear in the selected
payload descriptor and release manifest. Jaguar Dual models use the
Jaguar-class SPI software-mode implementation but remain subject to exact-model
manifest status. `verify` is local-only, `dry-run` performs target preflight
without erase/program commands, and `flash` requires the target nonce challenge.
