# Serial console

Opens a manual console at the postmerkOS hardware settings: 115200 baud, 8 data
bits, no parity, one stop bit, and XON/XOFF flow control. It can add the current
user to the owning serial-device group or use `sudo` for the current run.

Current firmware may first show the serial `pmc:` prompt or the postmerkOS
management menu. Type `shell` at either prompt when an unrestricted shell is
needed and the current role permits it.
