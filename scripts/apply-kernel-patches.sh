#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

mode=apply
case "${1:-}" in
  "") ;;
  --reverse) mode=reverse ;;
  *) die "usage: $0 [--reverse]" ;;
esac

patches=(
  "$REPO_ROOT/kernel/patches/0001-mips-vcoreiii-accept-uboot-bootargs.patch"
)

[[ -d "$SWITCH_DIR/linux-3.18" ]] || die "Linux source is missing: $SWITCH_DIR/linux-3.18"
need git

for patch_file in "${patches[@]}"; do
  [[ -f "$patch_file" ]] || die "Kernel patch is missing: $patch_file"
  if [[ "$mode" == apply ]]; then
    if git -C "$SWITCH_DIR" apply --whitespace=nowarn --reverse --check "$patch_file" >/dev/null 2>&1; then
      log "Kernel patch already applied: $(basename "$patch_file")"
    elif git -C "$SWITCH_DIR" apply --whitespace=nowarn --check "$patch_file" >/dev/null 2>&1; then
      git -C "$SWITCH_DIR" apply --whitespace=nowarn "$patch_file"
      log "Applied kernel patch: $(basename "$patch_file")"
    else
      die "Kernel patch does not apply cleanly: $patch_file"
    fi
  else
    if git -C "$SWITCH_DIR" apply --whitespace=nowarn --reverse --check "$patch_file" >/dev/null 2>&1; then
      git -C "$SWITCH_DIR" apply --whitespace=nowarn --reverse "$patch_file"
      log "Removed managed kernel patch: $(basename "$patch_file")"
    elif git -C "$SWITCH_DIR" apply --whitespace=nowarn --check "$patch_file" >/dev/null 2>&1; then
      : # Pristine source already has no managed patch.
    else
      die "Managed kernel patch state is inconsistent: $patch_file"
    fi
  fi
done
