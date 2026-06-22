# UART recovery package-header handoff correction

## Symptom

The external recovery payload uploaded and executed successfully, then emitted:

```text
PMOSREC READY 2 ...
PMOSREC DESCRIPTOR ...
PMOSREC RESULT ERROR PACKAGE-HEADER-TIMEOUT
```

No firmware image bytes had been transferred and no flash erase had started.

## Cause

The host returned from recovery-stage detection as soon as it parsed
`PMOSREC READY 2`. The recovery stage immediately prints a descriptor line after
READY using a polling UART. If the host begins the 124-byte binary package header
while that text is still transmitting, the target is not yet polling RX and its
small UART FIFO can overflow. The recovery stage then sees an incomplete package
header and reaches its receive deadline.

## Contract

The complete `PMOSREC DESCRIPTOR ...;END` line is the host-to-target handoff.
The flasher now waits for its terminating newline and validates:

- SoC family (`luton26` or `jaguar1`);
- family ID;
- SPI software-mode register;
- protocol version 2.

Only after that validation does it transmit the binary package header.

Future recovery payload builds increase the initial package-header grace period
from 3 seconds to 30 seconds. Per-frame interbyte and object-transfer limits are
unchanged. Existing corrected external recovery payloads remain wire-compatible;
updating the flasher alone is sufficient for an immediate retry.
