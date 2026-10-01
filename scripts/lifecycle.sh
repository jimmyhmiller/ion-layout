#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
read -r -a graphviz_flags <<< "$(pkg-config --cflags --libs libgvc libcgraph)"
# Link the engine directly so this tests its real callback lifecycle with
# libgvc, independently of CLI plugin discovery. SANITIZE=1 is optional.
check_flags=(-g -Wall -Wextra -Werror)
if [[ "${SANITIZE:-0}" == 1 ]]; then check_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
system_libs=(-lm)
if [[ "$(uname -s)" == Linux ]]; then system_libs+=(-ldl -lpthread); fi
"${CC:-cc}" "${check_flags[@]}" -Iinclude -Iplugin \
  tests/lifecycle.c plugin/gvplugin_ion.c plugin/compat.c target/release/libion_layout.a \
  "${graphviz_flags[@]}" "${system_libs[@]}" -o target/lifecycle
GVBINDIR="$ROOT/target/graphviz" target/lifecycle
