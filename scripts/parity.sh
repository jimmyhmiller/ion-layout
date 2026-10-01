#!/usr/bin/env bash
# Run the parity oracle: diff this port's layout against the original
# essence.ts on examples, real ion dumps, and randomized in-domain graphs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IONGRAPH="${IONGRAPH:-$ROOT/target/upstream/iongraph}"
if [[ ! -f "$IONGRAPH/generic-layout/layout.ts" ]]; then
  echo 'Set IONGRAPH to an upstream iongraph checkout (generic-layout/layout.ts is required).' >&2
  exit 1
fi
export IONGRAPH

cd "$ROOT"
cargo build --release --quiet --bin ion-dump

# npx resolves the pinned runner without depending on an arbitrary old cache
# entry. tsx imports the supplied upstream checkout directly.
cd "$IONGRAPH"
npx --yes --package tsx@4.20.5 tsx --stack-size=8000 "$ROOT/scripts/parity.ts"
