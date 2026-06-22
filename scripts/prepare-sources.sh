#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

if [[ ! -d "$SWITCH_DIR/.git" ]]; then
  if bool_enabled "${AUTO_CLONE:-0}" || ask_yes_no "Kernel/OpenWrt source is missing. Clone it now?" yes; then
    clone_or_update_ref "$SWITCH_REPO_URL" "$SWITCH_DIR" "$SWITCH_REF" "switch-11-22-ms220"
  else
    die "Kernel source is required."
  fi
else
  clone_or_update_ref "$SWITCH_REPO_URL" "$SWITCH_DIR" "$SWITCH_REF" "switch-11-22-ms220"
fi

git -C "$SWITCH_DIR" rev-parse HEAD > "$ARTIFACTS_DIR/kernel-source-revision.txt"

"$SCRIPT_DIR/prepare-loader-source.sh"
