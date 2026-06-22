#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
need unsquashfs
need python3
need sha256sum

FULL_IMAGE_SIZE=$((0x1000000))
ROOTFS_REGION_SIZE=$((0x800000))

find_donor() {
  local candidate
  if [[ -n "${DONOR_IMAGE:-}" ]]; then
    printf '%s\n' "$DONOR_IMAGE"
    return
  fi
  for candidate in \
    "$INPUTS_DIR/postmerkOS-20240818.bin" \
    "$INPUTS_DIR/donor-firmware.bin" \
    "$INPUTS_DIR/good-rootfs.squashfs"; do
    [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return; }
  done
}

DONOR="$(find_donor || true)"
if [[ -z "$DONOR" ]]; then
  if bool_enabled "${AUTO_DOWNLOAD_DONOR:-0}" || ask_yes_no "No donor image was found in inputs/. Download the known postmerkOS donor?" yes; then
    download_file "$DONOR_URL" "$DONOR_DEFAULT"
    DONOR="$DONOR_DEFAULT"
  else
    die "Place a donor image in $INPUTS_DIR or set DONOR_IMAGE=/path/to/file."
  fi
fi
[[ -f "$DONOR" ]] || die "Donor input does not exist: $DONOR"

rm -rf "$DONOR_ROOT"
mkdir -p "$DONOR_ROOT" "$ARTIFACTS_DIR"

case "$DONOR" in
  *.squashfs)
    rootfs_image="$DONOR"
    ;;
  *)
    size="$(file_size "$DONOR")"
    [[ "$size" -eq "$FULL_IMAGE_SIZE" ]] || \
      die "Donor firmware must be exactly 16 MiB; got $size bytes"
    dd if="$DONOR" of="$DONOR_ROOTFS_REGION" bs=1M skip=3 count=8 status=none
    [[ "$(file_size "$DONOR_ROOTFS_REGION")" -eq "$ROOTFS_REGION_SIZE" ]] || \
      die "Extracted donor SquashFS region is not 8 MiB"
    rootfs_image="$DONOR_ROOTFS_REGION"
    ;;
esac

unsquashfs -f -d "$DONOR_ROOT" "$rootfs_image"
[[ -d "$DONOR_ROOT/lib/modules" ]] || die "Donor /lib/modules is missing"

# Convert the donor module links into a self-contained module tree before it is
# staged into Buildroot. Family-specific objects must remain within /lib/modules.
normalized_modules="$EXTRACTED_DIR/donor-modules.materialized"
rm -rf "$normalized_modules"
python3 "$SCRIPT_DIR/materialize-module-tree.py" \
  --donor-root "$DONOR_ROOT" \
  --source "$DONOR_ROOT/lib/modules" \
  --output "$normalized_modules"
rm -rf "$DONOR_ROOT/lib/modules"
mv "$normalized_modules" "$DONOR_ROOT/lib/modules"
python3 "$VENDOR_MODULE_TOOL" create "$DONOR_ROOT/lib/modules" \
  --required-file "$VENDOR_MODULE_REQUIRED" --quiet || \
  die "Donor does not contain the complete Luton26/Jaguar/Jaguar-Dual module matrix"

verify_vendor_module_tree "$DONOR_ROOT/lib/modules" || \
  die "Materialized donor module tree failed platform-complete verification"
find "$DONOR_ROOT/lib/modules" -type f -printf '%P\n' | sort > "$ARTIFACTS_DIR/donor-module-files.txt"
for metadata in \
    postmerkos-required-modules.txt \
    postmerkos-all-modules.txt \
    postmerkos-modules.sha256; do
  cp -f "$DONOR_ROOT/lib/modules/$metadata" "$ARTIFACTS_DIR/$metadata"
done
sha256sum "$DONOR" > "$ARTIFACTS_DIR/donor-input.sha256"
touch "$STAMP_DIR/donor-prepared"
log "Donor module tree is ready"
