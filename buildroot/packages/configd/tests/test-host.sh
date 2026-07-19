#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PKG=$(CDPATH= cd -- "$HERE/.." && pwd)
OUT=${TMPDIR:-/tmp}/configd-test-network
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$PKG" -I"$PKG/../postmerkos" -I"$PKG/../pd690xx" \
  $(pkg-config --cflags json-c) \
  -o "$OUT" "$HERE/test_network.c" "$PKG/network.c" "$PKG/result.c" \
  "$PKG/../postmerkos/libpostmerkos.c" $(pkg-config --libs json-c)
"$OUT"

CONSOLE_OUT=${TMPDIR:-/tmp}/configd-test-console
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$PKG" -I"$PKG/../postmerkos" -I"$PKG/../pd690xx" \
  $(pkg-config --cflags json-c) \
  -o "$CONSOLE_OUT" "$HERE/test_console_cli.c" "$PKG/console_cli.c" \
  $(pkg-config --libs json-c)
"$CONSOLE_OUT"

ROLE_TMP=$(mktemp -d)
trap 'rm -rf "$ROLE_TMP"' EXIT HUP INT TERM
cat >"$ROLE_TMP/passwd" <<'EOF_PASSWD'
root:x:0:0:root:/root:/bin/sh
alice:x:1001:1001:Alice:/home/alice:/bin/sh
bob:x:1002:1002:Bob:/home/bob:/bin/sh
carol:x:1003:1003:Carol:/home/carol:/bin/sh
nobody:x:1004:1004:Nobody:/home/nobody:/bin/sh
EOF_PASSWD
cat >"$ROLE_TMP/group" <<'EOF_GROUP'
root:x:0:root
postmerkos-admin:x:2001:alice
postmerkos-operator:x:2002:bob
postmerkos-viewer:x:2003:carol
EOF_GROUP
ROLE_OUT=${TMPDIR:-/tmp}/configd-test-roles
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$PKG" $(pkg-config --cflags json-c) \
  -o "$ROLE_OUT" "$HERE/test_roles.c" "$PKG/roles.c" \
  $(pkg-config --libs json-c)
"$ROLE_OUT" "$ROLE_TMP/passwd" "$ROLE_TMP/group"

AUTH_OUT=${TMPDIR:-/tmp}/configd-test-auth
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$PKG" $(pkg-config --cflags json-c) \
  -o "$AUTH_OUT" "$HERE/test_auth.c" "$PKG/auth.c" "$PKG/roles.c" "$PKG/system_info.c" \
  $(pkg-config --libs json-c) -lcrypt
"$AUTH_OUT" "$ROLE_TMP/passwd" "$ROLE_TMP/group" "$ROLE_TMP/shadow"

SOCKET_OUT=${TMPDIR:-/tmp}/configd-test-socket-io
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" -o "$SOCKET_OUT" "$HERE/test_socket_io.c" "$PKG/socket_io.c"
"$SOCKET_OUT"

SERVICE_OUT=${TMPDIR:-/tmp}/configd-test-service-ops
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" $(pkg-config --cflags json-c) -o "$SERVICE_OUT" \
  "$HERE/test_service_ops.c" "$PKG/service_ops.c" $(pkg-config --libs json-c)
"$SERVICE_OUT"

RELEASE_OUT=${TMPDIR:-/tmp}/configd-test-release
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"$PKG" $(pkg-config --cflags json-c) \
  -o "$RELEASE_OUT" "$HERE/test_release.c" "$PKG/release.c" \
  $(pkg-config --libs json-c)
"$RELEASE_OUT"
SSH_KEYS_OUT=${TMPDIR:-/tmp}/configd-test-ssh-keys
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" -I"$PKG/../postmerkos" -I"$PKG/../pd690xx" \
  $(pkg-config --cflags json-c) -o "$SSH_KEYS_OUT" \
  "$HERE/test_ssh_keys.c" "$PKG/ssh_keys.c" "$PKG/config_file.c" \
  "$PKG/json_util.c" "$PKG/../postmerkos/libpostmerkos.c" \
  $(pkg-config --libs json-c)
"$SSH_KEYS_OUT"

HEALTH_OUT=${TMPDIR:-/tmp}/postmerkosctl-test-health
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" $(pkg-config --cflags json-c) -o "$HEALTH_OUT" \
  "$PKG/postmerkosctl.c" "$PKG/socket_io.c" $(pkg-config --libs json-c)
"$HERE/test_management_health.py" "$HEALTH_OUT"

SESSION_OUT=${TMPDIR:-/tmp}/configd-test-session
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" -o "$SESSION_OUT" "$HERE/test_session.c" "$PKG/session.c"
"$SESSION_OUT"

BOOT_OUT=${TMPDIR:-/tmp}/configd-test-bootstrap
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" -I"$PKG/../postmerkos" -I"$PKG/../pd690xx" \
  $(pkg-config --cflags json-c) -o "$BOOT_OUT" \
  "$PKG/main.c" "$PKG/click_port.c" "$PKG/click_global.c" "$PKG/status.c" \
  "$PKG/json_util.c" "$PKG/config_file.c" "$PKG/config_apply.c" \
  "$PKG/hardware.c" "$PKG/network.c" "$PKG/result.c" "$PKG/validation.c" \
  "$PKG/console_cli.c" "$PKG/release.c" "$PKG/roles.c" "$PKG/local_socket.c" \
  "$PKG/socket_io.c" "$PKG/service_ops.c" "$PKG/time_ops.c" "$PKG/system_info.c" "$PKG/system_identity.c" "$PKG/system_ops.c" "$PKG/port_clone.c" \
  "$PKG/compatibility.c" "$PKG/auth.c" "$PKG/ssh_keys.c" "$PKG/session.c" "$PKG/portstats.c" "$PKG/metrics.c" "$PKG/telemetry.c" \
  "$PKG/websocket_disabled.c" \
  "$PKG/../pd690xx/libpd690xx.c" "$PKG/../postmerkos/libpostmerkos.c" \
  $(pkg-config --libs json-c) -lcrypt
BOOT_TMP=$(mktemp -d)
cat >"$BOOT_TMP/switch.json" <<'EOF_BOOT'
{"network":{"ipv4":{"mode":"static","address":"192.0.2.10/24","gateway":"192.0.2.1","mtu":1500}}}
EOF_BOOT
POSTMERKOS_NETWORK_BOOTSTRAP_RECORD="$BOOT_TMP/result.json" \
  "$BOOT_OUT" -N --boot-output -W 0 -d -c "$BOOT_TMP/switch.json" \
  >"$BOOT_TMP/out" 2>"$BOOT_TMP/err"
grep -q '^postmerkOS network: PASS source=static address=192.0.2.10/24 ' "$BOOT_TMP/out"
! grep -q '^[[:space:]]*{' "$BOOT_TMP/out"
grep -q '"message":"Network bootstrap complete"' "$BOOT_TMP/result.json"
[ ! -s "$BOOT_TMP/err" ]
rm -rf "$BOOT_TMP"
printf '%s\n' 'configd condensed bootstrap output test passed'


IDENTITY_OUT=${TMPDIR:-/tmp}/configd-test-system-identity
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" $(pkg-config --cflags json-c) -o "$IDENTITY_OUT" \
  "$HERE/test_system_identity.c" "$PKG/system_identity.c" $(pkg-config --libs json-c)
"$IDENTITY_OUT"

HARDWARE_META_OUT=${TMPDIR:-/tmp}/configd-test-hardware-metadata
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" -I"$PKG/../pd690xx" $(pkg-config --cflags json-c) -o "$HARDWARE_META_OUT" \
  "$HERE/test_hardware_metadata.c" "$PKG/hardware.c" "$PKG/release.c" "$PKG/../pd690xx/libpd690xx.c" \
  $(pkg-config --libs json-c)
"$HARDWARE_META_OUT"

SYSTEM_INFO_OUT=${TMPDIR:-/tmp}/configd-test-system-info
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" $(pkg-config --cflags json-c) -o "$SYSTEM_INFO_OUT" \
  "$HERE/test_system_info.c" "$PKG/system_info.c" $(pkg-config --libs json-c)
"$SYSTEM_INFO_OUT"

TIME_OUT=${TMPDIR:-/tmp}/configd-test-time-ops
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" $(pkg-config --cflags json-c) -o "$TIME_OUT" \
  "$HERE/test_time_ops.c" "$PKG/time_ops.c" "$PKG/service_ops.c" $(pkg-config --libs json-c)
"$TIME_OUT"

OUT=${TMPDIR:-/tmp}/configd-test-portstats
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" \
  -o "$OUT" "$HERE/test_portstats.c" "$PKG/portstats.c"
"$OUT"

OUT=${TMPDIR:-/tmp}/configd-test-metrics
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" \
  -o "$OUT" "$HERE/test_metrics.c" "$PKG/metrics.c" "$PKG/portstats.c"
"$OUT"

OUT=${TMPDIR:-/tmp}/configd-test-metrics-server
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" \
  -o "$OUT" "$HERE/test_metrics_server.c" "$PKG/metrics.c" "$PKG/portstats.c"
"$OUT"

OUT=${TMPDIR:-/tmp}/configd-test-telemetry
cc -std=gnu11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$PKG" -I"$PKG/../postmerkos" -I"$PKG/../pd690xx" $(pkg-config --cflags json-c) \
  -o "$OUT" "$HERE/test_telemetry.c" "$PKG/telemetry.c" "$PKG/metrics.c" "$PKG/portstats.c" "$PKG/result.c" \
  "$PKG/../postmerkos/libpostmerkos.c" "$PKG/../pd690xx/libpd690xx.c" \
  $(pkg-config --libs json-c)
"$OUT"

"$HERE/test_management_plane_contract.py"
