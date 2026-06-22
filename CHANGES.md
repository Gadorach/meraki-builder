# Revision 3: deterministic configd rebuild and pipefail-safe validation

## meraki-builder

- Fix a false WebSocket-disabled result in `validate-image.sh`. The scripts run
  with `set -o pipefail`; `strings ... | grep -q` could find the valid marker,
  exit early, cause `strings` to receive SIGPIPE, and make the successful probe
  return status 141. Binary feature and dependency probes now consume all input.
- Rebuild the fixed-version local `configd` package on every rootfs build after
  package synchronization. The build performs `configd-dirclean`, removes stale
  installed tools, runs the explicit Buildroot `configd` target, and validates
  the freshly installed binary before filesystem finalization.
- Keep the source/config fingerprint as diagnostic state, but no longer trust it
  as the sole cache-invalidation mechanism after a failed image validation.
- Report the actual discovered WebSocket feature marker when final validation
  fails, instead of always describing the binary as disabled.
- Add a regression test with enough trailing binary strings to reproduce the
  former SIGPIPE/141 validator failure.

## postmerkos-ui

No source changes were required.

---

# WebSocket build-cache, local socket, and service-policy corrections

## meraki-builder

- Preserve configd's required feature macros when Buildroot supplies `CPPFLAGS`
  as a command-line variable. The package Makefile now uses `override CPPFLAGS +=`
  for the POSIX/default-source flags and `CONFIGD_ENABLE_WEBSOCKET`, preventing a
  binary that links `websocket.c`/libwebsockets but reports and behaves as
  WebSocket-disabled.
- Extend the Buildroot contract test to execute a dry-run configd build with
  command-line `CPPFLAGS` and verify that all required package macros survive.
- Detect and clean reused Buildroot output when switching between base and web
  images, including one-time cleanup of output created before UI-mode tracking.
- Fingerprint the synchronized configd package and its WebSocket feature choice;
  invalidate `configd-0.2` automatically when either changes.
- Forward `CLEAN_BUILDROOT` through top-level Distrobox execution.
- Mark web images explicitly and fail configd startup when a web image contains a
  WebSocket-disabled daemon.
- Validate the final configd binary for `websocket: enabled` and a
  `libwebsockets` dependency before publishing a web artifact.
- Replace split local-socket writes and one-shot reads with shared bounded,
  newline-framed I/O; suppress SIGPIPE in configd and `postmerkosctl`.
- Make service start/stop reconciliation idempotent to avoid duplicate chrony,
  uhttpd, and Dropbear start failures during the late service-policy pass.
- Add host tests for fragmented socket traffic, closed-peer writes, oversized
  frames, WebSocket-required init failure, and build-cache contracts.

## postmerkos-ui

No source changes were required. The current UI already requests the `configd-ws`
subprotocol and protocol-2 `hello` expected by the backend.

---

# UART recovery ACK, negotiation, and receive-window corrections

## meraki-builder

- Keep `UI_REF=ms42p-dev` as the postmerkos-ui default and correct all active
  documentation and source-contract tests that incorrectly claimed `main`.
- Replace the default broad UART divisor search with one descending pass over
  921600, 460800, and 230400 baud, stopping at the first bidirectional pass.
- Retain the former broad scan and midpoint refinement behind
  `--diagnostic-baud-scan`.
- Resynchronize compact ACK parsing by scanning for the binary ACK magic instead
  of repeatedly consuming fixed 28-byte blocks after corruption.
- Wait for a failed feature test to reach a terminal target state before another
  command is sent, and fail the session when fundamental raw ACK qualification
  cannot be cleanly recovered.
- Default framed transport to a one-frame production window because the VCore-III
  recovery UART has no RTS/CTS flow control and cannot safely absorb a continuous
  multi-frame burst while CRC/decode/copy work is performed between frames.
- Retain larger windows behind `--diagnostic-window-scan`, with `tcdrain()` and a
  conservative inter-frame idle guard.
- Preserve textual `FEATURE-FAIL` output consumed during compact-ACK scanning so
  command-level recovery can resynchronize cleanly.
- Return the host UART to 115200 immediately after `PMOSREC REBOOT NOW`, clear
  stale negotiated-rate bytes, and monitor several early loader/kernel markers.
- Classify serial input before echoing it: only complete ASCII protocol lines are
  written to the operator console, while deterministic baud-test and compact-ACK
  binary bytes stay silent. This prevents random XOFF and terminal escape bytes
  from freezing or corrupting live progress output while the transfer continues.
- Add regression tests for CRLF-expanded ACKs, missing bytes, conservative baud
  ordering, diagnostic refinement, safe window selection, guarded multi-frame
  output, reboot baud handoff, binary-safe console output, terminal-status
  preservation, and branch-selection policy.

## meraki-redboot

- Separate human-readable CRLF console output from byte-transparent UART output.
- Send compact ACK records and target-to-host deterministic test streams through
  the raw writer so binary `0x0a` bytes are never expanded.
- Correct `ERROR_PPM` calculation without 32-bit multiplication saturation.
- Drain the remainder of a failed feature-test burst before returning to the
  command parser, preventing binary frame data from becoming `UNKNOWN-COMMAND`
  floods.
- Make malformed headers retryable for the safe one-frame window, bound target
  frame timeout below the host ACK deadline, and report structured frame error,
  expected sequence, UART status, and drained-byte data.
- Add source-contract tests for binary output and framed-stream recovery paths.

## postmerkos-ui

No source changes. `meraki-builder` continues to consume `ms42p-dev` by default.
