# WebSocket build-cache and local-socket correction — 2026-06-20

## Observed failure

A web-enabled firmware image served the browser UI but reported:

```text
core: enabled
unix-socket: enabled
websocket: disabled
```

TCP port 4001 was absent, the browser closed with code 1006, and `pmc` intermittently reported `postmerkosctl session failed with status 141`.

## Root causes

Buildroot reused a completed local `configd-0.2` package after the UI feature selection and local package source changed. Because the package version and local source location were unchanged, Buildroot did not automatically invalidate the package stamps. This allowed a web UI to be packaged with a previously compiled WebSocket-disabled daemon.

The local Unix-socket request path also wrote the JSON body and newline separately, while configd performed a single read and closed after replying. The second client write could therefore hit a closed socket and terminate `postmerkosctl` with SIGPIPE (`128 + 13 = 141`). The server had the same unhandled-signal exposure while replying to disconnected clients.

## Corrections

- Track the last successfully built UI mode and perform a full Buildroot clean when the mode changes or when an older output tree has no mode stamp.
- Fingerprint synchronized configd source plus its Buildroot feature selection and run `configd-dirclean` whenever that fingerprint changes.
- Forward `CLEAN_BUILDROOT` through every top-level Distrobox transition.
- Mark web images explicitly and make `S15configd` reject a WebSocket-disabled binary when the marker is present.
- Validate the final SquashFS daemon for the enabled feature string and a `libwebsockets` dynamic dependency.
- Use a shared newline-framed socket I/O implementation for configd and `postmerkosctl`, including complete-line reads, bounded timeouts, checked writes, and SIGPIPE suppression.
- Reconcile service start/stop requests idempotently so the late policy pass does not attempt to start already-running chrony, uhttpd, or Dropbear.

## Build behavior

A normal `make web` now detects an old untracked output tree and cleans it once. Subsequent builds reuse Buildroot output while still rebuilding configd whenever its source or WebSocket feature selection changes.

An explicit clean build remains available:

```sh
CLEAN_BUILDROOT=1 make web
```

The value is preserved when the build transitions into Ubuntu Distrobox.
