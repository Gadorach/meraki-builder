# configd

## Purpose

Configd is the always-running privileged management core for postmerkOS. It owns desired switch state, validates changes, applies Click and PoE configuration, observes management networking, enforces roles, coordinates firmware operations, and serves the console and optional web UI.

## Runtime responsibilities

- Desired switch configuration: `/etc/switch.json`
- Service policy: `/config/postmerkos/services.json`
- Security policy: `/config/postmerkos/security.json`
- Time/NTP/DST policy: `/config/postmerkos/time.json`
- Firmware repositories: `/config/postmerkos/firmware-repositories.json`
- Update history and compatibility acknowledgements under `/config/postmerkos/`
- Local Unix socket: `/run/postmerkos/configd.sock`
- Optional authenticated WebSocket frontend on port 4001

Desired configuration is persistent state; status is observed state and may omit unavailable hardware. Invalid requests never replace the saved configuration. Required Click, PoE, network, and service operations are applied before persistence; a required failure rejects the transaction and attempts runtime rollback. Only explicitly optional or unreadable status paths become structured warnings.

## Interfaces

The local socket is used by `postmerkosctl` and the role-aware console. Peer identity is resolved once from `SO_PEERCRED`; UID 0 is always administrator, and passwd/group data is copied into owned buffers rather than retained from libc static lookup storage. Web builds additionally compile the WebSocket, authentication, terminal, and firmware-upload frontend. Both paths use the same operation handlers and capability checks.

Runtime feature discovery is available with:

```sh
configd --features
```

A web build reports the `configd-ws` subprotocol and port 4001. Its init script requires the Unix socket, a valid root session request, an actual WebSocket upgrade, `configd-ws` subprotocol selection, and a protocol-2 `hello` response before declaring configd ready.

One-shot recovery/automation examples:

```sh
configd --get-config-raw
configd --show-summary
configd --show-ports 1-12
configd --get-path ports.1.vlan.pvid
configd --set-path ports.1.enabled false
configd --set-string ports.1.name uplink
configd --validate /tmp/switch.json
configd --replace-file /tmp/switch.json
configd --network-bootstrap --network-wait 60
configd --network-bootstrap --boot-output --network-wait 60
postmerkosctl management-health
```

Responses are JSON envelopes with `ack`, `error`, or operation-specific types. An unauthenticated WebSocket may use only `hello`, `ping`, `auth`, and `logout`; all status and management operations require a resolved role. See [the protocol](docs/PROTOCOL.md).

## Configuration

Per-port state includes administrative state, name, PHY speed, flow control, EEE, storm control, VLAN, STP, and capability-dependent PoE:

```json
{
  "poe": {
    "enabled": true,
    "mode": "at",
    "policy": "normal",
    "observation_seconds": 300
  }
}
```

PoE mode is `af` or `at`; policy is `normal` or `boot-prune`. Uplink and non-PoE ports do not expose PoE state.

## Security

Linux users and the `postmerkos-admin`, `postmerkos-operator`, and `postmerkos-viewer` groups provide identity and role data. Root is always an administrator and cannot be deleted. Configd enforces capabilities for every local-socket and WebSocket operation.

Authentication uses the target system’s shadow/crypt implementation without PAM. This keeps dependencies within the 8 MiB SquashFS limit.

## Build integration

The core links JSON-C, `libpostmerkos`, and `libpd690xx`. Console-only builds compile the Unix-socket core without libwebsockets. `INCLUDE_UI=1` adds libwebsockets, authentication, terminal, firmware-upload, and browser-facing handlers.

## Tests

Run:

```sh
make -C buildroot/packages/configd ENABLE_WEBSOCKET=0
make -C buildroot/packages/configd test-host
```

The host suite includes repeated root/admin/operator/viewer lookups to catch account-storage corruption and role instability.

Module responsibilities are documented under [docs/modules](docs/modules/README.md). The Click graph is documented in [the repository architecture guide](../../../docs/architecture/click-system.md).

## Diagnostics

Boot-oriented network output is concise and the complete envelope is retained in `/run/postmerkos/network-bootstrap.json`. Daemon termination is recorded in `/run/postmerkos/configd.exit` with reason, exit status, timestamp, and uptime. `S15configd` uses a bounded supervisor; repeated failures remain visible on the hardware console and in `/run/postmerkos/configd.log`.
