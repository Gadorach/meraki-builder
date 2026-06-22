#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PROFILE="$ROOT/overlay/usr/sbin/postmerkos-board-profile"
INIT="$ROOT/overlay/etc/init.d/S11poe"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
for model in MS220-8P MS22P MS220-24P MS320-24P MS220-48P MS220-48LP MS220-48FP MS320-48P MS320-48LP MS320-48FP MS42P; do
  case "$model" in
    MS220-8P) a=7; b=12 ;;
    MS22P|MS220-24P) a=82; b=81 ;;
    MS320-24P) a=9; b=8 ;;
    MS42P) a=41; b=8 ;;
    *) a=41; b=40 ;;
  esac
  run="$TMP/$model"; mkdir -p "$run/gpio/gpio$a" "$run/gpio/gpio$b"
  : >"$run/gpio/export"; : >"$run/gpio/gpio$a/direction"; : >"$run/gpio/gpio$a/value"
  : >"$run/gpio/gpio$b/direction"; : >"$run/gpio/gpio$b/value"
  "$PROFILE" "$model" >"$run/boardinfo"
  printf 'IDENTITY_EXACT=1\n' >>"$run/boardinfo"
  POSTMERKOS_BOARDINFO="$run/boardinfo" POSTMERKOS_POE_INIT_STATUS="$run/status" \
    POSTMERKOS_GPIO_ROOT="$run/gpio" "$INIT" start >"$run/out"
  grep -q "postmerkOS PoE: PASS model=$model" "$run/out"
  grep -q '^state=ready$' "$run/status"
  [ "$(cat "$run/gpio/gpio$a/value")" = 0 ]
  [ "$(cat "$run/gpio/gpio$b/value")" = 1 ]
done
# Non-PoE exact profiles remain write-disabled.
run="$TMP/MS42"; mkdir -p "$run/gpio"; : >"$run/gpio/export"
"$PROFILE" MS42 >"$run/boardinfo"; printf 'IDENTITY_EXACT=1\n' >>"$run/boardinfo"
POSTMERKOS_BOARDINFO="$run/boardinfo" POSTMERKOS_POE_INIT_STATUS="$run/status" \
  POSTMERKOS_GPIO_ROOT="$run/gpio" "$INIT" start >"$run/out"
grep -q 'reason=not-poe-capable' "$run/out"
[ ! -s "$run/gpio/export" ]
printf '%s\n' 'PoE init verified-model write tests passed'
