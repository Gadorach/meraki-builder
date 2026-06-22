#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
INIT="$ROOT/overlay/etc/init.d/S15configd"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/click"
cat >"$TMP/configd" <<'DAEMON'
#!/bin/sh
[ "${1:-}" = --features ] || exit 1
printf '%s\n' 'core: enabled' 'unix-socket: enabled' 'websocket: disabled'
DAEMON
cat >"$TMP/supervisor" <<'SUPERVISOR'
#!/bin/sh
exit 99
SUPERVISOR
cat >"$TMP/ctl" <<'CTL'
#!/bin/sh
exit 1
CTL
chmod +x "$TMP/configd" "$TMP/supervisor" "$TMP/ctl"
: >"$TMP/web-ui"
set +e
CONFIGD_DAEMON="$TMP/configd" CONFIGD_SUPERVISOR="$TMP/supervisor" \
CONFIGD_CTL="$TMP/ctl" CONFIGD_CLICK_DIR="$TMP/click" \
CONFIGD_WEB_UI_MARKER="$TMP/web-ui" CONFIGD_LOGFILE="$TMP/configd.log" \
CONFIGD_SOCKET="$TMP/configd.sock" CONFIGD_CHILD_PIDFILE="$TMP/child.pid" \
CONFIGD_SUPERVISOR_PIDFILE="$TMP/supervisor.pid" CONFIGD_STOP_FILE="$TMP/stop" \
  "$INIT" start >"$TMP/out" 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ]
grep -q 'FAIL (web image contains WebSocket-disabled configd)' "$TMP/out"
grep -q '^websocket: disabled$' "$TMP/configd.log"
grep -q 'lacks required WebSocket support' "$TMP/configd.log"
printf '%s\n' 'configd WebSocket-required init test passed'
