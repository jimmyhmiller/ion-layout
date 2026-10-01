#!/usr/bin/env node
// Convert V8's Turboshaft block graphs, preserving block IDs, operations and CFG edges.
import fs from 'node:fs';
import path from 'node:path';
const [input, out] = process.argv.slice(2);
if (!input || !out) throw new Error('usage: v8-json-to-dot.mjs <turbo.json> <output-directory>');
const trace = JSON.parse(fs.readFileSync(input, 'utf8'));
fs.mkdirSync(out, { recursive: true });
const quote = s => JSON.stringify(String(s));
let count = 0;
for (const phase of trace.phases.filter(p => p.type === 'turboshaft_graph')) {
  const {blocks, nodes} = phase.data;
  const ids = new Set(blocks.map(b => b.id));
  const operations = new Map(blocks.map(b => [b.id, []]));
  for (const n of nodes) {
    if (!operations.has(n.block_id)) throw new Error(`Unknown block ${n.block_id}`);
    operations.get(n.block_id).push(`${n.id}: ${n.title}`);
  }
  const lines = ['digraph G {', 'graph [rankdir=TB];', 'node [shape=box, fontname="Menlo", fontsize=11];',
    `label=${quote(`${trace.function.functionName} — ${phase.name}`)};`];
  for (const b of blocks) {
    const label = [`B${b.id} [${b.type}]`, ...operations.get(b.id)].map(s => String(s).replaceAll('\\', '\\\\').replaceAll('"', '\\"')).join('\\l') + '\\l';
    lines.push(`B${b.id} [label="${label}"${b.type === 'LOOP' ? ', ion_loop_header=true' : ''}];`);
    for (const pred of b.predecessors) {
      if (!ids.has(pred)) throw new Error(`Unknown predecessor ${pred}`);
      lines.push(`B${pred} -> B${b.id};`);
    }
  }
  lines.push('}');
  const name = `${trace.function.functionName}-${phase.name.replaceAll(/[^\w.-]/g, '_')}`;
  fs.writeFileSync(path.join(out, `${name}.dot`), lines.join('\n'));
  count++;
}
if (!count) throw new Error('No Turboshaft block graphs found');
console.log(`${path.basename(input)}: ${count} compiler phases`);
