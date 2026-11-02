import { z } from 'zod';
import type { ProductKind } from '../db/schema.js';

// The grant payload of a product: what a purchase delivers. It is validated when a product is
// created or edited and copied onto the order line at checkout, so changing a product later never
// changes what an existing order delivers.
//
//   wallet_credit  platform-held currency: { type, currency, amount }. The platform credits the
//                  wallet ledger itself when the order is paid (amount x line quantity); the game
//                  server only reads the balance and spends from it.
//   entitlement    a named status the game server applies: { type, key, durationDays? } (e.g. "vip").
//                  durationDays absent = permanent. Delivered through the grants queue.
//   item           an item or cosmetic: { type, key, quantity? } (x line quantity). Delivered
//                  through the grants queue.

const key = z.string().regex(/^[a-z0-9][a-z0-9_.:-]{0,63}$/);
const currencyCode = z.string().regex(/^[a-z][a-z0-9_]{1,15}$/);

export const grantEntrySchema = z.discriminatedUnion('type', [
  z.object({ type: z.literal('wallet_credit'), currency: currencyCode, amount: z.number().int().min(1).max(1_000_000_000) }).strict(),
  z.object({ type: z.literal('entitlement'), key, durationDays: z.number().int().min(1).max(3650).optional() }).strict(),
  z.object({ type: z.literal('item'), key, quantity: z.number().int().min(1).max(1_000_000).default(1) }).strict(),
]);

export const grantPayloadSchema = z.object({ grants: z.array(grantEntrySchema).min(1).max(20) }).strict();

export type GrantEntry = z.infer<typeof grantEntrySchema>;
export type GrantPayload = z.infer<typeof grantPayloadSchema>;

/** Which entry types a product kind may contain. A bundle may mix anything. */
const ALLOWED: Record<ProductKind, ReadonlyArray<GrantEntry['type']>> = {
  currency_pack: ['wallet_credit'],
  vip: ['entitlement'],
  cosmetic: ['item'],
  bundle: ['wallet_credit', 'entitlement', 'item'],
};

/** Parse `raw` as a grant payload for `kind`. Returns the payload or a list of human-readable problems. */
export function parsePayloadForKind(kind: ProductKind, raw: unknown): { ok: true; payload: GrantPayload } | { ok: false; problems: string[] } {
  const parsed = grantPayloadSchema.safeParse(raw);
  if (!parsed.success) return { ok: false, problems: parsed.error.issues.map((i) => `${i.path.join('.') || 'grantPayload'}: ${i.message}`) };
  const allowed = ALLOWED[kind];
  const bad = parsed.data.grants.filter((g) => !allowed.includes(g.type));
  if (bad.length > 0) {
    return { ok: false, problems: [`kind "${kind}" cannot contain ${[...new Set(bad.map((b) => b.type))].join(', ')} grants (allowed: ${allowed.join(', ')})`] };
  }
  return { ok: true, payload: parsed.data };
}
