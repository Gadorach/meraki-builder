# Configd

Configd is the always-running privileged management core. It owns persistent configuration, exact-model capability discovery, Click/PoE application, management-network changes, accounts and roles, service policy, firmware staging, the local Unix socket, and the optional WebSocket API.

## Transaction contract

Configuration changes are validated and staged before persistence. Required Click, PoE, network, or service operations must succeed before the new desired state is committed. On failure, configd attempts runtime rollback, preserves the previous primary configuration, and refreshes the known-good backup only from a validated committed configuration. Results distinguish applied, warning, pending, unsupported, failed, and runtime-degraded operations.

External edits are treated the same way: configd validates and applies them before accepting them. A rejected edit restores the previous runtime state, active file, and known-good backup rather than silently leaving desired and observed state divergent.

Service status reports desired policy, observed process state, and whether they are synchronized. Known models use immutable `/run/postmerkos/boardinfo`; stale port-count files cannot override an exact profile.

The console communicates through `/run/postmerkos/configd.sock`. One-shot CLI commands remain available for recovery and automation. Web builds add libwebsockets and uhttpd without changing the configuration core.

See the package [README](../../buildroot/packages/configd/README.md) and [protocol](../../buildroot/packages/configd/docs/PROTOCOL.md).

## Runtime health and supervision

`S15configd` starts configd through a bounded supervisor. Readiness requires the local Unix socket plus an actual WebSocket upgrade, `configd-ws` subprotocol selection, and protocol-2 `hello` response. Unexpected exits are recorded in `/run/postmerkos/configd.exit`, detailed daemon output remains in `/run/postmerkos/configd.log`, and WebSocket connection events remain in `/run/postmerkos/websocket.log`. The supervisor retries only a bounded number of times to avoid a silent or unbounded crash loop.

Use `postmerkosctl management-health` to test the process, local socket/session path, WebSocket upgrade, subprotocol, and `hello` exchange.

Boot-time network initialization uses `configd --network-bootstrap --boot-output`. Serial receives concise `postmerkOS network: PASS|WARN|FAIL ...` records, while the complete JSON result is retained at `/run/postmerkos/network-bootstrap.json`.
