#!/usr/bin/env python3
"""Render representative genuine compiler CFGs next to stock Graphviz."""
import html
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'target/compiler-validation'
GALLERY = OUT / 'gallery'
GALLERY.mkdir(exist_ok=True)
env = dict(os.environ, GVBINDIR=str(ROOT / 'target/graphviz'))
samples = sorted((OUT / 'graphs').glob('*SpecialRPOScheduling.dot'))
samples += sorted((OUT / 'graphs').glob('compilerNested-*BuildGraph.dot'))
samples += sorted((OUT / 'graphs').glob('compilerSwitch-*BuildGraph.dot'))
samples += sorted((OUT / 'rust').glob('*SimplifyCfg-final.after.dot'))
samples += sorted((OUT / 'llvm').glob('.*.dot'))
parts = ['<!doctype html><meta charset="utf-8"><title>Real compiler graphs</title>',
         '<style>body{font:15px system-ui;margin:24px}section{display:flex;gap:20px}'
         'figure{width:48%;margin:0}img{width:100%}h2{margin-top:40px}</style>',
         '<h1>Real compiler graphs: Ion and dot</h1>',
         '<p>Click an image to inspect its full-size SVG. Sources and regeneration: scripts/compiler-check.sh.</p>']
for source in samples:
    name = source.stem.lstrip('.')
    parts.append(f'<h2>{html.escape(name)}</h2><section>')
    for engine in ['ion', 'dot']:
        filename = f'{name}.{engine}.svg'
        subprocess.run(['dot', f'-K{engine}', '-Tsvg', str(source), '-o', str(GALLERY / filename)], env=env, check=True)
        parts.append(f'<figure><figcaption>{engine}</figcaption><a href="{filename}"><img src="{filename}"></a></figure>')
    parts.append('</section>')
(GALLERY / 'index.html').write_text('\n'.join(parts))
print(GALLERY / 'index.html')
