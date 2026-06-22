#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/run" "$TMP/click/sw0_ctrl" "$TMP/sys" "$TMP/dev/input" "$TMP/profiles"
cp "$ROOT/files/hardware-controls-default.conf" "$TMP/profiles/default.conf"
cp "$ROOT"/files/profiles/*.conf "$TMP/profiles/"
for profile in "$TMP"/profiles/*.conf; do sed -i "s#=/click/#=$TMP/click/#g" "$profile"; done
: > "$TMP/click/sw0_ctrl/power_led_green"
: > "$TMP/click/sw0_ctrl/power_led_orange"
: > "$TMP/click/sw0_ctrl/led_mode"
: > "$TMP/click/sw0_ctrl/poe_led_state"
chmod 666 "$TMP/click/sw0_ctrl/"*
run_probe() {
  model=$1
  printf 'MODEL=%s\nIDENTITY_EXACT=1\n' "$model" > "$TMP/boardinfo"
  POSTMERKOS_RUN_DIR="$TMP/run" \
  POSTMERKOS_HARDWARE_PROFILE_DIR="$TMP/profiles" \
  POSTMERKOS_BOARDINFO="$TMP/boardinfo" \
  POSTMERKOS_CLICK_ROOT="$TMP/click" \
  POSTMERKOS_SYS_ROOT="$TMP/sys" \
  POSTMERKOS_PROC_INPUT="$TMP/proc-input" \
  POSTMERKOS_DEV_INPUT="$TMP/dev/input" \
    "$ROOT/files/postmerkos-hwprobe" refresh
}
run_probe MS220-8P
python3 - "$TMP/run/hardware-controls.json" <<'PY'
import json, sys
p=json.load(open(sys.argv[1]))
assert p['schema_version'] == 2
assert p['profile_exact'] is True
assert p['leds']['status']['backend'] == 'click-dual-state'
assert p['leds']['status']['available'] is True
assert p['leds']['status']['green']['protocol'] == 'named-state'
assert p['leds']['status']['orange']['protocol'] == 'named-state'
assert p['leds']['panel']['automatic_use'] is False
assert p['leds']['ports']['backend'] == 'click-poe-led-state'
assert p['leds']['ports']['available'] is True
assert p['reset_button']['destructive_enabled'] is False
PY
run_probe MS220-24P
python3 - "$TMP/run/hardware-controls.json" <<'PY'
import json, sys
p=json.load(open(sys.argv[1]))
assert p['profile_exact'] is True
assert p['leds']['ports']['available'] is False
assert p['reset_button']['destructive_enabled'] is False
PY
printf 'MODEL=UNKNOWN\nIDENTITY_EXACT=1\n' > "$TMP/boardinfo"
POSTMERKOS_RUN_DIR="$TMP/run" \
POSTMERKOS_HARDWARE_PROFILE_DIR="$TMP/profiles" \
POSTMERKOS_BOARDINFO="$TMP/boardinfo" \
POSTMERKOS_CLICK_ROOT="$TMP/click" \
POSTMERKOS_SYS_ROOT="$TMP/sys" \
POSTMERKOS_PROC_INPUT="$TMP/proc-input" \
POSTMERKOS_DEV_INPUT="$TMP/dev/input" \
  "$ROOT/files/postmerkos-hwprobe" refresh
python3 - "$TMP/run/hardware-controls.json" <<'PY'
import json, sys
p=json.load(open(sys.argv[1]))
assert p['profile_exact'] is False
assert p['leds']['status']['available'] is False
assert p['leds']['ports']['available'] is False
assert p['reset_button']['destructive_enabled'] is False
PY
# Only the two static-proven 8-port profiles may expose poe_led_state.
for profile in "$ROOT"/files/profiles/*.conf; do
  case "${profile##*/}" in
    MS220-8.conf|MS220-8P.conf) grep -q '^PORT_LED_BACKEND=click-poe-led-state$' "$profile" ;;
    *) ! grep -q '^PORT_LED_BACKEND=click-poe-led-state$' "$profile" ;;
  esac
done
printf '%s\n' 'postmerkos-hardware host tests passed'
