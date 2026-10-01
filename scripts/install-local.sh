#!/usr/bin/env bash
# Build and register Ion in the isolated plugin directory. This intentionally
# leaves the system Graphviz installation untouched.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build.sh"
printf 'Run with: GVBINDIR=%q dot -Kion -Tsvg examples/diamond.dot\n' "$ROOT/target/graphviz"
