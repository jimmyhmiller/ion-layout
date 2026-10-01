# Verification: how we know the layouts are right

Layout quality here is a tested property, not a vibe. Three independent
harnesses cross-check each other, plus visual tooling for human review.

## 1. Property tests — `cargo test`

`tests/properties.rs`, ported from upstream `generic-layout/test.ts` and
extended. ~650 graphs per run: fixed CFG shapes (chains, diamonds, loops,
nested loops, self-loops, multi-edges, disconnected, irreducible),
50–300-seed randomized DAGs, explicit-metadata loop graphs, and
metadata-free CFGs exercising loop inference. Invariants:

- no two node boxes overlap; same layer ⇒ same top y; layers strictly
  descend; everything inside the bounding box; positive coordinates
- layer monotonicity for forward edges; backedge blocks on their header's
  layer
- every edge gets a valid bezier route (3k+1 control points); routes start
  on the tail's boundary and the arrow tip lands on the head's boundary
- **no route passes through the interior of a foreign node** (each cubic is
  sampled; interiors shrunk 2pt)
- determinism: two runs produce identical output
- orientations: LR is the exact transpose of the size-swapped TB run; BT the
  exact mirror

## 2. Rendered-geometry checks — `node scripts/verify.mjs`

The property tests validate the core; this validates **what Graphviz will
actually draw**. Every `corpus/*.dot` (26 files: stress shapes, unicode,
long labels, clusters, undirected, rankdir variants, a real 150-block
SpiderMonkey graph, generated 120-node CFGs) is rendered with
`dot -Kion -Tjson` and machine-checked:

- C1 node-node overlap, C4 bounding box containment
- C2 label text fits inside its node (Graphviz's own text metrics)
- C3 edge splines sampled against all foreign node interiors
- C5 every edge has a spline and (when directed) an arrowhead
- C6 the **drawn shape polygon** matches the node's declared geometry — this
  exists because `-Tjson` reports `ND_width` while renderers draw the
  polygon, and a stale polygon once shipped boxes detached from their edges

`./scripts/check.sh` runs the build, core properties, rendered corpus, semantic
compatibility tests, and repeated libgvc lifecycle checks.

### Semantic compatibility and lifecycle

`scripts/compatibility.py` checks the meaning of Graphviz attributes rather
than just the existence of geometry: shape clipping, arrowless connectivity,
reverse arrows and sizing, record/HTML/compass ports, rank sets, minimum edge
length and excluded constraints, spacing/equal center separation, nested
clusters and compound edges in all four orientations, distinct incoming and
parallel routes, graph labels, collision-free edge labels, output formats,
and deterministic combined randomized graphs. Artifacts land in
`target/compatibility`.

`scripts/lifecycle.sh` exercises 100 layout/render/free cycles on reused
libgvc graphs and contexts. `SANITIZE=1` also instruments the C adapter with
AddressSanitizer and UBSan. libgvc owns graph initialization and final graph
cleanup; the plugin callback frees only its layout-specific resources.

Both macOS and Ubuntu 24.04 aarch64 passed the full functional suite with
Graphviz 13.1.2. macOS ASan/UBSan also passed. Linux LeakSanitizer reports
16,688 bytes / 468 allocations retained in the Pango/Fontconfig path; a
separate stock-dot baseline, with no Ion registration and complete cleanup,
reports 16,696 bytes / the same 468 allocations. This external font-state
issue is recorded in the project pad and is not suppressed by the harness.

The routing search starts in a local obstacle viewport and expands it only
when needed, checking every segment and certifying corner-rounding clearance.
Label collision tests in the adapter use recursive Bezier hull subdivision,
not fixed curve sampling.

## 3. The parity oracle — `./scripts/parity.sh`

The strongest guarantee: `scripts/parity.ts` runs the ORIGINAL
`generic-layout/layout.ts` (via the pinned tsx runner, from the `IONGRAPH` checkout) and this port
on **identical inputs** and requires **byte-exact node geometry** (position,
size, layer for every block). Cases:

- fixed CFG shapes and the iongraph demo CFG (`graph.json`)
- 100 randomized graphs (DAGs + explicit loop graphs)
- **mega-complex.json: every pass of every function** of a real 9.7MB
  SpiderMonkey dump — 281 real compiler graphs up to 69 blocks

Verified again during the compatibility work: **362 exact, 26 expected deviations, 0 failures**; the only non-exact cases are
auto-tagged deliberate deviations (below). All 281 real ion graphs match
byte-exactly.

Deviations the oracle knows about and tags instead of failing:

- **ports** — node too narrow for its output ports: we compress/widen
  (upstream lets routes start inside neighboring nodes)
- **shift** — anything left of the margin: we shift the drawing right
  (upstream renders at negative coordinates)
- **TSERR** — the original threw or hung on degraded input that we survive

## 4. Visual tooling

- `./scripts/gallery.sh` → `target/gallery/index.html`: every corpus graph,
  `-Kion` vs `-Kdot` side by side.
- `target/compare/index.html`: ours vs the original iongraph renderer on
  real mega-complex functions, synced scrolling + zoom. Node sizes legitimately
  differ (true font metrics vs the original's 6.5px/char estimate); the
  comparison is structure.

## Case study: when the same algorithm places a block differently

Worth recording because it *looks* like a porting bug and isn't. In
mega-complex f13, Block 66 sits ~2 columns left in the original render vs
ours, even though the parity oracle proves byte-exact equality on equal
inputs. Traced cause:

Block 66 is Block 59's **second successor** (the loop exit).
`straightenChildren` has a crossing guard: per layer, per round, a child is
only aligned if its layer position is right of the last child shifted this
round (`lastShifted`). B59's port-0 child (Block 60) sits to the *right* of
B66's dummy in layer order, so any round that shifts B60 blocks B66's
alignment. The pipeline runs exactly 2 rounds:

- with the original's 6.5px/char sizes, B59 was still drifting right between
  rounds, so B60 needed shifting in round 2 as well → B66 never got a turn →
  it stayed at the left margin;
- with real font metrics, B59 converged in round 1, round 2's B60 shift was
  a no-op, the guard stayed free → B66 aligned under B59's port.

So placements of loop-exit / later-port children are
**convergence-dependent**: sub-point node-size changes flip a discrete
alignment gate, not a proportional shift. Neither outcome is more correct;
with identical inputs the port reproduces the original's choice exactly
(verified both ways). Expect every "why is this block over there?"
difference vs the original viewer to be either node measurement or this.

## Debugging tools

- `ION_DEBUG_LNODES=1 ion-dump < graph.txt` — dump every layout node
  (layer, block, dummy target, flags, x, dst/src adjacency) and every block
  (layer, loop id, header/backedge, layout node).
- `ION_DEBUG_PASSES=1` — x snapshot after every straightening pass.
- `ION_DEBUG_CHILD=<block>` / `ION_DEBUG_CHILD_LI=<layer>` — log every
  `straightenChildren` decision (target, gate state, shift/skip) touching
  that block / on that layer.
- `ION_DUMP_INPUT=1 dot -Kion ...` — the plugin prints the exact layout
  input (sizes with full float precision, edges, metadata) in `ion-dump`'s
  stdin format, so any rendered graph can be replayed through the core
  byte-for-byte.
- `ion-dump` input format: `node <w> <h> <loop_depth> <header:0|1>
  <backedge:0|1>` per node, then `edge <tail> <head>`; output: `cell <i>
  <left> <top> <w> <h> <layer>` + `size <w> <h>`.

## Real compiler output

`./scripts/compiler-check.sh` generates fresh V8 Turboshaft block graphs from
`examples/compiler/v8.js`, native Rust MIR DOT from `examples/rustc-mir.rs`, and
native LLVM CFG DOT from `examples/compiler/cfg.c`. It requires Node with V8
TurboFan tracing, a Rust nightly, Clang and LLVM `opt` on PATH, plus the built
Ion plugin. It validates every generated compiler phase and renders selected
phases alongside stock dot in `target/compiler-validation/gallery/index.html`.
The V8 adapter preserves block/operation IDs, operation titles and CFG edges;
it does not render the separate sea-of-nodes dataflow graph or operand details.

On Node 26.5.0 (V8 14.6.202.34-node.24), Rust 1.96.0-nightly (2026-03-11),
and LLVM 22.1.8, 124 graphs passed: 24 V8 phases, 98 Rust MIR phases and two
LLVM CFGs. Visual inspection covered V8 switch and nested-loop phases, Rust
HTML-table blocks, and LLVM record labels with branch ports. Three V8 graphs
that exposed preferred-route collisions are checked in under `tests/compiler`
and run by the regular check script.

A fourth V8 fixture checks hooked bends on loop returns after automatic input
slots change. The semantic suite verifies the return edges never rise past
their final attachment height. Waypoint-routing tests also require continuous
Bézier tangents where outside-channel legs join.

Incoming-arrow readability regressions check visible spacing between arrow
silhouettes at arrowsize 0.5, 1 and 2 in TB, BT, LR and RL layouts. The captured
native LLVM nested-loop CFG also checks longer final stems and continuous
curve tangents. Automatic incoming slots reserve space for the arrow size and
line width, use staggered approach heights, and round attachment segments
together with the rest of the path. Explicit DOT ports retain their specified
attachment positions.
