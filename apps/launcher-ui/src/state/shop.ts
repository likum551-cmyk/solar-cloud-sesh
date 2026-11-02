import type { OwnedItem, Order, Product } from '../lib/schemas';

// Pure shop helpers: order stamps, money, VIP time and which showroom model a product gets.

export type StampTone = 'stamp' | 'pending' | 'void' | 'plain';

export interface Stamp {
  /** The word printed on the record: PAID, DELIVERED, REFUNDED, FAILED, ... */
  label: string;
  /** stamp = settled and kept (a "done" chip); plain = settled and given back (refunded);
   *  pending = not settled yet; void = failed, reversed or expired. */
  tone: StampTone;
}

/** The stamp an order carries on the record. Pending orders past their expiry read EXPIRED. */
export function stampFor(order: Pick<Order, 'status' | 'expiresAt'>, now: number): Stamp {
  switch (order.status) {
    case 'pending':
      return Date.parse(order.expiresAt) < now ? { label: 'EXPIRED', tone: 'void' } : { label: 'AWAITING PAYMENT', tone: 'pending' };
    case 'paid': return { label: 'PAID', tone: 'stamp' };
    case 'fulfilled': return { label: 'DELIVERED', tone: 'stamp' };
    case 'refunded': return { label: 'REFUNDED', tone: 'plain' };
    case 'failed': return { label: 'FAILED', tone: 'void' };
    case 'chargeback': return { label: 'REVERSED', tone: 'void' };
    default: return { label: order.status.toUpperCase(), tone: 'pending' };
  }
}

/**
 * What the account has paid, per currency, in minor units: paid and delivered orders only (a
 * refund, a chargeback, a failed or an open order is not money spent). Currencies in first-seen
 * order; none when nothing was paid.
 */
export function spentByCurrency(orders: readonly Pick<Order, 'status' | 'totalMinor' | 'currency'>[]): { currency: string; totalMinor: number }[] {
  const sums = new Map<string, number>();
  for (const o of orders) {
    if (o.status !== 'paid' && o.status !== 'fulfilled') continue;
    sums.set(o.currency, (sums.get(o.currency) ?? 0) + o.totalMinor);
  }
  return [...sums].map(([currency, totalMinor]) => ({ currency, totalMinor }));
}

/** Fraction digits of an ISO 4217 currency (EUR 2, JPY 0); 2 for an unknown code. */
export function currencyDigits(currency: string): number {
  try {
    return new Intl.NumberFormat('en', { style: 'currency', currency }).resolvedOptions().maximumFractionDigits ?? 2;
  } catch {
    return 2;
  }
}

/** Format an amount in minor units, e.g. (1999, 'EUR') -> "€19.99". */
export function formatMoney(minor: number, currency: string, locale = 'en'): string {
  const digits = currencyDigits(currency);
  const value = minor / 10 ** digits;
  try {
    return new Intl.NumberFormat(locale, { style: 'currency', currency, minimumFractionDigits: digits, maximumFractionDigits: digits }).format(value);
  } catch {
    return `${value.toFixed(digits)} ${currency}`;
  }
}

export type VipWindow =
  | { kind: 'none' }
  | { kind: 'permanent' }
  | { kind: 'timed'; startedAt: number; endsAt: number; remainingMs: number; totalMs: number };

/**
 * VIP time derived from owned `entitlement` grants with key "vip": each grant runs
 * `durationDays x quantity` from the later of its grant time and the end of the previous one
 * (VIP stacks). The platform does not report an expiry yet (NATIVE-INTEGRATION.md, API gaps),
 * so this is the client's reading of the ledger.
 */
export function vipWindow(items: OwnedItem[], now: number): VipWindow {
  const vip = items
    .filter((i) => i.type === 'entitlement' && i.payload.key === 'vip')
    .sort((a, b) => Date.parse(a.createdAt) - Date.parse(b.createdAt));
  if (vip.length === 0) return { kind: 'none' };
  let start: number | null = null;
  let end = 0;
  for (const g of vip) {
    const days = typeof g.payload.durationDays === 'number' ? g.payload.durationDays : null;
    if (days === null) return { kind: 'permanent' };
    const t = Date.parse(g.createdAt);
    const from = Math.max(t, end);
    if (start === null || t > end) start = from;
    end = from + days * Math.max(1, g.quantity) * 86_400_000;
  }
  if (start === null || end <= now) return { kind: 'none' };
  return { kind: 'timed', startedAt: start, endsAt: end, remainingMs: end - now, totalMs: end - start };
}

/** Which showroom subject a product is shown with; 'none' = a bare table (nothing on sale). */
export type ShowroomModel = 'car' | 'outfit' | 'watch' | 'emblem' | 'coin' | 'none';

/**
 * The 3D subject for a product: by the first item grant's key prefix (vehicle.*, outfit.*),
 * then by kind. The platform has no per-product model field yet; this mapping is the contract
 * until it does (docs/api/launcher-ui.md).
 */
export function showroomModelFor(p: Pick<Product, 'kind' | 'grants'>): ShowroomModel {
  const item = p.grants.find((g): g is { type: 'item'; key: string } => g.type === 'item' && typeof (g as { key?: unknown }).key === 'string');
  if (item?.key.startsWith('vehicle.')) return 'car';
  if (item?.key.startsWith('outfit.')) return 'outfit';
  if (item) return 'watch';
  if (p.kind === 'vip') return 'emblem';
  return 'coin';
}

/** Human label for a product kind. */
export function kindLabel(kind: string, p?: Pick<Product, 'kind' | 'grants'>): string {
  if (p) {
    const m = showroomModelFor(p);
    if (m === 'car') return 'Vehicle';
    if (m === 'outfit') return 'Outfit';
  }
  switch (kind) {
    case 'currency_pack': return 'Currency';
    case 'vip': return 'VIP';
    case 'cosmetic': return 'Cosmetic';
    case 'bundle': return 'Bundle';
    default: return kind;
  }
}
