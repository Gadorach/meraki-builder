#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

# Run the complete build in the supported Ubuntu environment on Arch-derived
# hosts so the kernel toolchain and Buildroot host utilities use one compiler
# baseline.
if [[ "${MS42P_IN_DISTROBOX:-0}" != 1 ]]; then
  if bool_enabled "${USE_DISTROBOX:-0}"; then
    exec "$SCRIPT_DIR/distrobox-run.sh" env \
      INCLUDE_UI="${INCLUDE_UI:-ask}" \
      CLEAN_BUILDROOT="${CLEAN_BUILDROOT:-0}" \
      REBUILD_KERNEL="${REBUILD_KERNEL:-0}" \
      REBUILD_LOADER="${REBUILD_LOADER:-0}" \
      CLEAN_KERNEL="${CLEAN_KERNEL:-0}" \
      LOADER_REPO_URL="${LOADER_REPO_URL:-https://github.com/Gadorach/meraki-redboot.git}" \
      LOADER_REF="${LOADER_REF:-main}" \
      ./scripts/build-all.sh
  elif command -v pacman >/dev/null 2>&1 && command -v distrobox >/dev/null 2>&1; then
    if ask_yes_no "Run the complete firmware build in Ubuntu 22.04 Distrobox?" yes; then
      exec "$SCRIPT_DIR/distrobox-run.sh" env \
        INCLUDE_UI="${INCLUDE_UI:-ask}" \
        CLEAN_BUILDROOT="${CLEAN_BUILDROOT:-0}" \
        REBUILD_KERNEL="${REBUILD_KERNEL:-0}" \
        REBUILD_LOADER="${REBUILD_LOADER:-0}" \
        CLEAN_KERNEL="${CLEAN_KERNEL:-0}" \
        LOADER_REPO_URL="${LOADER_REPO_URL:-https://github.com/Gadorach/meraki-redboot.git}" \
        LOADER_REF="${LOADER_REF:-main}" \
        ./scripts/build-all.sh
    fi
    export ALLOW_UNSUPPORTED_HOST_BUILD=1
    warn "Continuing on the Arch/CachyOS host. Buildroot 2023.02.4 is not compatible with GCC 16 without additional patches."
  fi
fi

missing=()
for cmd in git make tar xz rsync python3 sha256sum readelf unsquashfs mkfs.jffs2 file; do
  command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done
if (( ${#missing[@]} )); then
  warn "Missing required commands: ${missing[*]}"
  if bool_enabled "${AUTO_INSTALL_DEPS:-0}" || ask_yes_no "Install build dependencies now?" yes; then
    "$SCRIPT_DIR/install-deps.sh"
  else
    die "Install the missing build dependencies before continuing."
  fi
fi

case "${INCLUDE_UI:-ask}" in
  ask|'')
    if ask_yes_no "Include and build Gadorach/postmerkos-ui ($UI_REF)?" yes; then
      INCLUDE_UI=1
    else
      INCLUDE_UI=0
    fi
    ;;
  *)
    bool_enabled "$INCLUDE_UI" && INCLUDE_UI=1 || INCLUDE_UI=0
    ;;
esac
export INCLUDE_UI

# Always select the pinned local revision. This does not contact the network when
# the commit is already present in the existing checkout.
"$SCRIPT_DIR/prepare-sources.sh"

kernel_reusable=0
if [[ -f "$KERNEL_ARTIFACT_DIR/vmlinuz" && -f "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" && \
      -f "$KERNEL_HEADERS_TARBALL" && -f "$KERNEL_BUILD_CONTRACT_RECORD" ]] && \
   ! bool_enabled "${REBUILD_KERNEL:-0}"; then
  if python3 "$SCRIPT_DIR/kernel-build-contract.py" verify \
      --record "$KERNEL_BUILD_CONTRACT_RECORD" \
      --source-revision-file "$ARTIFACTS_DIR/kernel-source-revision.txt" \
      --patch "$KERNEL_BOOTARGS_PATCH" \
      --config-policy "$SCRIPT_DIR/configure-liveboot-kernel.py" \
      --vmlinuz "$KERNEL_ARTIFACT_DIR/vmlinuz" \
      --vmlinuz-bin "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" \
      --headers "$KERNEL_HEADERS_TARBALL"; then
    kernel_reusable=1
  else
    warn "Existing kernel artifacts do not satisfy the current PMOSLIVE boot-argument contract; rebuilding."
  fi
fi

if (( kernel_reusable == 0 )); then
  if bool_enabled "${AUTO_BUILD_KERNEL:-0}" || ask_yes_no "A usable PMOSLIVE-compatible kernel build is missing. Build it now?" yes; then
    "$SCRIPT_DIR/build-kernel.sh"
  else
    die "PMOSLIVE-compatible kernel artifacts are required."
  fi
else
  log "Reusing verified PMOSLIVE-compatible kernel artifacts"
fi

if [[ ! -f "$LOADER_ARTIFACT" || ! -f "$LOADER_MANIFEST" || ! -f "$LOADER_BUILD_SOURCE_RECORD" ]] || \
   bool_enabled "${REBUILD_LOADER:-0}"; then
  "$SCRIPT_DIR/build-loader.sh"
else
  if ! python3 - "$LOADER_ARTIFACT" "$LOADER_MANIFEST" "$LOADER_BUILD_SOURCE_RECORD" \
      "$LOADER_SOURCE_REVISION_FILE" "$RECOVERY_ARTIFACT_DIR" "$LIVEBOOT_ARTIFACT_DIR" <<'PY_LOADER'
import hashlib, json, sys
from pathlib import Path
image, manifest_path, source_record_path, selected_revision_path, recovery_dir, liveboot_dir = map(Path, sys.argv[1:])
data = image.read_bytes()
manifest = json.loads(manifest_path.read_text())
source_record = json.loads(source_record_path.read_text())
selected_revision = selected_revision_path.read_text().strip()
cap = manifest.get("uart_ramloader", {})
policies = manifest.get("policies", {})
assert len(data) == 0x40000
for marker in (b"PMOSRAM READY 2", b"PMOSBOOT MENU-PROBE", b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT"):
    assert marker in data
assert manifest.get("format") == "postmerkos.vcoreiii-linuxloader-build.v7"
assert cap.get("enabled") is True and cap.get("protocol_version") == 2
assert cap.get("boot_menu", {}).get("options") == {"1": "uart-ramloader", "2": "embedded-firmware-recovery", "3": "embedded-liveboot"}
assert cap.get("image_check_diagnostics") == "structured-pass-warn-fail-skip-values-v1"
assert cap.get("stage1_flash_offset") == 0x00020000
assert cap.get("stage1_storage_contract") == "single-shared-boot-region-blob-v1"
assert policies.get("payload_slot_end") == 0x300000 and policies.get("hard_payload_limit") == 0x2BFFE0
assert manifest.get("boot_region", {}).get("sha256") == hashlib.sha256(data).hexdigest()
assert source_record.get("project") == "Gadorach/meraki-redboot"
assert source_record.get("revision") == selected_revision
embedded = cap.get("embedded_recovery", {})
for family in ("luton26", "jaguar1"):
    payload = recovery_dir / f"recovery-{family}.bin"
    descriptor_path = recovery_dir / f"recovery-{family}.descriptor.json"
    assert payload.is_file() and descriptor_path.is_file()
    raw = payload.read_bytes()
    descriptor = json.loads(descriptor_path.read_text())
    assert descriptor.get("load_address") == 0x86C00000
    assert descriptor.get("entry_address") == 0x86C00000
    assert descriptor.get("entry_contract") == "flat-binary-byte-zero-v1"
    assert descriptor.get("manifest_lookup_contract") == "direct-object-members-v1"
    assert descriptor.get("hardware_preflight_contract") == "spi-nor-scratch-rw-restore-loader-crc-v4"
    assert descriptor.get("spi_master_enable_contract") == "preserve-general-ctrl-enable-spi-v1"
    assert descriptor.get("adaptive_transport_contract") == "pmosrec-v3-adaptive-uart-sparse-lz4-v1"
    assert descriptor.get("transport_integrity") == ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"]
    assert descriptor.get("operations") == ["verify", "preflight", "dry-run", "flash"]
    assert descriptor.get("preflight_scratch") == {"default_address": 0x00FF0000, "bytes": 0x10000, "minimum_address": 0x40000, "restore_original": True}
    binary = descriptor.get("binary", {})
    digest = hashlib.sha256(raw).hexdigest()
    assert binary.get("filename") == payload.name
    assert binary.get("bytes") == len(raw)
    assert str(binary.get("sha256", "")).lower() == digest
    record = embedded.get(family, {})
    assert record.get("size") == len(raw)
    assert str(record.get("sha256", "")).lower() == digest
    assert record.get("load_address") == 0x86C00000
    assert record.get("entry_address") == 0x86C00000
    assert record.get("entry_contract") == "flat-binary-byte-zero-v1"
    assert record.get("manifest_lookup_contract") == "direct-object-members-v1"
    assert record.get("hardware_preflight_contract") == "spi-nor-scratch-rw-restore-loader-crc-v4"
    assert record.get("spi_master_enable_contract") == "preserve-general-ctrl-enable-spi-v1"
    assert record.get("adaptive_transport_contract") == "pmosrec-v3-adaptive-uart-sparse-lz4-v1"

live_expected = {
    "luton26": (1, ["MS22", "MS22P", "MS220-8", "MS220-8P", "MS220-24", "MS220-24P"]),
    "jaguar1": (2, ["MS42", "MS42P"]),
}
expected_live_ram = {
    "kernel_load_address": 0x81000000,
    "image_staging_address": 0x81400000,
    "manifest_address": 0x82400000,
    "payload_address": 0x86C00000,
    "squashfs_address": 0x87000000,
    "boot_params_physical_address": 0x00000400,
    "boot_params_uncached_address": 0xA0000400,
    "boot_params_bytes": 0x00000C00,
    "linux_memory_mib": 120,
    "top_reserved_mib": 8,
}
for family, (family_id, models) in live_expected.items():
    live_payload = liveboot_dir / f"pmoslive-{family}.bin"
    live_descriptor_path = liveboot_dir / f"pmoslive-{family}.descriptor.json"
    if not live_payload.is_file() or not live_descriptor_path.is_file():
        raise SystemExit(f"PMOSLIVE build output is missing for {family}")
    live_raw = live_payload.read_bytes()
    live_descriptor = json.loads(live_descriptor_path.read_text(encoding="utf-8"))
    live_marker = (
        f"PMOSLIVE3;SOC={family};FAMILY={family_id};PROTO=3;FLASH=0;LIVEBOOT=1;"
        "IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
        "FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END"
    ).encode("ascii")
    if live_raw.count(live_marker) != 1:
        raise SystemExit(f"{live_payload.name} has an invalid embedded target descriptor")
    if any(marker in live_raw for marker in (b"ERASEFLASH", b"FLASH-PREFLIGHT", b"PROGRESS ERASE", b"PROGRESS PROGRAM")):
        raise SystemExit(f"{live_payload.name} unexpectedly retains flash-write markers")
    if live_descriptor.get("format") != "postmerkos.uart-liveboot-payload.v1" or live_descriptor.get("protocol_version") != 3:
        raise SystemExit(f"{live_payload.name} descriptor format/protocol is unsupported")
    if live_descriptor.get("soc_family") != family or live_descriptor.get("soc_family_id") != family_id:
        raise SystemExit(f"{live_payload.name} descriptor family is invalid")
    if live_descriptor.get("accepted_models") != models:
        raise SystemExit(f"{live_payload.name} descriptor target allow-list is invalid")
    if live_descriptor.get("operations") != ["verify", "dry-run", "liveboot"] or live_descriptor.get("flash_access") != "none":
        raise SystemExit(f"{live_payload.name} descriptor operation/flash policy is invalid")
    if live_descriptor.get("load_address") != 0x86C00000 or live_descriptor.get("entry_address") != 0x86C00000:
        raise SystemExit(f"{live_payload.name} descriptor high-memory address is invalid")
    if live_descriptor.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"{live_payload.name} descriptor entry contract is invalid")
    if live_descriptor.get("transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"{live_payload.name} descriptor transport contract is invalid")
    if live_descriptor.get("linux_handoff") != "mips-legacy-argc-argv-envp-external-initrd-v1":
        raise SystemExit(f"{live_payload.name} descriptor Linux handoff is invalid")
    if live_descriptor.get("platform_identity_handoff") != "kernel-command-line-postmerkos-model-v1":
        raise SystemExit(f"{live_payload.name} descriptor platform identity handoff is invalid")
    if live_descriptor.get("rootfs_handoff") != "squashfs-as-legacy-initrd-v1":
        raise SystemExit(f"{live_payload.name} descriptor rootfs handoff is invalid")
    if live_descriptor.get("ram_layout") != expected_live_ram:
        raise SystemExit(f"{live_payload.name} descriptor RAM layout is invalid")
    binary = live_descriptor.get("binary", {})
    digest = hashlib.sha256(live_raw).hexdigest()
    if binary.get("filename") != live_payload.name or binary.get("bytes") != len(live_raw) or str(binary.get("sha256", "")).lower() != digest:
        raise SystemExit(f"{live_payload.name} descriptor binary record mismatch")
    embedded_live = cap.get("embedded_liveboot", {}).get(family, {})
    if embedded_live.get("size") != len(live_raw) or str(embedded_live.get("sha256", "")).lower() != digest:
        raise SystemExit(f"{live_payload.name} standalone payload does not match loader embedding")
    expected_embedded = {
        "load_address": 0x86C00000,
        "entry_address": 0x86C00000,
        "entry_contract": "flat-binary-byte-zero-v1",
        "flash_access": "none",
        "accepted_models": models,
        "transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
        "linux_handoff": "mips-legacy-argc-argv-envp-external-initrd-v1",
        "platform_identity_handoff": "kernel-command-line-postmerkos-model-v1",
        "rootfs_handoff": "squashfs-as-legacy-initrd-v1",
        "kernel_load_address": 0x81000000,
        "squashfs_address": 0x87000000,
        "boot_params_physical_address": 0x00000400,
        "boot_params_uncached_address": 0xA0000400,
        "boot_params_bytes": 0x00000C00,
        "linux_memory_mib": 120,
        "top_reserved_mib": 8,
    }
    for key, value in expected_embedded.items():
        if embedded_live.get(key) != value:
            raise SystemExit(f"loader embedded {family} PMOSLIVE {key} mismatch")

PY_LOADER
  then
    warn "The cached loader does not match the selected meraki-redboot source release; rebuilding it."
    "$SCRIPT_DIR/build-loader.sh"
  else
    log "Reusing validated source-built meraki-redboot, recovery payloads, and PMOSLIVE"
  fi
fi

donor_modules_ready() {
  verify_vendor_module_tree "$DONOR_ROOT/lib/modules" >/dev/null 2>&1
}

if ! donor_modules_ready || bool_enabled "${REEXTRACT_DONOR:-0}"; then
  "$SCRIPT_DIR/prepare-donor.sh"
else
  log "Reusing verified materialized donor modules"
fi

if (( INCLUDE_UI )); then
  "$SCRIPT_DIR/build-ui.sh"
else
  log "Building without the optional web interface"
fi

"$SCRIPT_DIR/prepare-buildroot.sh"
"$SCRIPT_DIR/build-rootfs.sh"
"$SCRIPT_DIR/validate-image.sh"

IMAGE="$(cat "$ARTIFACTS_DIR/latest-image.txt")"
printf '\nBuild complete.\nImage: %s\nSHA256: %s\n' \
  "$IMAGE" "$(sha256sum "$IMAGE" | awk '{print $1}')"
