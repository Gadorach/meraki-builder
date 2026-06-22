# Service management

Persistent service policy is stored in `/config/postmerkos/services.json`.

Administrators can view status, start, stop, restart, configure, enable at boot, disable at boot, and read recent RAM logs for management services such as SSH, the optional web frontend, and chrony.

Critical forwarding, network, configd-core, and Click initialization services cannot be permanently disabled through normal management interfaces.


Configd is supervised as a critical management service. Its readiness check performs a real local-session request and WebSocket protocol-2 `hello`, not merely a TCP-listen test. Diagnostics are available with `postmerkosctl management-health` and through the serial `status` and `logs` commands.
