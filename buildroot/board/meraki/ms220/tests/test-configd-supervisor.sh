#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SUP="$ROOT/overlay/usr/sbin/postmerkos-configd-supervisor"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cat >"$TMP/daemon" <<'DAEMON'
#!/bin/sh
count_file=${TEST_COUNT:?}
count=0; [ -r "$count_file" ] && count=$(cat "$count_file")
count=$((count + 1)); echo "$count" >"$count_file"
printf 'exit_code=1\nreason=synthetic-failure\n' >"${CONFIGD_EXIT_FILE:?}"
exit 1
DAEMON
chmod +x "$TMP/daemon"
set +e
TEST_COUNT="$TMP/count" CONFIGD_DAEMON="$TMP/daemon" \
CONFIGD_CHILD_PIDFILE="$TMP/child.pid" CONFIGD_STOP_FILE="$TMP/stop" \
CONFIGD_EXIT_FILE="$TMP/exit" CONFIGD_CONSOLE=/dev/null \
CONFIGD_MAX_RESTARTS=2 CONFIGD_RESTART_WINDOW=60 \
  "$SUP" >"$TMP/out" 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ]
[ "$(cat "$TMP/count")" = 3 ]
grep -q 'restarting configd attempt=1/2' "$TMP/out"
grep -q 'restarting configd attempt=2/2' "$TMP/out"
grep -q 'exceeded 2 restarts' "$TMP/out"
printf '%s\n' 'configd bounded supervisor tests passed'
