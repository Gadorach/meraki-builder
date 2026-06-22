# WebSocket and management-path fix report

Date: 2026-06-20

## Hardware evidence

The affected firmware reported:

```text
core: enabled
unix-socket: enabled
websocket: disabled
```

No listener existed on TCP port 4001 and `/run/postmerkos/websocket.log` was absent. This confirms that the web image contained a WebSocket-disabled `configd`; it was not a runtime WebSocket handshake failure. The repeated local process/socket health passes also do not support a configd reboot loop.

## Corrected defects

1. Buildroot now detects changes between base and web image modes and cleans stale output when necessary.
2. The synchronized local `configd` package and its feature selection are fingerprinted. A changed fingerprint triggers `configd-dirclean` so fixed Buildroot package stamps cannot preserve an older daemon.
3. `CLEAN_BUILDROOT` is forwarded through top-level and direct Distrobox transitions.
4. Web images carry `/etc/postmerkos/features/web-ui`. Their configd init script fails closed when the daemon reports WebSocket support disabled.
5. Final web-image validation requires both `websocket: enabled` and a `libwebsockets` dynamic dependency in `/bin/configd`.
6. Configd and `postmerkosctl` now share bounded newline-framed socket I/O. Requests are sent atomically, responses are read to a complete frame, all writes are checked, and disconnected peers cannot terminate either process through SIGPIPE.
7. Service policy reconciliation avoids duplicate start/stop operations for uhttpd, chronyd, and Dropbear.
8. Regression tests cover fragmented frames, oversized frames, closed peers, fail-closed WebSocket startup, service idempotence, and Buildroot cache contracts.


## Follow-up compile-flag correction

A complete target build exposed a second issue that the original source review did
not catch. Buildroot invokes the configd package Makefile with `CPPFLAGS` assigned
on the command line. GNU make gives command-line variable assignments precedence
over ordinary Makefile `+=` assignments, so both
`-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` and
`-DCONFIGD_ENABLE_WEBSOCKET=1` were silently omitted.

The result was internally contradictory: `websocket.c` was compiled and
libwebsockets was linked, but `main.c` compiled its WebSocket initialization and
feature reporting out. The final-image validator correctly rejected that binary as
WebSocket-disabled.

The package Makefile now uses:

```make
override CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
...
override CPPFLAGS += -DCONFIGD_ENABLE_WEBSOCKET=1
```

The build-cache contract test now performs a real GNU make dry run with
Buildroot-style command-line `CPPFLAGS` and verifies that all package macros remain
in the configd compiler command.

## Repository scope

- `meraki-builder`: changed.
- `postmerkos-ui`: reviewed; no change required. The UI already uses subprotocol `configd-ws` and protocol-2 `hello`, matching the current backend.
- `meraki-redboot`: not involved in this fault and unchanged.

## Validation completed

- Full configd host test suite passed.
- Configd built successfully with WebSocket disabled under `-Wall -Wextra -Werror`.
- Socket framing and SIGPIPE regression tests passed.
- Configd supervisor and WebSocket-required init tests passed.
- Build cache/image contract tests passed, including command-line `CPPFLAGS` propagation.
- UI/configd contract passed for all 28 UI request methods.
- Board identity, PoE initialization, and postmerkos-hardware host tests passed.
- Documentation link validation passed.

A complete Buildroot web firmware build was not performed in this review environment. The corrected final-image validator will reject a web artifact unless its actual target `configd` is WebSocket-enabled and linked to libwebsockets.

## First rebuild

Use an explicit clean rebuild for the first corrected image:

```sh
CLEAN_BUILDROOT=1 make web
```

The corrected scripts preserve `CLEAN_BUILDROOT=1` when entering Ubuntu Distrobox.

## Post-flash verification

```sh
/bin/configd --features
grep -i ':0FA1' /proc/net/tcp /proc/net/tcp6
postmerkosctl management-health
cat /run/postmerkos/websocket.log
```

Expected feature result:

```text
core: enabled
unix-socket: enabled
websocket: enabled
websocket-port: 4001
websocket-protocol: configd-ws
```

## Revision 3: validator SIGPIPE and poisoned package cache

A subsequent real build exposed two remaining issues:

1. `validate-image.sh` used `strings ... | grep -q` and `readelf ... | grep -q` while `common.sh` enables `set -o pipefail`. On a sufficiently large configd binary, `grep -q` exits immediately after finding the valid marker, `strings` receives SIGPIPE, and the successful check is reported as status 141. The validator therefore falsely claimed that an enabled binary was disabled. The probes now consume their complete input.
2. A failed validation could leave a synchronized configd fingerprint alongside an older installed target binary. Every rootfs build now runs `configd-dirclean`, removes installed configd tools, rebuilds the local package explicitly, and verifies its feature marker and dynamic dependency before filesystem finalization.

The target compiler command must contain `-DCONFIGD_ENABLE_WEBSOCKET=1` for web builds, and the freshly installed binary must contain `websocket: enabled` and a `libwebsockets` dynamic dependency.
