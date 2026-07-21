#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

select_node() {
  local major=0 machine node_arch archive tools
  if command -v node >/dev/null 2>&1; then
    major="$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || printf '0')"
  fi
  if [[ "$major" =~ ^[0-9]+$ ]] && (( major >= 20 )) && command -v npm >/dev/null 2>&1; then
    return
  fi

  machine="$(uname -m)"
  case "$machine" in
    x86_64|amd64) node_arch=x64 ;;
    aarch64|arm64) node_arch=arm64 ;;
    *) die "No Node.js >=20 is installed and portable Node is unsupported on $machine" ;;
  esac

  tools="$BUILD_DIR/tools"
  archive="$tools/node-v$NODE_VERSION-linux-$node_arch.tar.xz"
  mkdir -p "$tools"
  if [[ ! -f "$archive" ]]; then
    log "Downloading portable Node.js $NODE_VERSION"
    download_file \
      "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-$node_arch.tar.xz" \
      "$archive"
  fi
  if [[ ! -x "$tools/node-v$NODE_VERSION-linux-$node_arch/bin/node" ]]; then
    tar -C "$tools" -xJf "$archive"
  fi
  export PATH="$tools/node-v$NODE_VERSION-linux-$node_arch/bin:$PATH"
  need node
  need npm
}

clone_or_update_git_ref "$UI_REPO_URL" "$UI_DIR" "$UI_REF" "postmerkos-ui"
ui_revision_before="$(git -C "$UI_DIR" rev-parse HEAD)"
[[ -z "$(git -C "$UI_DIR" status --porcelain)" ]] ||   die "postmerkos-ui checkout is not clean before build"
select_node

log "Building postmerkos-ui from authoritative origin/$RESOLVED_GIT_SYMBOLIC_REF"
# Production firmware owns its WebSocket endpoint. Host shell VITE_* values
# must never leak into the compiled appliance UI.
while IFS='=' read -r name _; do
  [[ "$name" == VITE_* ]] && unset "$name"
done < <(env)
(
  cd "$UI_DIR"
  npm_config_cache="$DOWNLOAD_DIR/npm-cache" npm ci --no-audit --no-fund
  npm run build
)

[[ -f "$UI_DIR/build/index.html" ]] || die "UI build did not produce build/index.html"
[[ "$(git -C "$UI_DIR" rev-parse HEAD)" == "$ui_revision_before" ]] ||   die "postmerkos-ui revision changed during build"
if ! git -C "$UI_DIR" diff --quiet || ! git -C "$UI_DIR" diff --cached --quiet; then
  die "postmerkos-ui tracked source changed during build; commit changes upstream instead of modifying them in meraki-builder"
fi
rm -rf "$BUILD_DIR/postmerkos-ui"
mkdir -p "$BUILD_DIR/postmerkos-ui"
rsync -a --delete "$UI_DIR/build/" "$BUILD_DIR/postmerkos-ui/"

git -C "$UI_DIR" rev-parse HEAD > "$ARTIFACTS_DIR/ui-source-revision.txt"
python3 - "$ARTIFACTS_DIR/ui-source.json" "$UI_REPO_URL" "$UI_REF" \
  "$RESOLVED_GIT_SYMBOLIC_REF" "$ui_revision_before" "$RESOLVED_GIT_DESCRIBE" <<'PY_UI'
import json, sys
from pathlib import Path
out, repository, requested, resolved, revision, describe = sys.argv[1:]
Path(out).write_text(json.dumps({
    "project": "Gadorach/postmerkos-ui",
    "repository": repository,
    "requested_ref": requested,
    "resolved_ref": resolved,
    "revision": revision,
    "describe": describe,
    "resolution": "authoritative-git",
}, indent=2, sort_keys=True) + "\n")
PY_UI
# Single-origin: the UI connects same-origin to /ws (proxied by pmweb) using the
# configd-ws subprotocol; it no longer hardcodes the :4001 port.
if ! grep -R -a -q 'configd-ws' "$BUILD_DIR/postmerkos-ui"; then
  die "production UI does not contain the configd-ws WebSocket subprotocol"
fi
if ! grep -R -a -q '/ws' "$BUILD_DIR/postmerkos-ui"; then
  die "production UI does not contain the same-origin /ws WebSocket path"
fi
find "$BUILD_DIR/postmerkos-ui" -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$ARTIFACTS_DIR/ui-files.sha256"
touch "$STAMP_DIR/ui-built"
log "UI output is ready in $BUILD_DIR/postmerkos-ui"
