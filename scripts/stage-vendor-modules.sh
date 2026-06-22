#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
need python3

source_tree="$DONOR_ROOT/lib/modules"
[[ -d "$source_tree" ]] || die "Donor module tree is missing: $source_tree"
verify_vendor_module_tree "$source_tree" || \
  die "Donor module tree is incomplete; run REEXTRACT_DONOR=1 make donor"

if (($# == 0)); then
  set -- "$GENERATED_OVERLAY/lib/modules"
fi

for destination in "$@"; do
  [[ -n "$destination" && "$destination" != / ]] || \
    die "Unsafe module staging destination: $destination"
  # Recreate the staging destination to prevent stale modules while avoiding
  # incremental-copy edge cases on overlay/container filesystems.
  rm -rf -- "$destination"
  mkdir -p "$destination"
  # Copy the complete donor module tree, not only the family used by the build
  # host. Runtime board identification chooses the appropriate family later.
  cp -a "$source_tree/." "$destination/"
  python3 "$VENDOR_MODULE_TOOL" verify "$destination" \
    --required-file "$VENDOR_MODULE_REQUIRED" --quiet || \
    die "Staged module tree is not platform complete: $destination"
  log "Staged complete VCore-III vendor module matrix in $destination"
done
