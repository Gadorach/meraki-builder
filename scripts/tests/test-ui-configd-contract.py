#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} /path/to/postmerkos-ui")
ui = Path(sys.argv[1])
if not (ui / "src").is_dir():
    raise SystemExit(f"UI source directory not found: {ui / 'src'}")
ui_text = "\n".join(p.read_text(errors="replace") for p in (ui / "src").rglob("*") if p.is_file())
ws = Path("buildroot/packages/configd/websocket.c").read_text(errors="replace")
ui_methods = set(re.findall(r"\brequest\(\s*['\"]([A-Za-z0-9_.:-]+)['\"]", ui_text))
backend_methods = set(re.findall(r"strcmp\([^,]+,\s*\"([A-Za-z0-9_.:-]+)\"\)", ws))
missing = sorted(ui_methods - backend_methods)
if missing:
    raise SystemExit("UI methods missing in configd: " + ", ".join(missing))
required = {"hello", "auth", "config", "replace_config", "firmware_upload_start", "firmware_upload_finish", "firmware_begin_flash"}
if not required.issubset(ui_methods):
    raise SystemExit("UI is missing required methods: " + ", ".join(sorted(required-ui_methods)))
api = (ui / "src/api.js").read_text(errors="replace")
if "configd-ws" not in api or "'wss'" not in api or "'ws'" not in api:
    raise SystemExit("UI does not implement dynamic WS/WSS with configd-ws")
menus = (ui / "src/menus.jsx").read_text(errors="replace")
if "manifest" not in menus.lower() or "artifact" not in ws.lower():
    raise SystemExit("browser manifest/artifact-name handoff is incomplete")
print(f"UI/configd contract passed ({len(ui_methods)} UI request methods)")
