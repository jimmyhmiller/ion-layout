#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
command -v pkg-config >/dev/null || { echo 'pkg-config is required' >&2; exit 1; }
GRAPHVIZ_VERSION="$(pkg-config --modversion libgvc)"
case "$GRAPHVIZ_VERSION" in
  13.*) ;;
  *) echo "Ion currently supports Graphviz 13.x (found $GRAPHVIZ_VERSION); internal clipping APIs require a validated ABI." >&2; exit 1 ;;
esac
GRAPHVIZ_PLUGIN_DIR="${GRAPHVIZ_PLUGIN_DIR:-$(pkg-config --variable=libdir libgvc)/graphviz}"
[[ -d "$GRAPHVIZ_PLUGIN_DIR" ]] || { echo "Graphviz plugin directory not found: $GRAPHVIZ_PLUGIN_DIR" >&2; exit 1; }
# Detect the plugin ABI from the installed configuration rather than assuming
# it is tied to the Graphviz release version.
shopt -s nullglob
configs=("$GRAPHVIZ_PLUGIN_DIR"/config[0-9]*)
[[ ${#configs[@]} -eq 1 ]] || { echo 'Expected one installed Graphviz plugin configuration' >&2; exit 1; }
GRAPHVIZ_ABI="${configs[0]##*/config}"
case "$(uname -s)" in
  Darwin) link_flags=(-dynamiclib); plugin="libgvplugin_ion.${GRAPHVIZ_ABI}.dylib"; alias=libgvplugin_ion.dylib; system_libs=(-lm) ;;
  Linux) link_flags=(-shared -fPIC); plugin="libgvplugin_ion.so.${GRAPHVIZ_ABI}"; alias=libgvplugin_ion.so; system_libs=(-ldl -lpthread -lm) ;;
  *) echo 'Supported build platforms: macOS and Linux' >&2; exit 1 ;;
esac
cargo build --release
mkdir -p target/graphviz
for installed in "$GRAPHVIZ_PLUGIN_DIR"/libgvplugin_*; do
  [[ -e "$installed" ]] || continue
  ln -sf "$installed" "target/graphviz/$(basename "$installed")"
done
# pkg-config supplies a shell word list by contract; arrays preserve the
# arguments after splitting and prevent later accidental expansion.
read -r -a graphviz_flags <<< "$(pkg-config --cflags --libs libgvc libcgraph)"
"${CC:-cc}" "${link_flags[@]}" -g -Wall -Wextra -Werror \
  -o "target/graphviz/$plugin" plugin/gvplugin_ion.c plugin/compat.c \
  target/release/libion_layout.a -Iinclude -Iplugin \
  "${graphviz_flags[@]}" "${system_libs[@]}"
ln -sf "$plugin" "target/graphviz/$alias"
GVBINDIR="$ROOT/target/graphviz" dot -c
printf 'Built %s/target/graphviz/%s (Graphviz %s)\n' "$ROOT" "$plugin" "$GRAPHVIZ_VERSION"
