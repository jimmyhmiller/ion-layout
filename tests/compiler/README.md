These regression fixtures come from Node v26.5.0, V8 14.6.202.34-node.24,
compiling examples/compiler/v8.js with --trace-turbo. The adapter preserves
Turboshaft block IDs, operation IDs/titles, loop headers and predecessor edges.
It does not include instruction operand detail or dataflow edges.

Before preferred-route obstacle certification, these three phases produced
8 edge/node intersections. scripts/check.sh verifies them without requiring
Node's optimization intrinsics or a Rust nightly. scripts/compiler-check.sh
regenerates and checks all phases from the locally installed compilers and
creates target/compiler-validation/gallery/index.html for visual comparison.

v8-switch-build.dot additionally checks automatic head-slot changes on loop
return edges. Retaining the old channel corner used to introduce an overshoot
and a hooked bend. The semantic suite checks those return approaches directly.

llvm-nested.dot is native LLVM 22.1.8 dot-cfg output from
examples/compiler/cfg.c compiled at -O1. It checks arrow spacing and rounded
incoming stems on the two predecessors of block 16.
