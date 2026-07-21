#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
common = (ROOT / "scripts/common.sh").read_text(encoding="utf-8")
prepare = (ROOT / "scripts/prepare-loader-source.sh").read_text(encoding="utf-8")
build_ui = (ROOT / "scripts/build-ui.sh").read_text(encoding="utf-8")
build_loader = (ROOT / "scripts/build-loader.sh").read_text(encoding="utf-8")

assert 'LOADER_REF="${LOADER_REF:-main}"' in common
assert 'UI_REF="${UI_REF:-ms42p-dev}"' in common
assert 'if [[ "$ref" == latest ]]' in common and 'ref=main' in common
assert 'origin/$ref' in common
assert 'latest tagged' not in common

for forbidden in (
    "git -C \"$LOADER_SOURCE_DIR\" apply",
    "git apply",
    "Applying meraki-redboot",
    "patches/meraki-redboot",
    "postmerkOS-builder",
):
    assert forbidden not in prepare, f"loader preparation still mutates upstream source: {forbidden}"

for required in (
    'clone_or_update_git_ref "$LOADER_REPO_URL"',
    "source contract validated; checkout left unmodified",
    "meraki-builder never applies source patches",
    'status --porcelain',
    "PMOSRECOVERY3;SOC=luton26",
    "PMOSRECOVERY3;SOC=jaguar1",
):
    assert required in prepare, f"loader preparation lacks policy marker: {required}"

assert not (ROOT / "patches" / "meraki-redboot").exists(), "builder still ships meraki-redboot patches"
assert 'clone_or_update_git_ref "$UI_REPO_URL" "$UI_DIR" "$UI_REF"' in build_ui
assert "tracked source changed during build" in build_ui
assert "LOADER_SOURCE_ARCHIVE is no longer supported" in prepare

assert "normalize_future_git_timestamps" in common
assert "LOADER_SOURCE_SELECTION_RECORD" in common and "LOADER_SOURCE_SELECTION_RECORD" in prepare
assert "RESOLVED_GIT_SYMBOLIC_REF" not in build_loader
assert "source provenance record is missing" in build_loader
assert "same-origin /ws WebSocket path" in build_ui
assert "while IFS='=' read -r name _" in build_ui

print("authoritative meraki-redboot/postmerkos-ui source policy passed")
