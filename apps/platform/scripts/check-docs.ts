// Documentation drift guard. docs/api.md carries a generated reference block (routes, tables,
// environment variables, error codes) between markers; this check fails when
//   - the stored block differs from what `renderReference()` produces from the code today
//     (fix: `npm run docs:reference -- --write`),
//   - a route has no example / an error code has no explanation / a table or variable has no note
//     (`referenceProblems()`),
//   - the narrative or the block does not mention a route, environment variable or table, or
//   - docs/game-server-contract.md does not mention every /internal route.
// A new route or column without documentation fails `npm run docs:check` (and test/docs.test.ts).
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { getTableName, is } from 'drizzle-orm';
import { PgTable } from 'drizzle-orm/pg-core';
import { envSchema } from '../src/config.js';
import * as schema from '../src/db/schema.js';
import { listRoutes } from '../src/http/app.js';
import { referenceProblems, renderReference, storedReference } from './gen-reference.js';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

/** Problems found (empty = documentation is complete). */
export function checkDocs(): string[] {
  const api = readFileSync(join(root, 'docs', 'api.md'), 'utf8');
  const contract = readFileSync(join(root, 'docs', 'game-server-contract.md'), 'utf8');
  const problems: string[] = [...referenceProblems()];

  const stored = storedReference(api);
  if (stored === null) problems.push('docs/api.md has no reference markers');
  else if (stored.trimEnd() !== renderReference().trimEnd()) problems.push('the reference block in docs/api.md is out of date: run `npm run docs:reference -- --write`');

  const routes = listRoutes();
  if (routes.length < 40) problems.push(`only ${routes.length} routes were found: the route registry is broken`);
  for (const r of routes) {
    const line = `${r.method.toUpperCase()} ${r.path}`;
    if (!api.includes(line)) problems.push(`docs/api.md does not mention "${line}"`);
    if (r.path.startsWith('/internal/') && !contract.includes(line)) problems.push(`docs/game-server-contract.md does not mention "${line}"`);
  }

  const envNames = Object.keys(envSchema.shape);
  if (envNames.length < 30) problems.push('the environment schema looks empty');
  for (const name of envNames) {
    if (!api.includes(`\`${name}\``)) problems.push(`docs/api.md does not document the environment variable ${name}`);
  }

  const tables = Object.values(schema as Record<string, unknown>).filter((v): v is PgTable => is(v, PgTable));
  if (tables.length < 20) problems.push('the table list looks empty');
  for (const t of tables) {
    const name = getTableName(t);
    if (!api.includes(`#### \`${name}\``)) problems.push(`docs/api.md has no "#### \`${name}\`" section`);
  }
  return problems;
}

if (process.argv[1] !== undefined && fileURLToPath(import.meta.url) === process.argv[1]) {
  const problems = checkDocs();
  for (const p of problems) process.stderr.write(`${p}\n`);
  if (problems.length > 0) process.exit(1);
  process.stdout.write('documentation covers every route, environment variable and table\n');
}
