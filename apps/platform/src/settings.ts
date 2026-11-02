import { and, eq, sql } from 'drizzle-orm';
import { z } from 'zod';
import type { Db } from './db/index.js';
import { accountSettings } from './db/schema.js';
import { ApiError } from './util/errors.js';

// Per-account client settings: an opaque JSON object the launcher owns (game folder, language,
// start minimised ...). The platform stores it and does not interpret it, but bounds it so one
// account cannot store an arbitrary blob: size cap, depth cap, key charset, no prototype keys.

const MAX_DEPTH = 6;
const MAX_KEYS_PER_OBJECT = 200;
const MAX_ARRAY_LENGTH = 200;
const MAX_STRING_LENGTH = 2048;
const FORBIDDEN_KEYS = new Set(['__proto__', 'constructor', 'prototype']);
const KEY_PATTERN = /^[A-Za-z0-9_.:-]{1,64}$/;

/** Problems with a settings value (empty = valid). `maxBytes` caps the serialised size. */
export function validateSettings(value: unknown, maxBytes: number): string[] {
  const problems: string[] = [];
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return ['settings must be a JSON object'];
  const walk = (v: unknown, path: string, depth: number): void => {
    if (problems.length >= 5) return;
    if (depth > MAX_DEPTH) {
      problems.push(`${path}: nested deeper than ${MAX_DEPTH} levels`);
      return;
    }
    if (v === null || typeof v === 'boolean') return;
    if (typeof v === 'number') {
      if (!Number.isFinite(v)) problems.push(`${path}: number is not finite`);
      return;
    }
    if (typeof v === 'string') {
      if (v.length > MAX_STRING_LENGTH) problems.push(`${path}: string longer than ${MAX_STRING_LENGTH}`);
      return;
    }
    if (Array.isArray(v)) {
      if (v.length > MAX_ARRAY_LENGTH) problems.push(`${path}: array longer than ${MAX_ARRAY_LENGTH}`);
      v.slice(0, MAX_ARRAY_LENGTH).forEach((item, i) => walk(item, `${path}[${i}]`, depth + 1));
      return;
    }
    if (typeof v === 'object') {
      const entries = Object.entries(v as Record<string, unknown>);
      if (entries.length > MAX_KEYS_PER_OBJECT) problems.push(`${path}: more than ${MAX_KEYS_PER_OBJECT} keys`);
      for (const [k, item] of entries.slice(0, MAX_KEYS_PER_OBJECT)) {
        if (FORBIDDEN_KEYS.has(k) || !KEY_PATTERN.test(k)) problems.push(`${path}.${k}: key must match ${KEY_PATTERN} and not be a prototype name`);
        else walk(item, `${path}.${k}`, depth + 1);
      }
      return;
    }
    problems.push(`${path}: unsupported value type`);
  };
  walk(value, 'settings', 0);
  if (problems.length === 0 && Buffer.byteLength(JSON.stringify(value), 'utf8') > maxBytes) {
    problems.push(`settings larger than ${maxBytes} bytes`);
  }
  return problems;
}

export const putSettingsSchema = z.object({
  // z.record would silently DROP a `__proto__` key before validateSettings could refuse it, so the
  // shape is checked by hand and the keys are left for validateSettings to judge.
  settings: z
    .custom<Record<string, unknown>>((v) => typeof v === 'object' && v !== null && !Array.isArray(v), 'settings must be a JSON object')
    .meta({ type: 'object', description: 'The settings object (see "Settings" in docs/api.md for the limits)' }),
  /** Optimistic concurrency: the version the client last read (0 = no settings stored yet). Omit to overwrite. */
  expectedVersion: z.number().int().min(0).optional(),
});

export interface SettingsView {
  settings: Record<string, unknown>;
  /** 0 = nothing stored yet. Increments on every write. */
  version: number;
  updatedAt: Date | null;
}

export async function getSettings(db: Db, accountId: string): Promise<SettingsView> {
  const [row] = await db.select().from(accountSettings).where(eq(accountSettings.accountId, accountId)).limit(1);
  if (!row) return { settings: {}, version: 0, updatedAt: null };
  return { settings: row.data as Record<string, unknown>, version: row.version, updatedAt: row.updatedAt };
}

/**
 * Replace the account's settings. Validated and size-capped (400 `invalid_settings`). With
 * `expectedVersion` the write only happens if nobody wrote since (409 `version_conflict`, with
 * the current version in `details`), so two PCs cannot silently overwrite each other.
 */
export async function putSettings(
  db: Db,
  accountId: string,
  input: z.infer<typeof putSettingsSchema>,
  maxBytes: number,
  now: Date,
): Promise<SettingsView> {
  const problems = validateSettings(input.settings, maxBytes);
  if (problems.length > 0) throw new ApiError(400, 'invalid_settings', 'The settings are not acceptable.', { details: { problems } });

  const t = accountSettings;
  if (input.expectedVersion === undefined) {
    const [row] = await db
      .insert(t)
      .values({ accountId, data: input.settings, version: 1, updatedAt: now })
      .onConflictDoUpdate({ target: t.accountId, set: { data: input.settings, version: sql`${t.version} + 1`, updatedAt: now } })
      .returning();
    return { settings: row?.data as Record<string, unknown>, version: row?.version ?? 1, updatedAt: row?.updatedAt ?? now };
  }

  const expected = input.expectedVersion;
  const [row] =
    expected === 0
      ? await db.insert(t).values({ accountId, data: input.settings, version: 1, updatedAt: now }).onConflictDoNothing({ target: t.accountId }).returning()
      : await db
          .update(t)
          .set({ data: input.settings, version: expected + 1, updatedAt: now })
          .where(and(eq(t.accountId, accountId), eq(t.version, expected)))
          .returning();
  if (!row) {
    const current = await getSettings(db, accountId);
    throw new ApiError(409, 'version_conflict', 'The settings changed since you read them.', { details: { currentVersion: current.version } });
  }
  return { settings: row.data as Record<string, unknown>, version: row.version, updatedAt: row.updatedAt };
}
