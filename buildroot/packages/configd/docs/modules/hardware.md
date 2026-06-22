# hardware module

`hardware.c` detects model, total ports, PoE-capable copper ports, and available PD690xx controllers.

## Detection inputs

- `CONFIGD_MODEL` test override.
- `/run/postmerkos/boardinfo` (`CONFIGD_BOARDINFO` override).
- `/tmp/NUM_PORTS` (`CONFIGD_NUM_PORTS` override).
- PD690xx I2C presence; skipped in tests with `CONFIGD_SKIP_I2C=1`.

`poe_supported` means the detected model has PoE-capable ports. `poe_available` means at least one controller is currently responding. Configuration validity is based on hardware support, not temporary controller availability, so desired state survives a controller probe failure.

SFP/SFP+ ports never accept PoE configuration.
