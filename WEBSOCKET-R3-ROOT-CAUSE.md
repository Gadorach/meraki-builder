# WebSocket build failure: revision 3 root cause

The reported `Web image contains a WebSocket-disabled configd binary` error was
not sufficient proof that the binary was disabled. Two issues combined:

1. `common.sh` enables `set -Eeuo pipefail`, while the image validator used
   `strings configd | grep -q ...`. Once `grep -q` found `websocket: enabled`, it
   exited without consuming the remaining output. `strings` then received
   SIGPIPE and returned 141, so `pipefail` marked the entire successful probe as
   failed.
2. A failed validation could leave a current configd fingerprint beside an older
   installed target binary. Later builds could finalize the rootfs without
   invoking the local package build again.

Revision 3 removes `grep -q` from producer pipelines and explicitly rebuilds and
verifies configd before every rootfs finalization. A web build must now show:

```text
==> Rebuilding configd from synchronized local sources
>>> configd 0.2 Extracting
>>> configd 0.2 Building
>>> configd 0.2 Installing to target
```

The configd compiler command must include `-DCONFIGD_ENABLE_WEBSOCKET=1`.
