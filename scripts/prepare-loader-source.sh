#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

if [[ -n "$LOADER_SOURCE_ARCHIVE" ]]; then
  die "LOADER_SOURCE_ARCHIVE is no longer supported. meraki-builder requires the authoritative meraki-redboot Git main branch and never imports or patches source archives."
fi

clone_or_update_git_ref "$LOADER_REPO_URL" "$LOADER_SOURCE_DIR" "$LOADER_REF" "meraki-redboot"
selected_revision="$(git -C "$LOADER_SOURCE_DIR" rev-parse HEAD)"

entry_descriptor="$LOADER_SOURCE_DIR/payloads/uart-firmware-recovery/write_descriptor.py"
recovery_source="$LOADER_SOURCE_DIR/payloads/uart-firmware-recovery/recovery.c"
stage_validator="$LOADER_SOURCE_DIR/scripts/validate_uart_stage1.py"
structural_stage_test="$LOADER_SOURCE_DIR/scripts/structural-test-clang.sh"

[[ -f "$LOADER_SOURCE_DIR/Makefile" ]] || die "meraki-redboot Makefile is missing"
[[ -f "$LOADER_SOURCE_DIR/VERSION" ]] || die "meraki-redboot VERSION is missing"
[[ -f "$LOADER_PAYLOAD_PACKER" ]] || die "meraki-redboot payload packer is missing"
[[ -f "$entry_descriptor" && -f "$recovery_source" ]] || \
  die "meraki-redboot main lacks the UART firmware recovery source"

require_source_marker() {
  local file="$1" marker="$2" description="$3"
  grep -Fq "$marker" "$file" || die \
    "selected meraki-redboot source lacks $description. Update Gadorach/meraki-redboot main; meraki-builder never applies source patches."
}

require_source_marker "$entry_descriptor" 'flat-binary-byte-zero-v1' 'the corrected flat-binary entry contract'
require_source_marker "$recovery_source" 'PACKAGE_HEADER_TIMEOUT_MS 30000u' 'the recovery package-header grace contract'
require_source_marker "$entry_descriptor" 'direct-object-members-v1' 'the direct-member manifest lookup contract'
require_source_marker "$recovery_source" 'direct_object_depth' 'the scoped manifest parser implementation'
require_source_marker "$entry_descriptor" 'spi-nor-scratch-rw-restore-loader-crc-v4' 'the destructive SPI NOR preflight contract'
require_source_marker "$entry_descriptor" 'preserve-general-ctrl-enable-spi-v1' 'the SPI master-enable contract'
require_source_marker "$recovery_source" 'SPI_CS0_MASK         0x01u' 'the MSCC active-mask chip-select contract'
require_source_marker "$recovery_source" 'spi_active_base' 'the MSCC software-SPI implementation'
require_source_marker "$entry_descriptor" 'pmosrec-v3-adaptive-uart-sparse-lz4-v1' 'the PMOSREC v3 adaptive transport contract'
require_source_marker "$recovery_source" 'PMOSRECOVERY3' 'PMOSREC protocol v3'
require_source_marker "$recovery_source" 'PREFLIGHT=4' 'PMOSREC hardware preflight v4'
require_source_marker "$stage_validator" 'PMOSRECOVERY3;SOC=luton26' 'the Luton26 PMOSREC v3 stage-validator marker'
require_source_marker "$stage_validator" 'PMOSRECOVERY3;SOC=jaguar1' 'the Jaguar1 PMOSREC v3 stage-validator marker'
require_source_marker "$structural_stage_test" 'PMOSRECOVERY3;SOC=luton26;STRUCTURAL' 'the Luton26 structural PMOSREC v3 marker'
require_source_marker "$structural_stage_test" 'PMOSRECOVERY3;SOC=jaguar1;STRUCTURAL' 'the Jaguar1 structural PMOSREC v3 marker'

[[ "$(git -C "$LOADER_SOURCE_DIR" rev-parse HEAD)" == "$selected_revision" ]] || \
  die "meraki-builder unexpectedly changed the selected meraki-redboot revision"
[[ -z "$(git -C "$LOADER_SOURCE_DIR" status --porcelain)" ]] || \
  die "meraki-builder unexpectedly modified the meraki-redboot source checkout"

printf '%s\n' "$selected_revision" > "$LOADER_SOURCE_REVISION_FILE"
cat "$LOADER_SOURCE_DIR/VERSION" > "$LOADER_SOURCE_VERSION_FILE"
python3 - "$LOADER_SOURCE_SELECTION_RECORD" "$LOADER_REPO_URL" "$LOADER_REF" \
  "$RESOLVED_GIT_SYMBOLIC_REF" "$selected_revision" "$RESOLVED_GIT_DESCRIBE" <<'PY_SOURCE'
import json, sys
from pathlib import Path
out, repository, requested, resolved, revision, describe = sys.argv[1:]
Path(out).write_text(json.dumps({
    "project": "Gadorach/meraki-redboot",
    "repository": repository,
    "requested_ref": requested,
    "resolved_ref": resolved,
    "revision": revision,
    "describe": describe,
    "resolution": "authoritative-git",
}, indent=2, sort_keys=True) + "\n")
PY_SOURCE
log "meraki-redboot $(cat "$LOADER_SOURCE_VERSION_FILE") selected from authoritative origin/$RESOLVED_GIT_SYMBOLIC_REF at $selected_revision"
log "meraki-redboot source contract validated; checkout left unmodified"
