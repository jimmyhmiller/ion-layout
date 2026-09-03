#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cargo build --release

mkdir -p target/graphviz

# GVBINDIR replaces Graphviz's normal plugin directory; it does not extend it.
# Populate the isolated directory with links to the installed render, device,
# and text-layout plugins so `dot -Kion -Tsvg` can both lay out and render.
GRAPHVIZ_PLUGIN_DIR="$(pkg-config --variable=libdir libgvc)/graphviz"
if [[ ! -d "$GRAPHVIZ_PLUGIN_DIR" ]]; then
  echo "Graphviz plugin directory not found: $GRAPHVIZ_PLUGIN_DIR" >&2
  exit 1
fi
for plugin in "$GRAPHVIZ_PLUGIN_DIR"/libgvplugin_*; do
  [[ -e "$plugin" ]] || continue
  ln -sf "$plugin" "target/graphviz/$(basename "$plugin")"
done

cc -dynamiclib \
  -g \
  -o target/graphviz/libgvplugin_ion.8.dylib \
  plugin/gvplugin_ion.c \
  target/release/libion_layout.a \
  -Iinclude \
  $(pkg-config --cflags --libs libgvc libcgraph)

ln -sf libgvplugin_ion.8.dylib target/graphviz/libgvplugin_ion.dylib

# Generate a configuration containing both the standard plugins linked above
# and the locally built Ion layout engine.
GVBINDIR="$ROOT/target/graphviz" dot -c

echo "Built $ROOT/target/graphviz/libgvplugin_ion.8.dylib"
