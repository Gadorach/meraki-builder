#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
IDENTITY="$ROOT/overlay/usr/sbin/postmerkos-board-identity"
PROFILE="$ROOT/overlay/usr/sbin/postmerkos-board-profile"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cat > "$TMP/board_data" <<'BD'
#!/bin/sh
case "$1" in
 product_number) echo 600-32010 ;;
 model_exact) echo MS42P ;;
 serial) echo Q2XX-ABCD-1234 ;;
 mac) echo 00:18:0a:12:34:56 ;;
 *) exit 1 ;;
esac
BD
chmod +x "$TMP/board_data"
POSTMERKOS_RUN_DIR="$TMP/run" POSTMERKOS_BOARDINFO="$TMP/run/boardinfo" \
POSTMERKOS_BOARD_DATA="$TMP/board_data" POSTMERKOS_BOARD_PROFILE="$PROFILE" \
  "$IDENTITY" >/dev/null
[ "$(stat -c %a "$TMP/run/boardinfo")" = 444 ]
grep -q '^MODEL=MS42P$' "$TMP/run/boardinfo"
grep -q '^MODULE_FAMILY=jaguar_dual$' "$TMP/run/boardinfo"
grep -q '^LOGICAL_PORTS=52$' "$TMP/run/boardinfo"
grep -q '^IDENTITY_EXACT=1$' "$TMP/run/boardinfo"
# Unknown identity must remove any stale record and fail closed.
cat > "$TMP/board_data" <<'BD'
#!/bin/sh
exit 1
BD
chmod +x "$TMP/board_data"
if POSTMERKOS_RUN_DIR="$TMP/run" POSTMERKOS_BOARDINFO="$TMP/run/boardinfo" \
   POSTMERKOS_BOARD_DATA="$TMP/board_data" POSTMERKOS_BOARD_PROFILE="$PROFILE" \
   "$IDENTITY" >/dev/null 2>&1; then
  echo 'identity unexpectedly accepted missing EEPROM data' >&2; exit 1
fi
[ ! -e "$TMP/run/boardinfo" ]
# A PMOSLIVE boot may supply exact model identity through the standard kernel
# command line when the physical EEPROM and flash fallback are unavailable.
printf '%s\n' 'console=ttyS0 postmerkos.live=1 postmerkos.model=MS42P root=/dev/ram0' >"$TMP/cmdline"
POSTMERKOS_RUN_DIR="$TMP/run" POSTMERKOS_BOARDINFO="$TMP/run/boardinfo" \
POSTMERKOS_BOARD_DATA="$TMP/board_data" POSTMERKOS_BOARD_PROFILE="$PROFILE" \
POSTMERKOS_PROC_CMDLINE="$TMP/cmdline" POSTMERKOS_LIVE_MARKER="$TMP/no-marker" \
  "$IDENTITY" >"$TMP/live-identity.console"
grep -q '^PMOSLIVE PLATFORM-READY MODEL=MS42P SOURCE=pmoslive-command-line$' "$TMP/live-identity.console"
grep -q '^MODEL=MS42P$' "$TMP/run/boardinfo"
grep -q '^PRODUCT_NUMBER=600-21020$' "$TMP/run/boardinfo"
grep -q '^IDENTITY_SOURCE=pmoslive-command-line$' "$TMP/run/boardinfo"
grep -q '^IDENTITY_VOLATILE=1$' "$TMP/run/boardinfo"
# All Luton26 live targets must resolve through the command-line fallback with
# their exact product number and profile.
for pair in \
  'MS22 600-20010' 'MS22P 600-20020' \
  'MS220-24 600-20030' 'MS220-24P 600-20040' \
  'MS220-8 600-28010' 'MS220-8P 600-28020'; do
  set -- $pair
  printf '%s\n' "postmerkos.live=1 postmerkos.model=$1 root=/dev/ram0" >"$TMP/cmdline"
  POSTMERKOS_RUN_DIR="$TMP/run" POSTMERKOS_BOARDINFO="$TMP/run/boardinfo" \
  POSTMERKOS_BOARD_DATA="$TMP/board_data" POSTMERKOS_BOARD_PROFILE="$PROFILE" \
  POSTMERKOS_PROC_CMDLINE="$TMP/cmdline" POSTMERKOS_LIVE_MARKER="$TMP/no-marker" \
    "$IDENTITY" >"$TMP/live-luton.console"
  grep -q "^PMOSLIVE PLATFORM-READY MODEL=$1 SOURCE=pmoslive-command-line$" "$TMP/live-luton.console"
  grep -q "^MODEL=$1$" "$TMP/run/boardinfo"
  grep -q "^PRODUCT_NUMBER=$2$" "$TMP/run/boardinfo"
done
# The command-line hint is accepted only for a live boot and only for a model
# supported by the exact profile table.
printf '%s\n' 'console=ttyS0 postmerkos.model=MS42P root=/dev/ram0' >"$TMP/cmdline"
if POSTMERKOS_RUN_DIR="$TMP/run" POSTMERKOS_BOARDINFO="$TMP/run/boardinfo" \
   POSTMERKOS_BOARD_DATA="$TMP/board_data" POSTMERKOS_BOARD_PROFILE="$PROFILE" \
   POSTMERKOS_PROC_CMDLINE="$TMP/cmdline" POSTMERKOS_LIVE_MARKER="$TMP/no-marker" \
   "$IDENTITY" >/dev/null 2>&1; then
  echo 'identity unexpectedly accepted a non-live model hint' >&2; exit 1
fi
[ ! -e "$TMP/run/boardinfo" ]
printf '%s\n' 'postmerkos.live=1 postmerkos.model=NOT-A-SWITCH' >"$TMP/cmdline"
if POSTMERKOS_RUN_DIR="$TMP/run" POSTMERKOS_BOARDINFO="$TMP/run/boardinfo" \
   POSTMERKOS_BOARD_DATA="$TMP/board_data" POSTMERKOS_BOARD_PROFILE="$PROFILE" \
   POSTMERKOS_PROC_CMDLINE="$TMP/cmdline" POSTMERKOS_LIVE_MARKER="$TMP/no-marker" \
   "$IDENTITY" >/dev/null 2>&1; then
  echo 'identity unexpectedly accepted an unsupported live model hint' >&2; exit 1
fi
[ ! -e "$TMP/run/boardinfo" ]
# Hardware-verified PoE profiles must expose their exact GPIO pair; non-PoE
# profiles remain write-disabled.
check_poe() {
  model=$1 a=$2 b=$3 output=$("$PROFILE" "$model")
  printf '%s\n' "$output" | grep -q '^POE_CAPABLE=1$'
  printf '%s\n' "$output" | grep -q '^POE_GPIO_VERIFIED=1$'
  printf '%s\n' "$output" | grep -q "^POE_GPIO_A=$a$"
  printf '%s\n' "$output" | grep -q "^POE_GPIO_B=$b$"
}
check_poe MS220-8P 7 12
check_poe MS22P 82 81
check_poe MS220-24P 82 81
check_poe MS320-24P 9 8
for model in MS220-48P MS220-48LP MS220-48FP MS320-48P MS320-48LP MS320-48FP; do check_poe "$model" 41 40; done
check_poe MS42P 41 8
for model in MS22 MS220-8 MS220-24 MS220-48 MS320-24 MS320-48 MS42; do
  output=$("$PROFILE" "$model")
  printf '%s\n' "$output" | grep -q '^POE_CAPABLE=0$'
  printf '%s\n' "$output" | grep -q '^POE_GPIO_VERIFIED=0$'
done
printf '%s\n' 'board identity tests passed'
