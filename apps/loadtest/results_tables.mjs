// Prints the markdown tables of docs/perf/RESULTS.md from two collected RESULT files.
// usage: node results_tables.mjs <baseline_results.txt> <final_results.txt>
import { readFileSync } from 'node:fs';

function parse(path) {
  const rows = [];
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
    if (!line.startsWith('RESULT ')) continue;
    const o = {};
    for (const kv of line.slice(7).trim().split(/\s+/)) {
      const i = kv.indexOf('=');
      o[kv.slice(0, i)] = kv.slice(i + 1);
    }
    rows.push(o);
  }
  return rows;
}

const base = parse(process.argv[2]);
const fin = parse(process.argv[3]);
if (base.length !== 12 || fin.length !== 12) {
  console.error(`expected 12 rows each, got baseline ${base.length}, final ${fin.length}`);
  process.exit(1);
}
const n = (v) => Number(v);
const ms = (us) => { const v = n(us) / 1000; return v >= 100 ? `${v.toFixed(0)} ms` : v >= 10 ? `${v.toFixed(1)} ms` : `${v.toFixed(2)} ms`; };
const kb = (b) => `${(n(b) / 1000).toFixed(1)}`;
const mb = (b) => `${(n(b) / 1e6).toFixed(2)} MB/s`;
const pct = (v) => `${n(v).toFixed(1)} %`;

console.log('RESULTS TABLE (v2)');
console.log('| bots | scenario | joined | tick work avg | p99 | max | missed ticks | server CPU (of 1 core) | out total (payload) | out per client avg / p99 | WorldState packet avg (entries avg) | client gets world pkts/s (of 20) | coarse bytes/s (all clients) | deltas | refused frames |');
console.log('|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|');
for (const r of fin) {
  console.log(`| ${r.bots} | ${r.scenario} | ${r.joined} | ${ms(r.tick_avg_us)} | ${ms(r.tick_p99_us)} | ${ms(r.tick_max_us)} | ${r.missed} | ${pct(r.cpu_pct)} | ${mb(r.out_payload_Bps)} | ${kb(r.cl_out_avg)} / ${kb(r.cl_out_p99)} KB/s | ${r.world_avg_B} B (${n(r.entries_avg).toFixed(1)}) | ${n(r.cli_world_ps).toFixed(1)} | ${mb(r.coarse_B_ps)} | ${n(r.delta_pct).toFixed(1)} % | ${r.cli_rejected} |`);
}

console.log('\nBEFORE / AFTER');
console.log('| bots | scenario | tick work avg v1 -> v2 | missed v1 -> v2 | CPU v1 -> v2 | out total v1 -> v2 | per client avg v1 -> v2 | tick-work factor | bytes factor |');
console.log('|---:|---|---:|---:|---:|---:|---:|---:|---:|');
for (let i = 0; i < 12; i++) {
  const a = base[i], b = fin[i];
  if (a.bots !== b.bots || a.scenario !== b.scenario) { console.error(`row ${i} mismatch ${a.bots}/${a.scenario} vs ${b.bots}/${b.scenario}`); process.exit(1); }
  const fw = n(a.tick_avg_us) / n(b.tick_avg_us);
  const fb = n(a.out_payload_Bps) / n(b.out_payload_Bps);
  console.log(`| ${a.bots} | ${a.scenario} | ${ms(a.tick_avg_us)} -> ${ms(b.tick_avg_us)} | ${a.missed} -> ${b.missed} | ${pct(a.cpu_pct)} -> ${pct(b.cpu_pct)} | ${mb(a.out_payload_Bps)} -> ${mb(b.out_payload_Bps)} | ${kb(a.cl_out_avg)} -> ${kb(b.cl_out_avg)} KB/s | ${fw.toFixed(1)}x | ${fb.toFixed(1)}x |`);
}
