#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
FLASHER=$(cd -- "$SCRIPT_DIR/.." && pwd)/firmware-flasher.sh

run_scope_case() {
    local choice=$1 expected_scope=$2 output
    output=$(
        printf '%s\n' "$choice" |
        FLASHER_TEST_EXPECTED_SCOPE="$expected_scope" bash -Eeuo pipefail -c '
            source "$1"
            need() { :; }
            select_firmware() { SELECTED_FIRMWARE=/tmp/test-image.bin; SELECTED_TYPE=full; }
            select_operation() { OPERATION=verify; OPERATION_ARGS=(--verify-only); }
            select_overlay_policy() {
                if [[ $FLASH_SCOPE == full ]]; then OVERLAY_POLICY=image; else OVERLAY_POLICY=preserve; fi
            }
            run_ssh_mode() {
                printf "REACHED-CONTROL scope=%s operation=%s overlay=%s\n" \
                    "$FLASH_SCOPE" "$OPERATION" "$OVERLAY_POLICY"
                [[ $FLASH_SCOPE == "$FLASHER_TEST_EXPECTED_SCOPE" ]]
            }
            main --control ssh
        ' bash "$FLASHER" 2>&1
    )
    grep -Fq "REACHED-CONTROL scope=$expected_scope operation=verify" <<<"$output" || {
        printf '%s\n' "$output" >&2
        return 1
    }
}

# These are the two interactive paths that previously returned silently after
# the scope prompt because prepare_version_hint returned status 1 in modern mode.
run_scope_case 1 system
run_scope_case 2 full

# Every no-op guard called as a normal command must explicitly succeed under set -e.
bash -Eeuo pipefail -c '
    source "$1"
    MODE=modern
    FIRMWARE_VERSION=""
    prepare_version_hint

    FLASH_SCOPE=system
    OPERATION=verify
    confirm_full_flash

    OVERLAY_POLICY=preserve
    clear_expected_changed_host_key

    printf "PASS-NOOP-GUARDS\n"
' bash "$FLASHER" | grep -Fq PASS-NOOP-GUARDS

# Unexpected failures must now be visible rather than looking like a normal exit.
set +e
error_output=$(bash -Eeuo pipefail -c 'source "$1"; false; echo unreachable' bash "$FLASHER" 2>&1)
error_rc=$?
set -e
[[ $error_rc -ne 0 ]]
grep -Fq 'unexpected command failure' <<<"$error_output"

echo 'PASS: firmware-flasher interactive scope and set -e control flow'

# The normal interactive workflow must expose meraki-redboot recovery as a
# first-class upload path and force the full-image policy before dispatch.
bootloader_menu_output=$(
    printf '1\n1\n3\n1\n' |
    bash -Eeuo pipefail -c '
        source "$1"
        need() { :; }
        select_firmware() { SELECTED_FIRMWARE=/tmp/test-image.bin; SELECTED_TYPE=full; }
        run_bootloader_recovery_mode() {
            printf "REACHED-BOOTLOADER scope=%s overlay=%s path=%s operation=%s control=%s\n" \
                "$FLASH_SCOPE" "$OVERLAY_POLICY" "$BOOTLOADER_RECOVERY_PATH" \
                "$OPERATION" "$CONTROL_PATH"
        }
        main
    ' bash "$FLASHER" 2>&1
)
grep -Fq '3) meraki-redboot UART recovery (pre-kernel full-image upload)' <<<"$bootloader_menu_output"
grep -Fq 'REACHED-BOOTLOADER scope=full overlay=image path=ram-upload operation=verify control=bootloader' \
    <<<"$bootloader_menu_output"

# --control bootloader was previously accepted by validation but never
# dispatched. It must behave as the explicit --bootloader-recovery alias.
bootloader_cli_output=$(
    bash -Eeuo pipefail -c '
        source "$1"
        need() { :; }
        select_firmware() { SELECTED_FIRMWARE=/tmp/test-image.bin; SELECTED_TYPE=full; }
        select_operation() { OPERATION=verify; OPERATION_ARGS=(--verify-only); }
        select_overlay_policy() { OVERLAY_POLICY=image; }
        run_bootloader_recovery_mode() {
            printf "REACHED-BOOTLOADER-CLI scope=%s path=%s control=%s\n" \
                "$FLASH_SCOPE" "$BOOTLOADER_RECOVERY_PATH" "$CONTROL_PATH"
        }
        main --control bootloader --transport uart
    ' bash "$FLASHER" 2>&1
)
grep -Fq 'REACHED-BOOTLOADER-CLI scope=full path=ram-upload control=bootloader' <<<"$bootloader_cli_output"

# All three recovery entry selections must map to the expected protocol mode.
for entry in '1 ram-upload' '2 embedded' '3 auto'; do
    read -r choice expected <<<"$entry"
    actual=$(printf '%s\n' "$choice" | bash -Eeuo pipefail -c '
        source "$1"
        select_bootloader_recovery_path >/dev/null
        printf "%s" "$BOOTLOADER_RECOVERY_PATH"
    ' bash "$FLASHER")
    [[ $actual == "$expected" ]]
done

echo 'PASS: firmware-flasher interactive bootloader UART recovery selection'

# Direct preflight must not require selecting or validating a 16 MiB firmware
# artifact. It only needs the family-specific external recovery payload.
preflight_cli_output=$(
    bash -Eeuo pipefail -c '
        source "$1"
        need() { :; }
        select_firmware() { echo "UNEXPECTED-FIRMWARE-SELECTION"; return 99; }
        select_overlay_policy() { OVERLAY_POLICY=image; }
        confirm_full_flash() { :; }
        run_bootloader_recovery_mode() {
            printf "REACHED-PREFLIGHT operation=%s type=%s scope=%s control=%s scratch=%s\n" \
                "$OPERATION" "$SELECTED_TYPE" "$FLASH_SCOPE" "$CONTROL_PATH" "$PREFLIGHT_SCRATCH"
        }
        main --bootloader-preflight --target-model MS42P --preflight-scratch 0x00ff0000
    ' bash "$FLASHER" 2>&1
)
! grep -Fq UNEXPECTED-FIRMWARE-SELECTION <<<"$preflight_cli_output"
grep -Fq 'REACHED-PREFLIGHT operation=preflight type=preflight scope=full control=bootloader scratch=0x00ff0000' \
    <<<"$preflight_cli_output"

echo 'PASS: firmware-flasher direct preflight does not require a firmware image'
