#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
load_build_state
for cmd in tar rsync python3 patch make; do need "$cmd"; done

[[ -f "$KERNEL_HEADERS_TARBALL" ]] || die "Kernel headers are missing. Run make kernel first."
[[ -d "$SWITCH_DIR/.git" ]] || die "Kernel/OpenWrt source checkout is missing. Run make sources first."
[[ -d "$DONOR_ROOT/lib/modules" ]] || die "Donor modules are missing. Run make donor first."
[[ -f "$LOADER_ARTIFACT" ]] || die "Source-built meraki-redboot is missing. Run make loader first."

"$SCRIPT_DIR/verify-inputs.sh"

if [[ ! -f "$BUILDROOT_ARCHIVE" ]]; then
  if bool_enabled "${AUTO_DOWNLOAD_BUILDROOT:-0}" || ask_yes_no "Buildroot $BUILDROOT_VERSION is missing. Download it now?" yes; then
    download_file "https://buildroot.org/downloads/buildroot-$BUILDROOT_VERSION.tar.xz" "$BUILDROOT_ARCHIVE"
  else
    die "Buildroot archive is required."
  fi
fi

if [[ ! -d "$BUILDROOT_DIR" ]]; then
  log "Extracting Buildroot $BUILDROOT_VERSION"
  mkdir -p "$BUILD_DIR"
  tar -C "$BUILD_DIR" -xJf "$BUILDROOT_ARCHIVE"
fi

log "Synchronizing this repository's MS42P board integration"
rm -rf "$BUILDROOT_DIR/board/meraki"
mkdir -p "$BUILDROOT_DIR/board"
rsync -a "$REPO_ROOT/buildroot/board/meraki/" "$BUILDROOT_DIR/board/meraki/"

log "Synchronizing custom Buildroot packages"
# Remove conflicting package directories before synchronizing the authoritative recipes.
rm -rf "$BUILDROOT_DIR/package/clickswstatus" "$BUILDROOT_DIR/package/find_hdr" \
  "$BUILDROOT_DIR/package/postmerkos-cli"
for src in "$REPO_ROOT"/buildroot/packages/*; do
  [[ -d "$src" ]] || continue
  name="$(basename "$src")"
  rm -rf "$BUILDROOT_DIR/package/$name"
  rsync -a "$src/" "$BUILDROOT_DIR/package/$name/"
done
cp -f "$REPO_ROOT/buildroot/packages/Config.in" "$BUILDROOT_DIR/package/Config.in.ms42p"

python3 - "$BUILDROOT_DIR/package/Config.in" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
# Remove direct source entries that conflict with the package-managed implementation.
conflicting_entries = {
    'source "package/click/Config.in"',
    'source "package/clickswstatus/Config.in"',
    'source "package/status/Config.in"',
    'source "package/configd/Config.in"',
    'source "package/find_hdr/Config.in"',
    'source "package/findhdr/Config.in"',
    'source "package/pd690xx/Config.in"',
    'source "package/fwupdate/Config.in"',
    'source "package/postmerkos-console/Config.in"',
    'source "package/postmerkos-hardware/Config.in"',
    'source "package/postmerkos-cli/Config.in"',
    'source "package/Config.in.ms42p"',
}
s = '\n'.join(line for line in s.splitlines() if line.strip() not in conflicting_entries) + '\n'
start = '# BEGIN MS42P CUSTOM PACKAGES'
end = '# END MS42P CUSTOM PACKAGES'
block = f'{start}\nsource "package/Config.in.ms42p"\n{end}\n'
if start in s:
    before, rest = s.split(start, 1)
    _, after = rest.split(end, 1)
    s = before + block + after.lstrip('\n')
else:
    pos = s.rfind('endmenu')
    if pos < 0:
        raise SystemExit('Could not locate final endmenu in package/Config.in')
    s = s[:pos] + block + s[pos:]
p.write_text(s)
PY

# Apply repository-maintained Buildroot patches without depending on line numbers
# in package/Config.in. Registration is generated directly from the maintained package index.
for patch_file in "$REPO_ROOT"/buildroot/patches/*.patch; do
  [[ -f "$patch_file" ]] || continue
  if patch --dry-run -N -p0 -d "$BUILDROOT_DIR" < "$patch_file" >/dev/null 2>&1; then
    patch -N -p0 -d "$BUILDROOT_DIR" < "$patch_file"
  elif patch --dry-run -R -p0 -d "$BUILDROOT_DIR" < "$patch_file" >/dev/null 2>&1; then
    log "Patch already applied: $(basename "$patch_file")"
  else
    die "Patch does not apply cleanly: $patch_file"
  fi
done

log "Constructing generated binary overlay"
rm -rf "$GENERATED_OVERLAY"
mkdir -p "$GENERATED_OVERLAY"

# Keep a board-local copy inside the prepared Buildroot tree. The post-build
# script installs from this authoritative payload, so a stale/moved absolute
# BR2_ROOTFS_OVERLAY path cannot silently omit the vendor objects.
"$SCRIPT_DIR/stage-vendor-modules.sh" \
  "$GENERATED_OVERLAY/lib/modules" \
  "$BUILDROOT_DIR/board/meraki/ms220/vendor-modules"

case "${DONOR_ETC_POLICY:-modules}" in
  modules|none)
    ;;
  missing|fallback)
    [[ -d "$DONOR_ROOT/etc" ]] || die "Donor /etc is unavailable"
    mkdir -p "$GENERATED_OVERLAY/etc"
    rsync -a --no-links "$DONOR_ROOT/etc/" "$GENERATED_OVERLAY/etc/"
    ;;
  *) die "DONOR_ETC_POLICY must be modules or missing" ;;
esac

INCLUDE_UI_VALUE=0
if bool_enabled "${INCLUDE_UI:-0}"; then
  INCLUDE_UI_VALUE=1
  [[ -f "$BUILD_DIR/postmerkos-ui/index.html" ]] || die "UI output is missing. Run make ui first."
  mkdir -p "$GENERATED_OVERLAY/www"
  rsync -a --delete "$BUILD_DIR/postmerkos-ui/" "$GENERATED_OVERLAY/www/"
  rsync -a "$REPO_ROOT/buildroot/features/web-overlay/" "$GENERATED_OVERLAY/"
  find "$GENERATED_OVERLAY/www" -type d -exec chmod 0755 {} +
  find "$GENERATED_OVERLAY/www" -type f -exec chmod 0644 {} +
fi

CONFIG="$BUILDROOT_DIR/.config"
cp -f "$BUILDROOT_DIR/board/meraki/ms220/buildroot-config" "$CONFIG"
python3 - "$CONFIG" "$KERNEL_HEADERS_TARBALL" "$GENERATED_OVERLAY" \
  "$INCLUDE_UI_VALUE" "${INCLUDE_CLICKSWSTATUS:-0}" <<'PY'
from pathlib import Path
import re, sys
p = Path(sys.argv[1])
headers = Path(sys.argv[2]).resolve()
generated = Path(sys.argv[3]).resolve()
include_ui = sys.argv[4] == '1'
include_status = sys.argv[5].lower() in {'1','y','yes','true','on','enabled'}
s = p.read_text()

def set_string(key, value):
    global s
    line = f'{key}="{value}"'
    pat = rf'^(?:{re.escape(key)}=.*|# {re.escape(key)} is not set)$'
    if re.search(pat, s, flags=re.M):
        s = re.sub(pat, line, s, flags=re.M)
    else:
        s = s.rstrip() + '\n' + line + '\n'

def set_bool(key, enabled):
    global s
    line = f'{key}=y' if enabled else f'# {key} is not set'
    pat = rf'^(?:{re.escape(key)}=.*|# {re.escape(key)} is not set)$'
    if re.search(pat, s, flags=re.M):
        s = re.sub(pat, line, s, flags=re.M)
    else:
        s = s.rstrip() + '\n' + line + '\n'

set_string('BR2_KERNEL_HEADERS_CUSTOM_TARBALL_LOCATION', f'file://{headers}')
set_string('BR2_PRIMARY_SITE', 'https://sources.buildroot.net')
set_string('BR2_ROOTFS_OVERLAY', f'{generated} board/meraki/ms220/overlay')
set_string('BR2_ROOTFS_POST_BUILD_SCRIPT', 'board/meraki/ms220/post-build.sh')
set_string('BR2_ROOTFS_POST_FAKEROOT_SCRIPT', '')
set_string('BR2_ROOTFS_POST_IMAGE_SCRIPT', 'board/meraki/ms220/post-image.sh')
set_bool('BR2_STATIC_LIBS', False)
set_bool('BR2_SHARED_LIBS', False)
set_bool('BR2_SHARED_STATIC_LIBS', True)
set_bool('BR2_PACKAGE_PD690XX', True)
set_bool('BR2_PACKAGE_FWUPDATE', True)
set_bool('BR2_PACKAGE_FWUPDATE_CURL', True)
set_bool('BR2_PACKAGE_CONFIGD', True)
set_bool('BR2_PACKAGE_CONFIGD_WEBSOCKET', include_ui)
set_bool('BR2_PACKAGE_LIBWEBSOCKETS', include_ui)
set_bool('BR2_PACKAGE_POSTMERKOS_CONSOLE', True)
set_bool('BR2_PACKAGE_POSTMERKOS_HARDWARE', True)
set_bool('BR2_PACKAGE_POSTMERKOS_CLI', False)
set_bool('BR2_PACKAGE_JQ', False)
set_bool('BR2_PACKAGE_LINUX_PAM', False)
set_bool('BR2_PACKAGE_FLEX', False)
set_bool('BR2_TOOLCHAIN_BUILDROOT_WCHAR', False)
set_bool('BR2_TOOLCHAIN_BUILDROOT_LOCALE', False)
# Single-origin TLS: pmweb (mongoose + mbedTLS) is the HTTPS front and WS proxy;
# uhttpd is no longer used. pmweb selects mongoose + mbedtls via its Config.in.
set_bool('BR2_PACKAGE_UHTTPD', False)
set_bool('BR2_PACKAGE_PMWEB', include_ui)
set_bool('BR2_PACKAGE_STATUS', include_status)
p.write_text(s)
PY

(
  cd "$BUILDROOT_DIR"
  make olddefconfig
)

grep -q '^BR2_SHARED_STATIC_LIBS=y$' "$CONFIG" || die "Buildroot did not retain shared/static library support"
grep -q '^BR2_PACKAGE_PD690XX=y$' "$CONFIG" || die "Buildroot did not retain PD690XX"
grep -q '^BR2_PACKAGE_FWUPDATE=y$' "$CONFIG" || die "Buildroot did not retain FWUPDATE"
grep -q '^BR2_PACKAGE_FWUPDATE_CURL=y$' "$CONFIG" || die "Buildroot did not retain FWUPDATE_CURL"
grep -q '^BR2_PACKAGE_CONFIGD=y$' "$CONFIG" || die "Buildroot did not retain CONFIGD"
if (( INCLUDE_UI_VALUE )); then
  grep -q '^BR2_PACKAGE_CONFIGD_WEBSOCKET=y$' "$CONFIG" || die "Buildroot did not retain CONFIGD WebSocket support"
  grep -q '^BR2_PACKAGE_LIBWEBSOCKETS=y$' "$CONFIG" || die "Buildroot did not retain libwebsockets for the optional web UI"
else
  ! grep -q '^BR2_PACKAGE_CONFIGD_WEBSOCKET=y$' "$CONFIG" || die "Console-only build unexpectedly retained WebSocket support"
  ! grep -q '^BR2_PACKAGE_LIBWEBSOCKETS=y$' "$CONFIG" || die "Console-only build unexpectedly retained libwebsockets"
fi
grep -q '^BR2_PACKAGE_POSTMERKOS_CONSOLE=y$' "$CONFIG" || die "Buildroot did not retain POSTMERKOS_CONSOLE"
grep -q '^BR2_PACKAGE_POSTMERKOS_HARDWARE=y$' "$CONFIG" || die "Buildroot did not retain POSTMERKOS_HARDWARE"
grep -q '^BR2_PACKAGE_AVAHI=y$' "$CONFIG" || die "Buildroot did not retain Avahi"
grep -q '^BR2_PACKAGE_AVAHI_DAEMON=y$' "$CONFIG" || die "Buildroot did not retain the Avahi daemon"
grep -q '^BR2_PACKAGE_LIBDAEMON=y$' "$CONFIG" || die "Buildroot did not retain Avahi's libdaemon dependency"
grep -q '^BR2_PACKAGE_EXPAT=y$' "$CONFIG" || die "Buildroot did not retain Avahi's Expat dependency"
! grep -q '^BR2_PACKAGE_AVAHI_AUTOIPD=y$' "$CONFIG" || die "Avahi auto-IP must remain disabled"
! grep -q '^BR2_PACKAGE_AVAHI_DEFAULT_SERVICES=y$' "$CONFIG" || die "Avahi default service advertisements must remain disabled"
! grep -q '^BR2_PACKAGE_DBUS=y$' "$CONFIG" || die "D-Bus must remain disabled for the compact management-only mDNS build"
grep -q '^BR2_PACKAGE_JSON_C=y$' "$CONFIG" || die "Buildroot did not retain JSON-C"
grep -q '^BR2_PACKAGE_LIBCURL=y$' "$CONFIG" || die "Buildroot did not retain libcurl for HTTP(S)/SFTP firmware transport"
grep -q '^BR2_PACKAGE_MBEDTLS=y$' "$CONFIG" || die "Buildroot did not retain mbed TLS for verified HTTPS"
grep -q '^BR2_PACKAGE_LIBSSH2=y$' "$CONFIG" || die "Buildroot did not retain libssh2 for SFTP"
grep -q '^BR2_PACKAGE_CA_CERTIFICATES=y$' "$CONFIG" || die "Buildroot did not retain CA certificates"
if grep -q '^BR2_PACKAGE_JQ=y$' "$CONFIG"; then die "Buildroot unexpectedly retained jq for the console"; fi
! grep -q '^BR2_PACKAGE_LINUX_PAM=y$' "$CONFIG" || die "Linux-PAM must remain disabled for the 8 MiB image"
! grep -q '^BR2_TOOLCHAIN_BUILDROOT_WCHAR=y$' "$CONFIG" || die "uClibc wchar support must remain disabled for the compact image"
! grep -q '^BR2_TOOLCHAIN_BUILDROOT_LOCALE=y$' "$CONFIG" || die "uClibc locale support must remain disabled for the compact image"
if (( INCLUDE_UI_VALUE )); then
  grep -q '^BR2_PACKAGE_PMWEB=y$' "$CONFIG" || die "Buildroot did not retain pmweb"
fi

cat > "$STATE_FILE" <<EOF_STATE
INCLUDE_UI=$INCLUDE_UI_VALUE
INCLUDE_CLICKSWSTATUS=${INCLUDE_CLICKSWSTATUS:-0}
DONOR_ETC_POLICY=${DONOR_ETC_POLICY:-modules}
EOF_STATE

cat > "$ARTIFACTS_DIR/build-configuration.txt" <<EOF_CONFIG
repository: $(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf 'working-tree')
buildroot: $BUILDROOT_VERSION
kernel source: $(git -C "$SWITCH_DIR" rev-parse HEAD)
include UI: $INCLUDE_UI_VALUE
UI repository: $UI_REPO_URL
UI ref: $UI_REF
donor /etc policy: ${DONOR_ETC_POLICY:-modules}
standalone clickswstatus: ${INCLUDE_CLICKSWSTATUS:-0}
EOF_CONFIG

touch "$STAMP_DIR/buildroot-prepared"
log "Buildroot is configured in $BUILDROOT_DIR"
