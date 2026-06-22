#!/usr/bin/env python3
import re
from pathlib import Path

roots = [Path('README.md'), Path('docs')]
files=[]
for root in roots:
    if root.is_file(): files.append(root)
    elif root.is_dir(): files.extend(root.rglob('*.md'))
missing=[]
for path in files:
    text=path.read_text(errors='replace')
    for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', text):
        if target.startswith(('http://','https://','mailto:','#')):
            continue
        target=target.split('#',1)[0]
        if not target:
            continue
        resolved=(path.parent/target).resolve()
        if not resolved.exists():
            missing.append((str(path),target))
if missing:
    for source,target in missing:
        print(f'{source}: missing {target}')
    raise SystemExit(1)
print(f'documentation link check passed ({len(files)} Markdown files)')
