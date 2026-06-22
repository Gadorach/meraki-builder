#!/usr/bin/env python3
"""Protocol and validation helpers for postmerkOS pre-kernel UART recovery."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import sys
import termios
import time
import zlib

RAM_MAGIC = b"PMOSRAM2"
RAM_FRAME_MAGIC = b"RMF2"
RAM_HEADER = struct.Struct("<8s7I32sI")
RAM_FRAME = struct.Struct("<4sIII")
PACKAGE_MAGIC = b"PMOSPKG2"
PACKAGE_FRAME_MAGIC = b"PKF2"
PACKAGE_HEADER = struct.Struct("<8s8I32s32s16sI")
PACKAGE_FRAME = struct.Struct("<4sIIII")
PREFLIGHT_MAGIC = b"PMOSPFT1"
PREFLIGHT_HEADER = struct.Struct("<8s6I")
PREFLIGHT_VERSION = 1
PREFLIGHT_FLAG_RESTORE = 1
DEFAULT_PREFLIGHT_SCRATCH = 0x00FF0000
PREFLIGHT_SCRATCH_BYTES = 64 * 1024
RAM_PROTOCOL_VERSION = 2
RECOVERY_PROTOCOL_VERSION = 3
PROTOCOL_VERSION = RAM_PROTOCOL_VERSION
FULL_IMAGE_SIZE = 16 * 1024 * 1024
LOADER_REGION_SIZE = 0x40000
KERNEL_OFFSET = 0x40000
ROOTFS_OFFSET = 0x300000
KERNEL_REGION_SIZE = ROOTFS_OFFSET - KERNEL_OFFSET
SPIM_MAGIC = 0x4D495053
SPIM_HEADER = struct.Struct("<8I")
SPIM_ALIGNMENT = 32
SPIM_MAX_PAYLOAD = KERNEL_REGION_SIZE - SPIM_HEADER.size
FLAG_FULL_FLASH = 1
FLAG_DRY_RUN = 2
FLAG_FORCE_UNTESTED = 4
OBJECT_IMAGE = 1
OBJECT_MANIFEST = 2

MODEL_FAMILY = {
    "MS22": "luton26", "MS22P": "luton26", "MS220-8": "luton26",
    "MS220-8P": "luton26", "MS220-24": "luton26", "MS220-24P": "luton26",
    "MS320-24": "jaguar1", "MS320-24P": "jaguar1",
    "MS220-48": "jaguar1", "MS220-48P": "jaguar1", "MS220-48LP": "jaguar1",
    "MS220-48FP": "jaguar1", "MS320-48": "jaguar1", "MS320-48P": "jaguar1",
    "MS320-48LP": "jaguar1", "MS320-48FP": "jaguar1", "MS42": "jaguar1",
    "MS42P": "jaguar1",
}
FAMILY_ID = {"luton26": 1, "jaguar1": 2}
FAMILY_SPI_ADDRESS = {"luton26": 0x70000064, "jaguar1": 0x70000068}
ALLOWED_MODEL_STATUS = {"validated", "confirmed", "untested"}
DESCRIPTOR_RE = re.compile(
    rb"PMOSRECOVERY3;SOC=(luton26|jaguar1);FAMILY=([12]);SPI=([0-9a-f]{8});PROTO=3;PREFLIGHT=4;BAUDTEST=1;FRAME_MAX=4096;WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;CONFIRM_RETRY=1;AUTO_CONFIRM=1;AUTO_REBOOT=1;END"
)


class ProtocolError(RuntimeError):
    pass


class EmbeddedRecoveryEntryError(ProtocolError):
    """The loader jumped to an embedded payload that never reached its byte-zero entry."""


@dataclass(frozen=True)
class PayloadDescriptor:
    family: str
    family_id: int
    spi_address: int
    sha256: str
    size: int
    load_address: int
    entry_address: int
    entry_contract: str
    manifest_lookup_contract: str
    hardware_preflight_contract: str
    spi_master_enable_contract: str
    adaptive_transport_contract: str


@dataclass(frozen=True)
class BundleInfo:
    image: Path
    manifest: Path
    model: str
    family: str
    image_sha256: bytes
    manifest_sha256: bytes
    image_crc32: int
    manifest_crc32: int
    manifest_bytes: bytes
    model_status: str


def sha256_file(path: Path) -> bytes:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.digest()


def crc32_file(path: Path) -> int:
    value = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value = zlib.crc32(block, value)
    return value & 0xFFFFFFFF


def inspect_payload(path: Path, descriptor_path: Path | None = None) -> PayloadDescriptor:
    data = path.read_bytes()
    matches = list(DESCRIPTOR_RE.finditer(data))
    if len(matches) != 1:
        if b"PMOSRECOVERY2;SOC=" in data:
            raise ProtocolError("stale PMOSRECOVERY2 descriptor; rebuild the PMOSREC v3 recovery payload")
        raise ProtocolError("recovery payload must contain exactly one PMOSRECOVERY3 descriptor")
    match = matches[0]
    family = match.group(1).decode("ascii")
    family_id = int(match.group(2))
    if FAMILY_ID[family] != family_id:
        raise ProtocolError("recovery payload descriptor family ID is inconsistent")

    if descriptor_path is None:
        descriptor_path = path.with_suffix(".descriptor.json")
    if not descriptor_path.is_file():
        raise ProtocolError(
            f"recovery payload entry descriptor is missing: {descriptor_path}; "
            "rebuild meraki-redboot with the flat-binary entry fix"
        )
    try:
        metadata = json.loads(descriptor_path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"invalid recovery payload descriptor: {exc}") from exc
    binary = metadata.get("binary")
    if not isinstance(binary, dict):
        raise ProtocolError("recovery payload descriptor has no binary record")
    digest = hashlib.sha256(data).hexdigest()
    if binary.get("filename") != path.name or binary.get("bytes") != len(data):
        raise ProtocolError("recovery payload descriptor binary size/name mismatch")
    if str(binary.get("sha256", "")).lower() != digest:
        raise ProtocolError("recovery payload descriptor SHA-256 mismatch")
    if metadata.get("soc_family") != family or metadata.get("soc_family_id") != family_id:
        raise ProtocolError("recovery payload sidecar family does not match its embedded marker")
    if metadata.get("spi_software_mode_address") != int(match.group(3), 16):
        raise ProtocolError("recovery payload sidecar SPI address does not match its embedded marker")
    load_address = metadata.get("load_address")
    entry_address = metadata.get("entry_address")
    entry_contract = metadata.get("entry_contract")
    manifest_lookup_contract = metadata.get("manifest_lookup_contract")
    hardware_preflight_contract = metadata.get("hardware_preflight_contract")
    spi_master_enable_contract = metadata.get("spi_master_enable_contract")
    adaptive_transport_contract = metadata.get("adaptive_transport_contract")
    if load_address != 0x81000000 or entry_address != 0x81000000:
        raise ProtocolError("recovery payload is not linked for load/entry address 0x81000000")
    if entry_contract != "flat-binary-byte-zero-v1":
        raise ProtocolError(
            "recovery payload lacks the flat-binary-byte-zero-v1 entry contract; "
            "the v0.7.0 payload can hang immediately after PASS-RECOVERY-EXEC"
        )
    if manifest_lookup_contract != "direct-object-members-v1":
        raise ProtocolError(
            "recovery payload lacks direct-object-members-v1 manifest parsing; "
            "nested kernel/region digests can shadow artifact.sha256"
        )
    if metadata.get("protocol_version") != RECOVERY_PROTOCOL_VERSION:
        raise ProtocolError("recovery payload protocol is not PMOSREC v3")
    if hardware_preflight_contract != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise ProtocolError("recovery payload lacks PREFLIGHT=4 adaptive transport and destructive scratch read/write/restore support")
    if spi_master_enable_contract != "preserve-general-ctrl-enable-spi-v1":
        raise ProtocolError("recovery payload lacks the SPI master-enable handoff correction")
    if adaptive_transport_contract != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise ProtocolError("recovery payload lacks the PMOSREC v3 adaptive transport contract")
    return PayloadDescriptor(
        family=family,
        family_id=family_id,
        spi_address=int(match.group(3), 16),
        sha256=digest,
        size=len(data),
        load_address=load_address,
        entry_address=entry_address,
        entry_contract=entry_contract,
        manifest_lookup_contract=manifest_lookup_contract,
        hardware_preflight_contract=hardware_preflight_contract,
        spi_master_enable_contract=spi_master_enable_contract,
        adaptive_transport_contract=adaptive_transport_contract,
    )


def _validate_loader_capability(manifest: dict, loader_sha256: str, family: str) -> None:
    recovery = manifest.get("recovery")
    if not isinstance(recovery, dict):
        raise ProtocolError("manifest does not contain a recovery capability record")
    loader = recovery.get("uart_ramloader")
    if not isinstance(loader, dict) or loader.get("enabled") is not True:
        raise ProtocolError("firmware loader is not marked UART RAM-loader capable")
    if loader.get("protocol_version") != PROTOCOL_VERSION:
        raise ProtocolError("firmware loader protocol version is incompatible")
    if str(loader.get("loader_sha256", "")).lower() != loader_sha256:
        raise ProtocolError("firmware loader capability digest does not match the image")
    supported = loader.get("supported_soc_families")
    if not isinstance(supported, list) or family not in supported:
        raise ProtocolError("firmware loader capability does not include the target SoC family")
    if loader.get("build_manifest_format") != "postmerkos.vcoreiii-linuxloader-build.v7":
        raise ProtocolError("firmware loader is not a meraki-redboot v0.7 source build")
    menu = loader.get("boot_menu")
    expected_options = {"1": "uart-ramloader", "2": "embedded-firmware-recovery"}
    if not isinstance(menu, dict) or menu.get("options") != expected_options:
        raise ProtocolError("firmware loader does not expose the meraki-redboot recovery menu")
    if loader.get("image_check_diagnostics") != "structured-pass-warn-fail-skip-values-v1":
        raise ProtocolError("firmware loader does not declare structured image diagnostics")
    embedded = loader.get("embedded_recovery")
    record = embedded.get(family) if isinstance(embedded, dict) else None
    if not isinstance(record, dict) or not re.fullmatch(r"[0-9a-fA-F]{64}", str(record.get("sha256", ""))):
        raise ProtocolError(f"firmware loader does not bind an embedded recovery payload for {family}")
    if record.get("load_address") != 0x81000000 or record.get("entry_address") != 0x81000000:
        raise ProtocolError("firmware loader embedded recovery has an invalid load/entry address")
    if record.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise ProtocolError(
            "firmware image contains the affected v0.7.0 recovery layout; rebuild meraki-redboot "
            "with the flat-binary-byte-zero-v1 entry fix before flashing"
        )
    if record.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise ProtocolError(
            "firmware image contains a recovery parser that permits nested digest shadowing; "
            "rebuild meraki-redboot with direct-object-members-v1 manifest lookup"
        )
    if record.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise ProtocolError(
            "firmware image contains a recovery payload without adaptive UART and destructive SPI NOR preflight support"
        )
    if record.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise ProtocolError("firmware image embedded recovery lacks the PMOSREC v3 adaptive transport contract")
    if record.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise ProtocolError(
            "firmware image contains a recovery payload without the SPI master-enable handoff correction"
        )


def validate_spim_kernel(image: Path, artifact: dict | None = None) -> dict[str, int | str]:
    with image.open("rb") as stream:
        stream.seek(KERNEL_OFFSET)
        header = stream.read(SPIM_HEADER.size)
        if len(header) != SPIM_HEADER.size:
            raise ProtocolError("image is too short for the SPIM kernel header")
        words = SPIM_HEADER.unpack(header)
        magic, load, size, entry, stored_crc, r0, r1, r2 = words
        if magic != SPIM_MAGIC:
            raise ProtocolError("image kernel region is missing the SPIM header")
        if load != 0x81000000 or entry != 0x81000000:
            raise ProtocolError("SPIM kernel load/entry address is incompatible")
        if not 0 < size <= SPIM_MAX_PAYLOAD:
            raise ProtocolError("SPIM kernel payload exceeds the postmerkOS kernel slot")
        if size % SPIM_ALIGNMENT:
            raise ProtocolError("SPIM kernel payload size is not 32-byte aligned")
        if (r0, r1, r2) != (0, 0, 0):
            raise ProtocolError("SPIM kernel reserved header words must be zero")
        payload = stream.read(size)
        if len(payload) != size:
            raise ProtocolError("SPIM kernel payload is truncated")
    zero_header = bytearray(header)
    struct.pack_into("<I", zero_header, 16, 0)
    calculated_crc = zlib.crc32(zero_header + payload) & 0xFFFFFFFF
    if stored_crc != calculated_crc:
        raise ProtocolError(
            f"SPIM kernel CRC mismatch: expected 0x{stored_crc:08x}, got 0x{calculated_crc:08x}"
        )
    result: dict[str, int | str] = {
        "payload_bytes": size,
        "load_address": load,
        "entry_point": entry,
        "crc32": f"{stored_crc:08x}",
        "sha256": hashlib.sha256(payload).hexdigest(),
    }
    if artifact is not None:
        record = artifact.get("kernel_payload")
        if not isinstance(record, dict):
            raise ProtocolError("manifest artifact is missing kernel_payload metadata")
        for key in ("payload_bytes", "load_address", "entry_point"):
            if record.get(key) != result[key]:
                raise ProtocolError(f"manifest kernel_payload {key} does not match the image")
        manifest_crc = str(record.get("crc32", "")).lower().removeprefix("0x")
        if manifest_crc != result["crc32"]:
            raise ProtocolError("manifest kernel_payload CRC does not match the image")
        if str(record.get("sha256", "")).lower() != result["sha256"]:
            raise ProtocolError("manifest kernel_payload SHA-256 does not match the image")
        if record.get("alignment_bytes") != SPIM_ALIGNMENT or record.get("header_bytes") != SPIM_HEADER.size:
            raise ProtocolError("manifest kernel_payload geometry is incompatible")
    return result



def _recovery_payload_record(manifest: dict, family: str, model: str) -> dict:
    recovery = manifest.get("recovery")
    if not isinstance(recovery, dict):
        raise ProtocolError("manifest does not contain recovery metadata")
    firmware = recovery.get("uart_firmware")
    if not isinstance(firmware, dict) or firmware.get("enabled") is not True:
        raise ProtocolError("manifest does not enable UART firmware recovery")
    if firmware.get("protocol_version") != RECOVERY_PROTOCOL_VERSION:
        raise ProtocolError("manifest UART firmware recovery protocol is incompatible")
    if firmware.get("full_image_bytes") != FULL_IMAGE_SIZE:
        raise ProtocolError("manifest UART firmware recovery image size is incompatible")
    if firmware.get("operations") != ["verify", "preflight", "dry-run", "flash"]:
        raise ProtocolError("manifest UART firmware recovery operation contract is incompatible")
    if firmware.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise ProtocolError("manifest lacks destructive SPI NOR preflight support")
    if firmware.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise ProtocolError("manifest lacks the SPI master-enable handoff correction")
    expected_scratch = {
        "default_address": DEFAULT_PREFLIGHT_SCRATCH,
        "bytes": PREFLIGHT_SCRATCH_BYTES,
        "minimum_address": LOADER_REGION_SIZE,
        "restore_original": True,
    }
    if firmware.get("preflight_scratch") != expected_scratch:
        raise ProtocolError("manifest preflight scratch contract is incompatible")
    geometry = firmware.get("flash_geometry")
    expected_geometry = {
        "bytes": FULL_IMAGE_SIZE, "erase_bytes": 64 * 1024,
        "page_bytes": 256, "address_bytes": 3,
    }
    if geometry != expected_geometry:
        raise ProtocolError("manifest flash geometry is incompatible with the recovery payload")
    jedec = firmware.get("accepted_jedec_ids")
    if not isinstance(jedec, list) or not jedec or any(
        not isinstance(item, str) or not re.fullmatch(r"[0-9a-fA-F]{6}", item) for item in jedec
    ):
        raise ProtocolError("manifest accepted JEDEC ID list is invalid")
    payloads = firmware.get("payloads")
    record = payloads.get(family) if isinstance(payloads, dict) else None
    if not isinstance(record, dict):
        raise ProtocolError(f"manifest has no recovery payload record for {family}")
    if record.get("soc_family_id") != FAMILY_ID[family]:
        raise ProtocolError("manifest recovery payload family ID is inconsistent")
    if record.get("spi_software_mode_address") != FAMILY_SPI_ADDRESS[family]:
        raise ProtocolError("manifest recovery payload SPI register is inconsistent")
    accepted_models = record.get("accepted_models")
    if not isinstance(accepted_models, list) or model not in accepted_models:
        raise ProtocolError(f"manifest recovery payload does not accept {model}")
    if not isinstance(record.get("bytes"), int) or not 0 < record["bytes"] <= 4 * 1024 * 1024:
        raise ProtocolError("manifest recovery payload size is invalid")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", str(record.get("sha256", ""))):
        raise ProtocolError("manifest recovery payload SHA-256 is invalid")
    if record.get("load_address") != 0x81000000 or record.get("entry_address") != 0x81000000:
        raise ProtocolError("manifest recovery payload load/entry address is invalid")
    if record.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise ProtocolError("manifest recovery payload lacks the corrected byte-zero entry contract")
    if record.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise ProtocolError("manifest recovery payload lacks scoped direct-member manifest parsing")
    if record.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise ProtocolError("manifest recovery payload lacks scratch read/write/restore preflight support")
    if record.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise ProtocolError("manifest recovery payload lacks the SPI master-enable correction")
    if record.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise ProtocolError("manifest recovery payload lacks PMOSREC v3 adaptive transport")
    return record


def validate_recovery_payload(path: Path, descriptor: PayloadDescriptor, bundle: BundleInfo) -> None:
    try:
        manifest = json.loads(bundle.manifest_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:  # defensive; validate_bundle parsed it
        raise ProtocolError(f"invalid release manifest: {exc}") from exc
    record = _recovery_payload_record(manifest, bundle.family, bundle.model)
    if descriptor.family != bundle.family or descriptor.family_id != FAMILY_ID[bundle.family]:
        raise ProtocolError("recovery payload SoC family does not match the selected target")
    if descriptor.spi_address != FAMILY_SPI_ADDRESS[bundle.family]:
        raise ProtocolError("recovery payload SPI register does not match the selected target")
    if descriptor.size != record["bytes"]:
        raise ProtocolError("recovery payload size does not match the release manifest")
    if descriptor.sha256.lower() != str(record["sha256"]).lower():
        raise ProtocolError("recovery payload SHA-256 does not match the release manifest")
    if descriptor.load_address != record["load_address"] or descriptor.entry_address != record["entry_address"]:
        raise ProtocolError("recovery payload entry addresses do not match the release manifest")
    if descriptor.entry_contract != record["entry_contract"]:
        raise ProtocolError("recovery payload entry contract does not match the release manifest")
    if descriptor.manifest_lookup_contract != record["manifest_lookup_contract"]:
        raise ProtocolError("recovery payload manifest lookup contract does not match the release manifest")
    if descriptor.hardware_preflight_contract != record["hardware_preflight_contract"]:
        raise ProtocolError("recovery payload preflight contract does not match the release manifest")
    if descriptor.spi_master_enable_contract != record["spi_master_enable_contract"]:
        raise ProtocolError("recovery payload SPI master-enable contract does not match the release manifest")
    if descriptor.adaptive_transport_contract != record.get("adaptive_transport_contract"):
        raise ProtocolError("recovery payload adaptive transport contract does not match the release manifest")


def validate_bundle(image: Path, manifest_path: Path, model: str, *, force: bool) -> BundleInfo:
    if model not in MODEL_FAMILY:
        raise ProtocolError(f"unsupported exact target model: {model}")
    if not image.is_file() or image.stat().st_size != FULL_IMAGE_SIZE:
        raise ProtocolError("pre-kernel recovery requires an exact 16 MiB full image")
    if not manifest_path.is_file():
        raise ProtocolError("pre-kernel recovery requires the release manifest")
    manifest_bytes = manifest_path.read_bytes()
    if not manifest_bytes or len(manifest_bytes) > 64 * 1024:
        raise ProtocolError("manifest must be from 1 through 65536 bytes")
    try:
        manifest = json.loads(manifest_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"invalid release manifest: {exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("target_family") != "vcore3":
        raise ProtocolError("manifest target_family must be vcore3")
    artifact = manifest.get("artifact")
    models = manifest.get("models")
    if not isinstance(artifact, dict) or not isinstance(models, dict):
        raise ProtocolError("manifest is missing artifact or model metadata")
    image_sha = sha256_file(image)
    if artifact.get("bytes") != FULL_IMAGE_SIZE:
        raise ProtocolError("manifest artifact size does not match the full image")
    if str(artifact.get("sha256", "")).lower() != image_sha.hex():
        raise ProtocolError("manifest artifact SHA-256 does not match the image")
    if artifact.get("boot_chain") != "vcoreiii-linuxloader-spim-v2":
        raise ProtocolError("manifest boot_chain is not supported by pre-kernel recovery")
    status = models.get(model)
    if status == "known-incompatible":
        raise ProtocolError(f"manifest marks {model} as known-incompatible")
    if status not in ALLOWED_MODEL_STATUS:
        raise ProtocolError(f"manifest has no supported compatibility status for {model}")
    if status == "untested" and not force:
        raise ProtocolError(f"{model} is untested in this artifact; force operation is required")
    with image.open("rb") as stream:
        loader = stream.read(LOADER_REGION_SIZE)
        stream.seek(ROOTFS_OFFSET)
        rootfs_magic = stream.read(4)
    _validate_loader_capability(manifest, hashlib.sha256(loader).hexdigest(), MODEL_FAMILY[model])
    _recovery_payload_record(manifest, MODEL_FAMILY[model], model)
    for marker in (b"PMOSRAM READY 2", b"PMOSBOOT MENU-PROBE", b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY"):
        if marker not in loader:
            raise ProtocolError(f"image bootloader region is missing meraki-redboot capability marker {marker!r}")
    validate_spim_kernel(image, artifact)
    if rootfs_magic != b"hsqs":
        raise ProtocolError("image rootfs region is missing the SquashFS header")
    return BundleInfo(
        image=image,
        manifest=manifest_path,
        model=model,
        family=MODEL_FAMILY[model],
        image_sha256=image_sha,
        manifest_sha256=hashlib.sha256(manifest_bytes).digest(),
        image_crc32=crc32_file(image),
        manifest_crc32=zlib.crc32(manifest_bytes) & 0xFFFFFFFF,
        manifest_bytes=manifest_bytes,
        model_status=status,
    )


class SerialLink:
    def __init__(self, fd: int, *, echo: bool = True) -> None:
        self.fd = fd
        self.echo = echo
        self.buffer = bytearray()

    def write_all(self, data: bytes, timeout: float = 10.0) -> None:
        view = memoryview(data)
        deadline = time.monotonic() + timeout
        while view:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError("serial write timed out")
            _, writable, _ = select.select([], [self.fd], [], min(remaining, 0.25))
            if not writable:
                continue
            try:
                count = os.write(self.fd, view)
            except (BlockingIOError, InterruptedError):
                continue
            if count <= 0:
                raise ProtocolError("serial write returned no progress")
            view = view[count:]

    def _read_once(self, timeout: float) -> bytes:
        """Read serial bytes without writing unclassified data to the terminal.

        A single kernel read can contain both an ASCII status line and the start
        of a binary payload.  Echoing that raw read can inject XOFF or terminal
        escape sequences into the operator's terminal while the transfer keeps
        running.  Text is echoed only after read_line() has isolated a complete
        protocol line; binary reads remain silent unless explicitly requested.
        """
        readable, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not readable:
            return b""
        try:
            return os.read(self.fd, 4096)
        except (BlockingIOError, InterruptedError):
            return b""

    @staticmethod
    def _safe_line_for_console(raw: bytes) -> str:
        payload = raw.rstrip(b"\r\n")
        return "".join(
            chr(value) if value == 0x09 or 0x20 <= value <= 0x7E
            else f"\\x{value:02x}"
            for value in payload
        )

    def discard_buffer(self) -> None:
        self.buffer.clear()
        try:
            while self._read_once(0.0):
                pass
        except OSError:
            pass

    def prepend_buffer(self, data: bytes) -> None:
        if data:
            self.buffer[:0] = data

    def drain_output(self) -> None:
        try:
            termios.tcdrain(self.fd)
        except OSError as exc:
            raise ProtocolError(f"serial output drain failed: {exc}") from exc

    def read_exact(self, length: int, timeout: float, *, echo: bool = False) -> bytes:
        if length < 0:
            raise ValueError("length must be non-negative")
        deadline = time.monotonic() + timeout
        output = bytearray()
        if self.buffer:
            take = min(length, len(self.buffer))
            output.extend(self.buffer[:take])
            del self.buffer[:take]
        while len(output) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError(f"timed out reading {length} binary bytes")
            readable, _, _ = select.select([self.fd], [], [], min(remaining, 0.25))
            if not readable:
                continue
            try:
                data = os.read(self.fd, min(65536, length - len(output)))
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                continue
            if echo:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
            output.extend(data)
        return bytes(output)

    def read_line(self, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                raw = bytes(self.buffer[: newline + 1])
                del self.buffer[: newline + 1]
                if self.echo:
                    print(self._safe_line_for_console(raw), flush=True)
                return raw.rstrip(b"\r\n").decode("utf-8", "replace")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError("timed out waiting for target response")
            data = self._read_once(min(remaining, 0.25))
            if data:
                self.buffer.extend(data)

    def wait_for(
        self,
        prefixes: tuple[str, ...],
        timeout: float,
        *,
        error_prefixes: tuple[str, ...] = (),
    ) -> str:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError(f"timed out waiting for: {', '.join(prefixes)}")
            line = self.read_line(remaining)
            if (
                line.startswith("PMOSRAM ABORT")
                or line.startswith("PMOSREC RESULT ERROR")
                or (error_prefixes and line.startswith(error_prefixes))
            ):
                raise ProtocolError(line)
            if line.startswith(prefixes):
                return line


def make_ram_header(payload: bytes, load: int, entry: int, chunk_size: int) -> bytes:
    if not (0x81000000 <= load < 0x87F00000):
        raise ProtocolError("load address is outside the supported DRAM staging range")
    if not payload or load + len(payload) > 0x87F00000:
        raise ProtocolError("RAM payload does not fit in the supported DRAM range")
    if not (load <= entry < load + len(payload)) or (load | entry) & 3:
        raise ProtocolError("entry/load address is unaligned or entry is outside the payload")
    if not 64 <= chunk_size <= 4096:
        raise ProtocolError("chunk size must be from 64 through 4096 bytes")
    base = RAM_HEADER.pack(
        RAM_MAGIC, PROTOCOL_VERSION, 0, load, entry, len(payload), chunk_size,
        zlib.crc32(payload) & 0xFFFFFFFF, hashlib.sha256(payload).digest(), 0,
    )
    return base[:-4] + struct.pack("<I", zlib.crc32(base[:-4]) & 0xFFFFFFFF)


def _send_frame_with_ack(
    link: SerialLink,
    frame: bytes,
    ack_prefix: str,
    nack_prefix: str,
    *,
    retries: int,
    ack_timeout: float,
    completion_prefix: str | None = None,
) -> str:
    response_prefixes = (ack_prefix, nack_prefix)
    if completion_prefix is not None:
        response_prefixes += (completion_prefix,)
    for attempt in range(retries + 1):
        link.write_all(frame, timeout=max(10.0, len(frame) / 4096.0))
        try:
            line = link.wait_for(response_prefixes, ack_timeout)
        except ProtocolError as exc:
            # Explicit target failures are terminal. Only a missing response is retryable.
            if "timed out" not in str(exc) or attempt >= retries:
                raise
            continue
        if line.startswith(ack_prefix) or (completion_prefix is not None and line.startswith(completion_prefix)):
            return line
        if attempt >= retries:
            raise ProtocolError(f"target repeatedly rejected frame: {line}")
    raise ProtocolError("frame retry limit exhausted")


def send_ram_payload(link: SerialLink, payload: bytes, load: int, entry: int,
                     chunk_size: int, retries: int, ack_timeout: float) -> str:
    header = make_ram_header(payload, load, entry, chunk_size)
    link.write_all(header)
    link.wait_for(("PMOSRAM HEADER-ACK",), 5.0)
    verified: str | None = None
    for sequence, offset in enumerate(range(0, len(payload), chunk_size)):
        data = payload[offset : offset + chunk_size]
        frame = RAM_FRAME.pack(RAM_FRAME_MAGIC, sequence, len(data), zlib.crc32(data) & 0xFFFFFFFF) + data
        seq = f"{sequence:08x}"
        final_frame = offset + len(data) == len(payload)
        response = _send_frame_with_ack(
            link, frame, f"PMOSRAM ACK {seq}", f"PMOSRAM NACK {seq}",
            retries=retries, ack_timeout=ack_timeout,
            completion_prefix="PMOSRAM VERIFIED" if final_frame else None,
        )
        if response.startswith("PMOSRAM VERIFIED"):
            verified = response
    if verified is None:
        verified = link.wait_for(("PMOSRAM VERIFIED",), 10.0)
    link.wait_for(("PMOSRAM EXEC",), 5.0)
    return verified


def make_preflight_header(*, scratch_address: int = DEFAULT_PREFLIGHT_SCRATCH,
                          scratch_size: int = PREFLIGHT_SCRATCH_BYTES,
                          pattern_seed: int = 0x504D4F53) -> bytes:
    if scratch_address < LOADER_REGION_SIZE:
        raise ProtocolError("preflight scratch address overlaps the protected bootloader region")
    if scratch_size != PREFLIGHT_SCRATCH_BYTES or scratch_address % scratch_size:
        raise ProtocolError("preflight scratch range must be one aligned 64 KiB erase block")
    if scratch_address + scratch_size > FULL_IMAGE_SIZE:
        raise ProtocolError("preflight scratch range exceeds the 16 MiB flash")
    base = PREFLIGHT_HEADER.pack(
        PREFLIGHT_MAGIC, PREFLIGHT_VERSION, PREFLIGHT_FLAG_RESTORE,
        scratch_address, scratch_size, pattern_seed & 0xFFFFFFFF, 0,
    )
    return base[:-4] + struct.pack("<I", zlib.crc32(base[:-4]) & 0xFFFFFFFF)


def make_package_header(bundle: BundleInfo, *, dry_run: bool, force: bool,
                        chunk_size: int) -> bytes:
    flags = FLAG_FULL_FLASH | (FLAG_DRY_RUN if dry_run else 0) | (FLAG_FORCE_UNTESTED if force else 0)
    model = bundle.model.encode("ascii")
    if len(model) > 15:
        raise ProtocolError("target model does not fit the wire header")
    base = PACKAGE_HEADER.pack(
        PACKAGE_MAGIC, PROTOCOL_VERSION, flags, FAMILY_ID[bundle.family],
        FULL_IMAGE_SIZE, len(bundle.manifest_bytes), chunk_size,
        bundle.image_crc32, bundle.manifest_crc32,
        bundle.image_sha256, bundle.manifest_sha256, model.ljust(16, b"\0"), 0,
    )
    return base[:-4] + struct.pack("<I", zlib.crc32(base[:-4]) & 0xFFFFFFFF)


def send_object(link: SerialLink, path: Path | None, data_bytes: bytes | None,
                object_id: int, chunk_size: int, retries: int, ack_timeout: float) -> None:
    if (path is None) == (data_bytes is None):
        raise ValueError("exactly one object source is required")
    total = path.stat().st_size if path is not None else len(data_bytes or b"")
    sent = 0
    sequence = 0
    verified = False
    stream = path.open("rb") if path is not None else None
    try:
        while sent < total:
            data = stream.read(chunk_size) if stream is not None else (data_bytes or b"")[sent : sent + chunk_size]
            if not data:
                raise ProtocolError("object source ended before its declared size")
            frame = PACKAGE_FRAME.pack(
                PACKAGE_FRAME_MAGIC, object_id, sequence, len(data), zlib.crc32(data) & 0xFFFFFFFF
            ) + data
            object_hex = f"{object_id:08x}"
            seq_hex = f"{sequence:08x}"
            final_frame = sent + len(data) == total
            response = _send_frame_with_ack(
                link, frame, f"PMOSPKG ACK {object_hex} {seq_hex}",
                f"PMOSPKG NACK {object_hex} {seq_hex}", retries=retries,
                ack_timeout=ack_timeout,
                completion_prefix=(f"PMOSPKG OBJECT-VERIFIED {object_hex}" if final_frame else None),
            )
            if response.startswith(f"PMOSPKG OBJECT-VERIFIED {object_hex}"):
                verified = True
            sent += len(data)
            sequence += 1
            percent = int(sent * 100 / total)
            if percent == 100 or percent % 5 == 0:
                print(f"object {object_id}: {sent}/{total} bytes ({percent}%)")
    finally:
        if stream is not None:
            stream.close()
    if not verified:
        link.wait_for((f"PMOSPKG OBJECT-VERIFIED {object_id:08x}",), 10.0)
