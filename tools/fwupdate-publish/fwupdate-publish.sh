#!/usr/bin/env bash
set -Eeuo pipefail

MODE=modern

usage() {
    cat <<'USAGE'
usage: fwupdate-publish.sh [--modern|--checksum-only|--legacy] FIRMWARE.bin DESTINATION [VERSION] [DESCRIPTION]

Publish a complete postmerkOS firmware image to an HTTP(S) repository and
atomically update index.tsv.

Modern mode is the default and requires the current four-file artifact set:
  FIRMWARE.bin
  FIRMWARE.bin.sha256
  FIRMWARE.bin.manifest.json
  FIRMWARE.bin.manifest.json.sha256

Checksum-only mode publishes the original image and strict basename-matching
SHA-256 sidecar for the current updater's --no-manifest path. Legacy mode emits
the same two-file repository contract for older updater implementations.
USAGE
}

die() { printf 'fwupdate-publish: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required command is missing: $1"; }

while (($#)); do
    case $1 in
        --modern) MODE=modern; shift ;;
        --checksum-only|--original-artifacts) MODE=checksum; shift ;;
        --legacy) MODE=legacy; shift ;;
        --help|-h) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done

(($# >= 2 && $# <= 4)) || { usage >&2; exit 2; }
for command in awk install mv python3 realpath sha256sum stat; do need "$command"; done

[[ -f $1 ]] || die "firmware not found: $1"
firmware=$(realpath "$1")
dest=$2
version=${3:-}
description=${4:-postmerkOS VCore-III firmware}
name=$(basename -- "$firmware")
[[ $name =~ ^[A-Za-z0-9._-]+$ ]] || die "unsafe filename: $name"
[[ $description != *'|'* && $description != *$'\n'* && $description != *$'\r'* ]] || \
    die 'description may not contain pipes or newlines'
size=$(stat -c %s -- "$firmware")
[[ $size -eq 16777216 ]] || die "expected a complete 16 MiB firmware image, got $size bytes"

verify_sidecar() {
    local file=$1 sidecar=$2 expected listed actual
    [[ -f $sidecar ]] || return 1
    expected=$(awk 'NF >= 2 && $1 !~ /^#/ {print tolower($1); exit}' "$sidecar")
    listed=$(awk 'NF >= 2 && $1 !~ /^#/ {$1=""; sub(/^[ \t]+[*]?/, ""); sub(/\r$/, ""); print; exit}' "$sidecar")
    [[ $expected =~ ^[0-9a-f]{64}$ && $listed == "$(basename -- "$file")" ]] || return 1
    actual=$(sha256sum "$file" | awk '{print $1}')
    [[ $actual == "$expected" ]]
}

if [[ $MODE == modern ]]; then
    manifest="$firmware.manifest.json"
    manifest_sha="$manifest.sha256"
    verify_sidecar "$firmware" "$firmware.sha256" || die 'firmware SHA-256 sidecar is missing or invalid'
    verify_sidecar "$manifest" "$manifest_sha" || die 'artifact manifest or its SHA-256 sidecar is missing or invalid'
    manifest_version=$(python3 - "$manifest" <<'PY_VERSION'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    value = json.load(stream).get('version')
if not isinstance(value, str) or not value:
    raise SystemExit(1)
print(value)
PY_VERSION
) || die 'manifest has no valid version'
    [[ -n $version ]] || version=$manifest_version
    python3 - "$firmware" "$manifest" "$version" <<'PY'
import hashlib
import json
import os
import sys
image, manifest_path, requested_version = sys.argv[1:]
try:
    with open(manifest_path, encoding='utf-8') as stream:
        manifest = json.load(stream)
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f'invalid artifact manifest: {exc}')
artifact = manifest.get('artifact')
models = manifest.get('models')
if not isinstance(manifest.get('version'), str) or not manifest['version']:
    raise SystemExit('manifest has no valid version')
if requested_version and manifest['version'] != requested_version:
    raise SystemExit(f"requested version {requested_version!r} does not match manifest version {manifest['version']!r}")
if not isinstance(artifact, dict) or not isinstance(models, dict) or not models:
    raise SystemExit('manifest is missing artifact or model compatibility data')
if artifact.get('filename') != os.path.basename(image):
    raise SystemExit('manifest artifact filename mismatch')
if artifact.get('bytes') != os.path.getsize(image):
    raise SystemExit('manifest artifact size mismatch')
h = hashlib.sha256()
with open(image, 'rb') as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        h.update(block)
digest = h.hexdigest()
if artifact.get('sha256', '').lower() != digest:
    raise SystemExit('manifest artifact SHA-256 mismatch')
PY
else
    [[ -n $version ]] || { version=${name#ms42p-postmerkos-}; version=${version%.bin}; }
fi
[[ $version =~ ^[A-Za-z0-9._+-]+$ ]] || die "unsafe version: $version"

mkdir -p -- "$dest"
staging=$(mktemp -d "$dest/.publish.XXXXXX")
cleanup() { rm -rf -- "$staging"; }
trap cleanup EXIT HUP INT TERM
install -m 0644 "$firmware" "$staging/$name"
(cd "$staging" && sha256sum "$name" > "$name.sha256")
if [[ $MODE == modern ]]; then
    install -m 0644 "$firmware.manifest.json" "$staging/$name.manifest.json"
    (cd "$staging" && sha256sum "$name.manifest.json" > "$name.manifest.json.sha256")
fi

# Move completed files into place before publishing the catalog entry.
for file in "$staging"/*; do mv -f -- "$file" "$dest/$(basename -- "$file")"; done

index="$dest/index.tsv"
tmp="$dest/.index.tsv.$$"
{
    printf '# version|target|firmware filename|byte size|description\n'
    printf '%s|ms42p|%s|%s|%s\n' "$version" "$name" "$size" "$description"
    if [[ -f $index ]]; then
        awk -F'|' -v v="$version" -v n="$name" '
            /^#/ { next }
            !($1 == v && $2 == "ms42p") && $3 != n { print }
        ' "$index"
    fi
} > "$tmp"
mv -f -- "$tmp" "$index"

printf 'Published (%s): %s\n' "$MODE" "$dest/$name"
printf 'Checksum:       %s\n' "$dest/$name.sha256"
if [[ $MODE == modern ]]; then
    printf 'Manifest:       %s\n' "$dest/$name.manifest.json"
    printf 'Manifest hash:  %s\n' "$dest/$name.manifest.json.sha256"
fi
printf 'Index:          %s\n' "$index"
