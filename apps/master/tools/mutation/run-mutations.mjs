// Mutation sweep for apps/master/: apply one source mutation at a time, run vitest, record which
// tests fail, restore the file. Usage:
//   node run-mutations.mjs <masterDir> <mutations.json> <workDir> <summary.json> [idFilter,idFilter]
import { copyFileSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import path from 'node:path';

const [masterDir, mutationsFile, workDir, summaryFile, filterArg] = process.argv.slice(2);
if (!masterDir || !mutationsFile || !workDir || !summaryFile) {
  console.error('usage: run-mutations.mjs <masterDir> <mutations.json> <workDir> <summary.json> [ids]');
  process.exit(2);
}
const only = filterArg ? new Set(filterArg.split(',')) : null;
const srcDir = path.join(masterDir, 'src');
const backupDir = path.join(workDir, 'src-backup');
mkdirSync(backupDir, { recursive: true });

const sha = (buf) => createHash('sha256').update(buf).digest('hex');
const srcFiles = readdirSync(srcDir).filter((f) => f.endsWith('.ts'));
const original = new Map();
for (const f of srcFiles) {
  copyFileSync(path.join(srcDir, f), path.join(backupDir, f));
  original.set(f, sha(readFileSync(path.join(srcDir, f))));
}
const restoreAll = () => {
  for (const f of srcFiles) copyFileSync(path.join(backupDir, f), path.join(srcDir, f));
};

const vitestBin = path.join(masterDir, 'node_modules', 'vitest', 'vitest.mjs');
function runTests(label) {
  const out = path.join(workDir, `result-${label}.json`);
  const r = spawnSync(process.execPath, [vitestBin, 'run', '--reporter=json', `--outputFile=${out}`], {
    cwd: masterDir,
    encoding: 'utf8',
    timeout: 240000,
  });
  let data = null;
  try {
    data = JSON.parse(readFileSync(out, 'utf8'));
  } catch (err) {
    return { crashed: true, detail: `no JSON result (exit ${r.status}): ${String(err)} ${(r.stderr || '').slice(0, 400)}` };
  }
  const failed = [];
  for (const file of data.testResults ?? []) {
    const base = path.basename(file.name);
    if (file.status === 'failed' && (file.assertionResults ?? []).every((a) => a.status !== 'failed')) {
      failed.push(`${base}: <suite failed to run> ${(file.message || '').slice(0, 120)}`);
    }
    for (const a of file.assertionResults ?? []) if (a.status === 'failed') failed.push(`${base}: ${a.fullName}`);
  }
  return { crashed: false, total: data.numTotalTests, passed: data.numPassedTests, failedCount: data.numFailedTests, failed };
}

const summary = { baseline: null, results: [] };
try {
  summary.baseline = runTests('baseline');
  if (summary.baseline.crashed || summary.baseline.failedCount !== 0) {
    console.error('baseline is not green, aborting:', JSON.stringify(summary.baseline).slice(0, 600));
    writeFileSync(summaryFile, JSON.stringify(summary, null, 2));
    process.exitCode = 1;
  } else {
    console.log(`baseline: ${summary.baseline.passed}/${summary.baseline.total} passed`);
    const mutations = JSON.parse(readFileSync(mutationsFile, 'utf8'));
    for (const m of mutations) {
      if (only && !only.has(m.id)) continue;
      const target = path.join(srcDir, path.basename(m.file));
      const text = readFileSync(target, 'utf8');
      const parts = text.split(m.find);
      if (parts.length !== 2) {
        summary.results.push({ id: m.id, rule: m.rule, error: `find string matched ${parts.length - 1} times` });
        console.log(`${m.id} ERROR find matched ${parts.length - 1} times`);
        continue;
      }
      try {
        writeFileSync(target, parts.join(m.replace)); // split/join: replacement text is literal
        const r = runTests(m.id);
        const killed = r.crashed || r.failedCount > 0;
        summary.results.push({ id: m.id, rule: m.rule, killed, failedCount: r.failedCount ?? null, sample: (r.failed ?? []).slice(0, 4), crashed: r.crashed ? r.detail : undefined });
        console.log(`${m.id} ${killed ? 'KILLED  ' : 'SURVIVED'} failed=${r.failedCount} ${m.rule}`);
      } finally {
        copyFileSync(path.join(backupDir, path.basename(m.file)), target);
      }
    }
  }
} finally {
  restoreAll();
  const bad = [];
  for (const f of srcFiles) if (sha(readFileSync(path.join(srcDir, f))) !== original.get(f)) bad.push(f);
  summary.restored = bad.length === 0 ? 'all src files byte-identical to the originals' : `MISMATCH: ${bad.join(', ')}`;
  writeFileSync(summaryFile, JSON.stringify(summary, null, 2));
  console.log(summary.restored);
}
