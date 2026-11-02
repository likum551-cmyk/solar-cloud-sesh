import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { PgDatabase, PgQueryResultHKT } from 'drizzle-orm/pg-core';
import pg from 'pg';
import { drizzle as drizzlePg } from 'drizzle-orm/node-postgres';
import { migrate as migratePg } from 'drizzle-orm/node-postgres/migrator';
import type { Logger } from '../util/logger.js';
import * as schema from './schema.js';

export * as schema from './schema.js';

/**
 * The query interface every module takes. Both drivers (node-postgres in production, PGlite in
 * tests and local dev) and drizzle transactions are PgDatabase, so a function that takes `Db` can
 * run inside `db.transaction(async (tx) => ...)` by being handed `tx`.
 */
export type Db = PgDatabase<PgQueryResultHKT, typeof schema>;

export interface DbHandle {
  db: Db;
  kind: 'pg' | 'pglite';
  /** Apply every pending migration from the drizzle/ folder. Run once per deploy (not per process). */
  migrate(): Promise<void>;
  /** Resolves when the database answers a trivial query. */
  ping(): Promise<void>;
  close(): Promise<void>;
}

/** The drizzle/ folder (holds meta/_journal.json), found by walking up from this module (src/ or dist/src/). */
export function findMigrationsFolder(): string {
  let dir = dirname(fileURLToPath(import.meta.url));
  for (let i = 0; i < 6; i++) {
    const candidate = join(dir, 'drizzle');
    if (existsSync(join(candidate, 'meta', '_journal.json'))) return candidate;
    dir = dirname(dir);
  }
  throw new Error('drizzle/ migrations folder not found');
}

/**
 * Open the database named by `url`:
 *   postgres://... | postgresql://...   node-postgres pool (production)
 *   pglite:memory                       in-process Postgres, discarded on exit (tests, dev)
 *   pglite:<directory>                  in-process Postgres persisted to a directory (dev)
 * PGlite is a devDependency: a production install without it fails here with a clear message.
 */
export async function openDatabase(url: string, opts: { poolMax?: number; logger?: Logger } = {}): Promise<DbHandle> {
  if (url.startsWith('pglite:')) {
    let mod: typeof import('@electric-sql/pglite');
    let drizzlePglite: typeof import('drizzle-orm/pglite');
    let migratorPglite: typeof import('drizzle-orm/pglite/migrator');
    try {
      mod = await import('@electric-sql/pglite');
      drizzlePglite = await import('drizzle-orm/pglite');
      migratorPglite = await import('drizzle-orm/pglite/migrator');
    } catch (err) {
      throw new Error(`pglite: URLs need the devDependency @electric-sql/pglite (${(err as Error).message})`);
    }
    const location = url.slice('pglite:'.length);
    const client = location === 'memory' || location === '' ? new mod.PGlite() : new mod.PGlite(location);
    await client.waitReady;
    const db = drizzlePglite.drizzle(client, { schema }) as unknown as Db;
    return {
      db,
      kind: 'pglite',
      migrate: () => migratorPglite.migrate(db as never, { migrationsFolder: findMigrationsFolder() }),
      ping: async () => void (await client.query('select 1')),
      close: () => client.close(),
    };
  }

  const pool = new pg.Pool({ connectionString: url, max: opts.poolMax ?? 10 });
  // An idle client erroring (server restart, network cut) must not crash the process: the pool
  // discards it and the next query opens a fresh connection.
  pool.on('error', (err) => opts.logger?.error('idle postgres client error', { err: err.message }));
  const db = drizzlePg(pool, { schema }) as unknown as Db;
  return {
    db,
    kind: 'pg',
    migrate: () => migratePg(db as never, { migrationsFolder: findMigrationsFolder() }),
    ping: async () => void (await pool.query('select 1')),
    close: () => pool.end(),
  };
}
