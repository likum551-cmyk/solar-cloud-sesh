import { and, asc, desc, eq, inArray, lt, or, sql } from 'drizzle-orm';
import { z } from 'zod';
import type { Deps } from '../deps.js';
import type { Db } from '../db/index.js';
import {
  accounts,
  grants,
  orderItems,
  orders,
  paymentEvents,
  products,
  walletBalances,
  walletLedger,
  PRODUCT_KINDS,
  ORDER_STATUSES,
  type OrderStatus,
  type ProductKind,
} from '../db/schema.js';
import { writeAudit, type AdminCtx } from '../admin/service.js';
import { assertNotBanned, findAccountById, type AccountRow } from '../auth/accounts.js';
import { enforce } from '../auth/throttle.js';
import { hasOpenSession } from '../game/join.js';
import type { GameServerRow } from '../servers/service.js';
import { ApiError, isCheckViolation, isForeignKeyViolation, isUniqueViolation } from '../util/errors.js';
import { errorFields } from '../util/logger.js';
import { grantPayloadSchema, parsePayloadForKind } from './payload.js';
import { WebhookVerificationError, type PaymentProvider, type WebhookEvent } from './provider.js';

type ProductRow = typeof products.$inferSelect;
type OrderRow = typeof orders.$inferSelect;
type OrderItemRow = typeof orderItems.$inferSelect;

const PLATFORM = 'platform';

/**
 * Failure reasons a verified payment.succeeded may still undo: our own expiry timer and an earlier
 * failed attempt (providers let a customer retry the same checkout). amount_mismatch and
 * checkout_error never revive: a human decides. The orders_status_transition trigger allows
 * failed -> paid for exactly these reasons.
 */
const REVIVABLE_FAILURES: string[] = ['expired', 'payment_failed'];

/** An order that can still be paid: pending, or failed for a revivable reason. */
const openForPayment = (orderId: string) =>
  and(eq(orders.id, orderId), or(eq(orders.status, 'pending'), and(eq(orders.status, 'failed'), inArray(orders.failureReason, REVIVABLE_FAILURES))));

export function activeProvider(deps: Deps): PaymentProvider {
  const p = deps.payments.find((x) => x.id === deps.config.paymentProvider);
  if (!p) throw new Error(`payment provider "${deps.config.paymentProvider}" is not registered`);
  return p;
}

// ---------------------------------------------------------------------------------------------
// Products

const sku = z.string().regex(/^[a-z0-9][a-z0-9_.-]{1,62}$/);
const productFields = {
  sku,
  name: z.string().trim().min(1).max(80),
  description: z.string().trim().max(2000),
  /** Minor units (cents). */
  priceMinor: z.number().int().min(1).max(100_000_000),
  currency: z.string().regex(/^[A-Z]{3}$/),
  kind: z.enum(PRODUCT_KINDS),
  grantPayload: z.unknown(),
  active: z.boolean(),
  sort: z.number().int().min(-1_000_000).max(1_000_000),
  imageUrl: z.string().url().max(500).refine((u) => /^https:\/\//i.test(u), 'must be an https URL').nullable(),
  maxQuantity: z.number().int().min(1).max(100),
};

function checkPayload(kind: ProductKind, raw: unknown, ctx: z.RefinementCtx): void {
  const r = parsePayloadForKind(kind, raw);
  if (!r.ok) for (const p of r.problems) ctx.addIssue({ code: 'custom', path: ['grantPayload'], message: p });
}

export const productCreateSchema = z
  .object({
    ...productFields,
    // Defaults only on create: on update an absent key must mean "leave unchanged".
    description: productFields.description.default(''),
    active: productFields.active.default(true),
    sort: productFields.sort.default(0),
    imageUrl: productFields.imageUrl.default(null),
    maxQuantity: productFields.maxQuantity.default(1),
  })
  .superRefine((p, ctx) => checkPayload(p.kind, p.grantPayload, ctx));

export const productUpdateSchema = z
  .object(productFields)
  .partial()
  .refine((o) => Object.keys(o).length > 0, 'at least one field is required');

export interface ProductView {
  id: string;
  sku: string;
  name: string;
  description: string;
  priceMinor: number;
  currency: string;
  kind: ProductKind;
  /** What the purchase delivers (validated payload). */
  grants: z.infer<typeof grantPayloadSchema>['grants'];
  active: boolean;
  sort: number;
  imageUrl: string | null;
  maxQuantity: number;
}

function productView(p: ProductRow): ProductView {
  return {
    id: p.id, sku: p.sku, name: p.name, description: p.description, priceMinor: p.priceMinor, currency: p.currency,
    kind: p.kind as ProductKind, grants: grantPayloadSchema.parse(p.grantPayload).grants, active: p.active,
    sort: p.sort, imageUrl: p.imageUrl, maxQuantity: p.maxQuantity,
  };
}

export async function listProducts(db: Db, opts: { activeOnly: boolean }): Promise<ProductView[]> {
  const rows = await db.select().from(products).where(opts.activeOnly ? eq(products.active, true) : undefined).orderBy(asc(products.sort), asc(products.name));
  return rows.map(productView);
}

export async function createProduct(deps: Deps, ctx: AdminCtx, input: z.infer<typeof productCreateSchema>): Promise<ProductView> {
  const now = deps.clock();
  const payload = parsePayloadForKind(input.kind, input.grantPayload);
  if (!payload.ok) throw new ApiError(400, 'invalid_request', payload.problems.join('; '));
  try {
    return await deps.db.transaction(async (tx) => {
      const [row] = await tx
        .insert(products)
        .values({ ...input, grantPayload: payload.payload, createdAt: now, updatedAt: now })
        .returning();
      if (!row) throw new Error('product insert returned no row');
      await writeAudit(tx, ctx.actor, { action: 'product.create', targetType: 'product', targetId: row.id, details: { sku: row.sku }, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
      return productView(row);
    });
  } catch (err) {
    if (isUniqueViolation(err, 'products_sku_uq')) throw new ApiError(409, 'sku_exists', 'A product with that SKU already exists.');
    throw err;
  }
}

export async function updateProduct(deps: Deps, ctx: AdminCtx, id: string, input: z.infer<typeof productUpdateSchema>): Promise<ProductView> {
  const now = deps.clock();
  return deps.db.transaction(async (tx) => {
    const [current] = await tx.select().from(products).where(eq(products.id, id)).for('update');
    if (!current) throw new ApiError(404, 'product_not_found', 'No such product.');
    // The kind and payload must stay consistent with each other whichever of them changes.
    const kind = (input.kind ?? current.kind) as ProductKind;
    const payload = parsePayloadForKind(kind, input.grantPayload !== undefined ? input.grantPayload : current.grantPayload);
    if (!payload.ok) throw new ApiError(400, 'invalid_request', payload.problems.join('; '));
    try {
      const [row] = await tx
        .update(products)
        .set({ ...input, kind, grantPayload: payload.payload, updatedAt: now })
        .where(eq(products.id, id))
        .returning();
      if (!row) throw new ApiError(404, 'product_not_found', 'No such product.');
      await writeAudit(tx, ctx.actor, { action: 'product.update', targetType: 'product', targetId: id, details: { fields: Object.keys(input) }, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
      return productView(row);
    } catch (err) {
      if (isUniqueViolation(err, 'products_sku_uq')) throw new ApiError(409, 'sku_exists', 'A product with that SKU already exists.');
      throw err;
    }
  });
}

/** Delete a product nobody has ordered. A product with orders can only be deactivated (409 `product_in_use`). */
export async function deleteProduct(deps: Deps, ctx: AdminCtx, id: string): Promise<void> {
  const now = deps.clock();
  try {
    await deps.db.transaction(async (tx) => {
      const rows = await tx.delete(products).where(eq(products.id, id)).returning({ id: products.id });
      if (rows.length === 0) throw new ApiError(404, 'product_not_found', 'No such product.');
      await writeAudit(tx, ctx.actor, { action: 'product.delete', targetType: 'product', targetId: id, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
    });
  } catch (err) {
    if (err instanceof ApiError) throw err;
    if (isForeignKeyViolation(err)) {
      throw new ApiError(409, 'product_in_use', 'Orders reference this product; deactivate it instead.');
    }
    throw err;
  }
}

// ---------------------------------------------------------------------------------------------
// Orders and checkout

export interface OrderView {
  id: string;
  status: OrderStatus;
  currency: string;
  totalMinor: number;
  provider: string;
  /** Present while the order is pending. */
  checkoutUrl: string | null;
  failureReason: string | null;
  createdAt: Date;
  expiresAt: Date;
  paidAt: Date | null;
  fulfilledAt: Date | null;
  refundedAt: Date | null;
  items: { lineNo: number; sku: string; name: string; kind: string; quantity: number; unitPriceMinor: number }[];
}

function orderView(o: OrderRow, items: OrderItemRow[]): OrderView {
  return {
    id: o.id,
    status: o.status as OrderStatus,
    currency: o.currency,
    totalMinor: o.totalMinor,
    provider: o.provider,
    checkoutUrl: o.status === 'pending' ? o.checkoutUrl : null,
    failureReason: o.failureReason,
    createdAt: o.createdAt,
    expiresAt: o.expiresAt,
    paidAt: o.paidAt,
    fulfilledAt: o.fulfilledAt,
    refundedAt: o.refundedAt,
    items: items.map((i) => ({ lineNo: i.lineNo, sku: i.sku, name: i.name, kind: i.kind, quantity: i.quantity, unitPriceMinor: i.unitPriceMinor })),
  };
}

async function loadOrderView(db: Db, o: OrderRow): Promise<OrderView> {
  const items = await db.select().from(orderItems).where(eq(orderItems.orderId, o.id)).orderBy(asc(orderItems.lineNo));
  return orderView(o, items);
}

export const checkoutSchema = z.object({
  items: z.array(z.object({ sku, quantity: z.number().int().min(1).max(100) })).min(1).max(10),
});
export const idempotencyKeySchema = z.string().regex(/^[A-Za-z0-9_-]{8,64}$/);

/**
 * Create an order for the account and a checkout session at the payment provider.
 *
 * Banned accounts cannot buy; an unverified email cannot buy when REQUIRE_VERIFIED_EMAIL=1. With
 * an Idempotency-Key a retried request returns the original order (the same lines are required,
 * else 422 `idempotency_mismatch`) and finishes the provider call if the first attempt died
 * before it. The order is committed before the provider is called, so a crash leaves a pending
 * order that the maintenance job expires, never a payment without an order.
 */
export async function checkout(deps: Deps, account: AccountRow, input: z.infer<typeof checkoutSchema>, idempotencyKey: string | null): Promise<OrderView> {
  const now = deps.clock();
  await enforce(deps.db, `checkout:${account.id}`, 10, 3600, now);
  if (deps.config.requireVerifiedEmail && account.emailVerifiedAt === null) {
    throw new ApiError(403, 'email_not_verified', 'Verify your email address before buying.');
  }
  await assertNotBanned(deps.db, account.id, now);

  // Merge duplicate SKUs into one line.
  const wanted = new Map<string, number>();
  for (const it of input.items) wanted.set(it.sku, (wanted.get(it.sku) ?? 0) + it.quantity);

  if (idempotencyKey !== null) {
    const existing = await findOrderByKey(deps.db, account.id, idempotencyKey);
    if (existing) return resumeCheckout(deps, account, existing, wanted);
  }

  const rows = await deps.db.select().from(products).where(inArray(products.sku, [...wanted.keys()]));
  const bySku = new Map(rows.map((r) => [r.sku, r]));
  let currency: string | null = null;
  let total = 0;
  for (const [s, qty] of wanted) {
    const p = bySku.get(s);
    if (!p) throw new ApiError(404, 'product_not_found', `No such product: ${s}`);
    if (!p.active) throw new ApiError(409, 'product_inactive', `Product is not on sale: ${s}`);
    if (qty > p.maxQuantity) throw new ApiError(422, 'quantity_exceeds_limit', `At most ${p.maxQuantity} of ${s} per order.`);
    if (currency !== null && currency !== p.currency) throw new ApiError(422, 'mixed_currency', 'All items in an order must use the same currency.');
    currency = p.currency;
    total += p.priceMinor * qty;
  }
  if (currency === null) throw new ApiError(400, 'invalid_request', 'The order has no items.');

  let order: OrderRow;
  try {
    order = await deps.db.transaction(async (tx) => {
      const [o] = await tx
        .insert(orders)
        .values({
          accountId: account.id,
          currency: currency as string,
          totalMinor: total,
          provider: activeProvider(deps).id,
          idempotencyKey,
          createdAt: now,
          updatedAt: now,
          expiresAt: new Date(now.getTime() + deps.config.orderTtlMinutes * 60_000),
        })
        .returning();
      if (!o) throw new Error('order insert returned no row');
      let line = 0;
      for (const [s, qty] of wanted) {
        const p = bySku.get(s) as ProductRow;
        await tx.insert(orderItems).values({
          orderId: o.id, lineNo: line++, productId: p.id, sku: p.sku, name: p.name, kind: p.kind,
          unitPriceMinor: p.priceMinor, currency: p.currency, quantity: qty, grantPayload: p.grantPayload,
        });
      }
      return o;
    });
  } catch (err) {
    if (idempotencyKey !== null && isUniqueViolation(err, 'orders_idempotency_uq')) {
      const existing = await findOrderByKey(deps.db, account.id, idempotencyKey);
      if (existing) return resumeCheckout(deps, account, existing, wanted);
    }
    throw err;
  }
  return startProviderCheckout(deps, account, order);
}

async function findOrderByKey(db: Db, accountId: string, key: string): Promise<OrderRow | null> {
  const [row] = await db.select().from(orders).where(and(eq(orders.accountId, accountId), eq(orders.idempotencyKey, key))).limit(1);
  return row ?? null;
}

async function resumeCheckout(deps: Deps, account: AccountRow, order: OrderRow, wanted: Map<string, number>): Promise<OrderView> {
  const items = await deps.db.select().from(orderItems).where(eq(orderItems.orderId, order.id));
  const same = items.length === wanted.size && items.every((i) => wanted.get(i.sku) === i.quantity);
  if (!same) throw new ApiError(422, 'idempotency_mismatch', 'This Idempotency-Key was used for a different order.');
  if (order.status === 'pending' && order.providerRef === null) return startProviderCheckout(deps, account, order);
  return orderView(order, items);
}

async function startProviderCheckout(deps: Deps, account: AccountRow, order: OrderRow): Promise<OrderView> {
  const provider = deps.payments.find((p) => p.id === order.provider) ?? activeProvider(deps);
  const items = await deps.db.select().from(orderItems).where(eq(orderItems.orderId, order.id)).orderBy(asc(orderItems.lineNo));
  try {
    const session = await provider.createCheckout({
      orderId: order.id,
      accountId: account.id,
      currency: order.currency,
      totalMinor: order.totalMinor,
      items: items.map((i) => ({ sku: i.sku, name: i.name, quantity: i.quantity, unitPriceMinor: i.unitPriceMinor })),
      customerEmail: account.email,
      expiresAt: order.expiresAt,
    });
    const [updated] = await deps.db
      .update(orders)
      .set({ providerRef: session.providerRef, checkoutUrl: session.checkoutUrl, updatedAt: deps.clock() })
      .where(eq(orders.id, order.id))
      .returning();
    return orderView(updated ?? order, items);
  } catch (err) {
    deps.logger.error('checkout creation failed at the payment provider', { orderId: order.id, provider: provider.id, ...errorFields(err) });
    await deps.db
      .update(orders)
      .set({ status: 'failed', failureReason: 'checkout_error', updatedAt: deps.clock() })
      .where(and(eq(orders.id, order.id), eq(orders.status, 'pending')));
    throw new ApiError(503, 'payment_unavailable', 'The payment provider is unavailable. Try again later.');
  }
}

export async function listOrders(deps: Deps, accountId: string, limit = 50): Promise<OrderView[]> {
  const rows = await deps.db.select().from(orders).where(eq(orders.accountId, accountId)).orderBy(desc(orders.createdAt)).limit(limit);
  return Promise.all(rows.map((o) => loadOrderView(deps.db, o)));
}

export async function getOrder(deps: Deps, orderId: string, accountId: string | null): Promise<OrderView> {
  const [row] = await deps.db.select().from(orders).where(accountId === null ? eq(orders.id, orderId) : and(eq(orders.id, orderId), eq(orders.accountId, accountId))).limit(1);
  if (!row) throw new ApiError(404, 'order_not_found', 'No such order.');
  return loadOrderView(deps.db, row);
}

export const adminOrdersQuerySchema = z.object({
  status: z.enum(ORDER_STATUSES).optional(),
  accountId: z.guid().optional(),
  limit: z.coerce.number().int().min(1).max(200).default(50),
});

export async function listAdminOrders(deps: Deps, q: z.infer<typeof adminOrdersQuerySchema>): Promise<(OrderView & { accountId: string })[]> {
  const conds = [];
  if (q.status) conds.push(eq(orders.status, q.status));
  if (q.accountId) conds.push(eq(orders.accountId, q.accountId));
  const rows = await deps.db.select().from(orders).where(conds.length ? and(...conds) : undefined).orderBy(desc(orders.createdAt)).limit(q.limit);
  return Promise.all(rows.map(async (o) => ({ ...(await loadOrderView(deps.db, o)), accountId: o.accountId })));
}

// ---------------------------------------------------------------------------------------------
// Fulfilment: paid -> grants (and wallet credits); refund/chargeback -> revokes

/**
 * pending (or a revivable failed) -> paid, then create the grants of every order line. Wallet credits are applied by the
 * platform in the same transaction (grant row born `acked` by "platform" + a ledger credit keyed
 * `grant:<id>`); item and entitlement grants stay `pending` for the game server to pull. An order
 * with nothing left to deliver goes straight to `fulfilled`; otherwise it becomes `fulfilled`
 * when the last pending grant is acked. Returns false if the order was not open for payment.
 */
async function markPaid(tx: Db, order: OrderRow, now: Date): Promise<boolean> {
  const [paid] = await tx
    .update(orders)
    .set({ status: 'paid', paidAt: now, failureReason: null, updatedAt: now })
    .where(openForPayment(order.id))
    .returning({ id: orders.id });
  if (!paid) return false;

  const lines = await tx.select().from(orderItems).where(eq(orderItems.orderId, order.id)).orderBy(asc(orderItems.lineNo));
  let awaitingServer = 0;
  for (const line of lines) {
    const payload = grantPayloadSchema.parse(line.grantPayload);
    for (let entryNo = 0; entryNo < payload.grants.length; entryNo++) {
      const entry = payload.grants[entryNo];
      if (!entry) continue;
      const byPlatform = entry.type === 'wallet_credit';
      const [g] = await tx
        .insert(grants)
        .values({
          accountId: order.accountId, orderId: order.id, lineNo: line.lineNo, entryNo, kind: 'grant',
          type: entry.type, sku: line.sku, payload: entry, quantity: line.quantity,
          status: byPlatform ? 'acked' : 'pending', createdAt: now,
          ackedAt: byPlatform ? now : null, ackedBy: byPlatform ? PLATFORM : null,
        })
        .returning({ id: grants.id });
      if (!g) throw new Error('grant insert returned no row');
      if (entry.type === 'wallet_credit') {
        await tx.insert(walletLedger).values({
          accountId: order.accountId, currency: entry.currency, delta: entry.amount * line.quantity,
          reason: 'purchase', idempotencyKey: `grant:${g.id}`, grantId: g.id, createdAt: now,
        });
      } else {
        awaitingServer++;
      }
    }
  }
  if (awaitingServer === 0) await markFulfilled(tx, order.id, now);
  return true;
}

async function markFulfilled(tx: Db, orderId: string, now: Date): Promise<void> {
  await tx
    .update(orders)
    .set({ status: 'fulfilled', fulfilledAt: now, updatedAt: now })
    .where(and(eq(orders.id, orderId), eq(orders.status, 'paid')));
}

/**
 * paid|fulfilled -> refunded|chargeback, and a revoke entry for every grant of the order
 * (unique per revoked grant, so it can only happen once). A wallet credit is clawed back by a
 * negative ledger entry that may leave the balance below zero (debt, paid off by later
 * purchases); item/entitlement revokes are queued for the game server. Returns false if the
 * order was not in a revocable state.
 */
async function revokeOrder(tx: Db, order: OrderRow, to: 'refunded' | 'chargeback', now: Date): Promise<boolean> {
  const [moved] = await tx
    .update(orders)
    .set({ status: to, updatedAt: now, ...(to === 'refunded' ? { refundedAt: now } : {}) })
    .where(and(eq(orders.id, order.id), inArray(orders.status, ['paid', 'fulfilled'])))
    .returning({ id: orders.id });
  if (!moved) return false;

  const original = await tx.select().from(grants).where(and(eq(grants.orderId, order.id), eq(grants.kind, 'grant'))).orderBy(asc(grants.seq));
  for (const g of original) {
    const byPlatform = g.type === 'wallet_credit';
    const [r] = await tx
      .insert(grants)
      .values({
        accountId: g.accountId, orderId: g.orderId, lineNo: g.lineNo, entryNo: g.entryNo, kind: 'revoke', revokesGrantId: g.id,
        type: g.type, sku: g.sku, payload: g.payload, quantity: g.quantity,
        status: byPlatform ? 'acked' : 'pending', createdAt: now, ackedAt: byPlatform ? now : null, ackedBy: byPlatform ? PLATFORM : null,
      })
      .returning({ id: grants.id });
    if (!r) throw new Error('revoke insert returned no row');
    if (byPlatform) {
      const entry = g.payload as { currency: string; amount: number };
      await tx.insert(walletLedger).values({
        accountId: g.accountId, currency: entry.currency, delta: -(entry.amount * g.quantity),
        reason: 'revoke', idempotencyKey: `revoke:${g.id}`, grantId: r.id, createdAt: now,
      });
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Webhooks

export interface WebhookOutcome {
  received: true;
  /** True when this event id was seen before: nothing was done. */
  duplicate: boolean;
  result: 'applied' | 'ignored' | 'unmatched' | 'rejected' | 'duplicate';
}

/**
 * Verify and apply a provider webhook. Exactly-once by construction: (provider, event_id) is
 * unique, the event row and every effect commit in one transaction, and order transitions are
 * conditional UPDATEs under a row lock, so a redelivery, a concurrent twin or a late event cannot
 * double-apply. Every verified event gets HTTP 200 whatever its result (providers retry on
 * anything else); the result is recorded on the payment_events row. A bad signature throws
 * 400 `invalid_webhook` and records nothing.
 */
export async function handleWebhook(deps: Deps, providerId: string, rawBody: string, headers: Record<string, string>): Promise<WebhookOutcome> {
  const provider = deps.payments.find((p) => p.id === providerId);
  if (!provider) throw new ApiError(404, 'unknown_provider', 'No such payment provider.');
  let event: WebhookEvent;
  try {
    event = provider.verifyWebhook({ rawBody, headers });
  } catch (err) {
    if (err instanceof WebhookVerificationError) {
      deps.logger.warn('webhook rejected', { provider: providerId, reason: err.message });
      throw new ApiError(400, 'invalid_webhook', 'Webhook verification failed.');
    }
    throw err;
  }
  const now = deps.clock();

  return deps.db.transaction(async (tx): Promise<WebhookOutcome> => {
    const [row] = await tx
      .insert(paymentEvents)
      .values({ provider: provider.id, eventId: event.eventId, type: event.type, orderId: event.orderId, result: 'applied', payload: event, receivedAt: now })
      .onConflictDoNothing({ target: [paymentEvents.provider, paymentEvents.eventId] })
      .returning({ id: paymentEvents.id });
    if (!row) return { received: true, duplicate: true, result: 'duplicate' };

    const [order] = await tx
      .select()
      .from(orders)
      .where(and(eq(orders.id, event.orderId), eq(orders.provider, provider.id)))
      .for('update');

    let result: 'applied' | 'ignored' | 'unmatched' | 'rejected';
    let detail: string | null = null;
    if (!order) {
      result = 'unmatched';
      detail = 'no order with that id for this provider';
      deps.logger.warn('webhook for an unknown order', { provider: provider.id, eventId: event.eventId, orderId: event.orderId });
    } else {
      ({ result, detail } = await applyEvent(deps, tx, order, event, now));
    }
    await tx.update(paymentEvents).set({ result, detail }).where(eq(paymentEvents.id, row.id));
    return { received: true, duplicate: false, result };
  });
}

async function applyEvent(
  deps: Deps,
  tx: Db,
  order: OrderRow,
  event: WebhookEvent,
  now: Date,
): Promise<{ result: 'applied' | 'ignored' | 'rejected'; detail: string | null }> {
  const ignored = (detail: string) => {
    deps.logger.warn('webhook event ignored', { orderId: order.id, status: order.status, type: event.type, detail });
    return { result: 'ignored' as const, detail };
  };
  switch (event.type) {
    case 'payment.succeeded': {
      // A failed order whose failure was only OUR timer (expired) or an earlier failed attempt
      // (payment_failed) is revived by a verified success: the customer has paid, so they get the goods.
      const late = order.status === 'failed' && order.failureReason !== null && REVIVABLE_FAILURES.includes(order.failureReason);
      if (order.status !== 'pending' && !late) {
        if (order.status === 'failed') {
          deps.logger.error('payment received for a failed order that cannot be revived: refund it at the provider', { orderId: order.id, failureReason: order.failureReason, eventId: event.eventId });
        }
        return ignored(`order is ${order.status}`);
      }
      if (event.amountMinor !== order.totalMinor || event.currency !== order.currency) {
        // Money arrived but not the right amount: deliver nothing, fail the order, tell the operator loudly.
        await tx.update(orders).set({ status: 'failed', failureReason: 'amount_mismatch', updatedAt: now }).where(openForPayment(order.id));
        deps.logger.error('payment amount mismatch: refund manually at the provider', {
          orderId: order.id, expected: `${order.totalMinor} ${order.currency}`, got: `${event.amountMinor} ${event.currency}`,
        });
        return { result: 'rejected', detail: 'amount_mismatch' };
      }
      if (!(await markPaid(tx, order, now))) return ignored('order was not pending');
      if (late) {
        deps.logger.warn('payment arrived after the order had failed: order revived and fulfilled', { orderId: order.id, failureReason: order.failureReason });
        return { result: 'applied', detail: `late_payment:${order.failureReason}` };
      }
      return { result: 'applied', detail: null };
    }
    case 'payment.failed': {
      if (order.status !== 'pending') return ignored(`order is ${order.status}`);
      await tx.update(orders).set({ status: 'failed', failureReason: 'payment_failed', updatedAt: now }).where(and(eq(orders.id, order.id), eq(orders.status, 'pending')));
      return { result: 'applied', detail: null };
    }
    case 'refund.succeeded': {
      if (event.amountMinor !== null && event.amountMinor < order.totalMinor) {
        deps.logger.error('partial refund received: not supported, handle manually', { orderId: order.id, amount: event.amountMinor, total: order.totalMinor });
        return { result: 'rejected', detail: 'partial_refund_unsupported' };
      }
      return (await revokeOrder(tx, order, 'refunded', now)) ? { result: 'applied', detail: null } : ignored(`order is ${order.status}`);
    }
    case 'chargeback.opened':
      return (await revokeOrder(tx, order, 'chargeback', now)) ? { result: 'applied', detail: null } : ignored(`order is ${order.status}`);
    default: {
      const never: never = event.type;
      throw new Error(`unhandled webhook event type ${String(never)}`);
    }
  }
}

export const refundSchema = z.object({ reason: z.string().trim().max(300).optional() });

/**
 * Admin refund of a paid or fulfilled order: asks the provider (idempotent per order), then
 * marks the order refunded and queues the revokes in one transaction. If the provider call
 * fails nothing changes (503 `refund_failed`). The provider's later `refund.succeeded` webhook
 * finds the order already refunded and is ignored.
 */
export async function refundOrder(deps: Deps, ctx: AdminCtx, orderId: string, input: z.infer<typeof refundSchema>): Promise<OrderView> {
  const [order] = await deps.db.select().from(orders).where(eq(orders.id, orderId)).limit(1);
  if (!order) throw new ApiError(404, 'order_not_found', 'No such order.');
  if (order.status !== 'paid' && order.status !== 'fulfilled') {
    throw new ApiError(409, 'order_not_refundable', `An order that is ${order.status} cannot be refunded.`);
  }
  const provider = deps.payments.find((p) => p.id === order.provider);
  if (!provider || order.providerRef === null) throw new ApiError(409, 'order_not_refundable', 'The order has no provider payment to refund.');
  try {
    await provider.refund({
      orderId: order.id, providerRef: order.providerRef, amountMinor: order.totalMinor, currency: order.currency,
      idempotencyKey: `refund:${order.id}`, reason: input.reason ?? null,
    });
  } catch (err) {
    deps.logger.error('provider refund failed', { orderId, ...errorFields(err) });
    throw new ApiError(503, 'refund_failed', 'The payment provider could not refund this order. Nothing was changed.');
  }

  const now = deps.clock();
  return deps.db.transaction(async (tx) => {
    const [locked] = await tx.select().from(orders).where(eq(orders.id, orderId)).for('update');
    if (!locked) throw new ApiError(404, 'order_not_found', 'No such order.');
    const moved = await revokeOrder(tx, locked, 'refunded', now);
    if (!moved) throw new ApiError(409, 'order_not_refundable', `The order is now ${locked.status}.`);
    await writeAudit(tx, ctx.actor, { action: 'order.refund', targetType: 'order', targetId: orderId, details: { reason: input.reason ?? null, totalMinor: locked.totalMinor, currency: locked.currency }, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
    const [after] = await tx.select().from(orders).where(eq(orders.id, orderId));
    return loadOrderView(tx, after as OrderRow);
  });
}

/** Housekeeping: pending orders past their expiry fail as "expired". Returns the count. */
export async function expirePendingOrders(deps: Deps, now: Date): Promise<number> {
  const rows = await deps.db
    .update(orders)
    .set({ status: 'failed', failureReason: 'expired', updatedAt: now })
    .where(and(eq(orders.status, 'pending'), lt(orders.expiresAt, now)))
    .returning({ id: orders.id });
  return rows.length;
}

// ---------------------------------------------------------------------------------------------
// Grants: what the game server pulls and acknowledges

export interface GrantView {
  id: string;
  seq: number;
  kind: 'grant' | 'revoke';
  /** For a revoke: the grant it takes back. */
  revokesGrantId: string | null;
  type: 'entitlement' | 'item';
  sku: string;
  /** The entry as defined on the product ({ type, key, durationDays? } or { type, key, quantity }). */
  payload: unknown;
  /** The order line's quantity: multiply item quantities / entitlement durations by it. */
  quantity: number;
  orderId: string;
  createdAt: Date;
}

/**
 * Pending item/entitlement entries for an account, oldest first (apply them in this order: a
 * revoke follows its grant). Requires the account to have an open game session on `server` and to
 * not be banned (403 `account_banned`, checked first). Pulling does not consume anything: the same
 * entries come back until each is acknowledged.
 *
 * Pulling BINDS each returned entry to `server` (`grants.delivering_server`): only that server's
 * ack counts (ackGrant, and the grants_ack_matches_binding check). An account has at most one open
 * session, so the server that holds it is the one delivering; when the player moves, the next
 * server's pull takes the entries over and the previous server's ack is refused.
 */
export async function pullGrants(deps: Deps, server: GameServerRow, accountId: string, limit = 100): Promise<GrantView[]> {
  await assertNotBanned(deps.db, accountId, deps.clock());
  if (!(await hasOpenSession(deps, server.id, accountId))) {
    throw new ApiError(409, 'no_active_session', 'The account has no open game session on this server.');
  }
  const rows = await deps.db.transaction(async (tx) => {
    const picked = await tx
      .select({ id: grants.id })
      .from(grants)
      .where(and(eq(grants.accountId, accountId), eq(grants.status, 'pending')))
      .orderBy(asc(grants.seq))
      .limit(limit);
    if (picked.length === 0) return [];
    return tx
      .update(grants)
      .set({ deliveringServer: server.id })
      .where(and(inArray(grants.id, picked.map((p) => p.id)), eq(grants.status, 'pending')))
      .returning();
  });
  rows.sort((a, b) => a.seq - b.seq);
  return rows.map((g) => ({
    id: g.id, seq: g.seq, kind: g.kind as 'grant' | 'revoke', revokesGrantId: g.revokesGrantId,
    type: g.type as 'entitlement' | 'item', sku: g.sku, payload: g.payload, quantity: g.quantity,
    orderId: g.orderId, createdAt: g.createdAt,
  }));
}

export interface AckResult {
  id: string;
  /** True when it was already acknowledged (a retried ack). */
  alreadyAcked: boolean;
}

/**
 * Acknowledge that the game server applied a grant or revoke. One conditional UPDATE
 * (`status = 'pending'`) so only the first ack wins; a repeat is a harmless 200 with
 * `alreadyAcked: true`. Acking the last pending grant of a paid order moves it to `fulfilled`.
 * Not tied to an open session: the server may finish applying after the player left.
 *
 * Only the server the entry was delivered to (`delivering_server`, set by pullGrants) may ack it,
 * in the same conditional UPDATE: any other server, and any ack of an entry nobody pulled, is 409
 * `grant_not_pulled` and changes nothing. A repeat ack by the acking server stays a harmless 200;
 * another server never learns that an entry that is not its own was acknowledged.
 */
export async function ackGrant(deps: Deps, server: GameServerRow, grantId: string): Promise<AckResult> {
  const now = deps.clock();
  return deps.db.transaction(async (tx) => {
    const [acked] = await tx
      .update(grants)
      .set({ status: 'acked', ackedAt: now, ackedBy: server.id })
      .where(and(eq(grants.id, grantId), eq(grants.status, 'pending'), eq(grants.deliveringServer, server.id)))
      .returning({ id: grants.id, orderId: grants.orderId, kind: grants.kind });
    if (!acked) {
      const [existing] = await tx.select({ status: grants.status, ackedBy: grants.ackedBy }).from(grants).where(eq(grants.id, grantId)).limit(1);
      if (!existing) throw new ApiError(404, 'grant_not_found', 'No such grant.');
      if (existing.status === 'acked' && existing.ackedBy === server.id) return { id: grantId, alreadyAcked: true };
      throw new ApiError(409, 'grant_not_pulled', 'This grant was not delivered to this server. Pull it first.');
    }
    if (acked.kind === 'grant') {
      // Lock the order, then see whether anything is still waiting for the server.
      const [order] = await tx.select().from(orders).where(eq(orders.id, acked.orderId)).for('update');
      if (order?.status === 'paid') {
        const [pending] = await tx
          .select({ n: sql<number>`count(*)::int` })
          .from(grants)
          .where(and(eq(grants.orderId, order.id), eq(grants.kind, 'grant'), eq(grants.status, 'pending')));
        if ((pending?.n ?? 0) === 0) await markFulfilled(tx, order.id, now);
      }
    }
    return { id: grantId, alreadyAcked: false };
  });
}

/** Items and entitlements the account owns now (grants with no revoke), for the launcher's "owned" screen. */
export async function listOwned(db: Db, accountId: string) {
  const rows = await db
    .select()
    .from(grants)
    .where(and(eq(grants.accountId, accountId), eq(grants.kind, 'grant'), sql`${grants.type} <> 'wallet_credit'`, sql`not exists (select 1 from grants r where r.revokes_grant_id = ${grants.id})`))
    .orderBy(desc(grants.seq));
  return rows.map((g) => ({
    id: g.id, sku: g.sku, type: g.type, payload: g.payload, quantity: g.quantity, orderId: g.orderId,
    delivered: g.status === 'acked', createdAt: g.createdAt,
  }));
}

// ---------------------------------------------------------------------------------------------
// Wallet

export interface Balance {
  currency: string;
  balance: number;
}

export async function getBalances(db: Db, accountId: string): Promise<Balance[]> {
  const rows = await db.select({ currency: walletBalances.currency, balance: walletBalances.balance }).from(walletBalances).where(eq(walletBalances.accountId, accountId)).orderBy(asc(walletBalances.currency));
  return rows;
}

export async function getBalance(db: Db, accountId: string, currency: string): Promise<number> {
  const [row] = await db.select({ balance: walletBalances.balance }).from(walletBalances).where(and(eq(walletBalances.accountId, accountId), eq(walletBalances.currency, currency)));
  return row?.balance ?? 0;
}

export const ledgerQuerySchema = z.object({
  limit: z.coerce.number().int().min(1).max(200).default(50),
  before: z.coerce.number().int().positive().optional(),
});

export async function listLedger(db: Db, accountId: string, q: z.infer<typeof ledgerQuerySchema>) {
  const rows = await db
    .select()
    .from(walletLedger)
    .where(and(eq(walletLedger.accountId, accountId), q.before !== undefined ? lt(walletLedger.id, q.before) : undefined))
    .orderBy(desc(walletLedger.id))
    .limit(q.limit);
  return rows.map((r) => ({ id: r.id, currency: r.currency, delta: r.delta, reason: r.reason, memo: r.memo, createdAt: r.createdAt }));
}

export const spendSchema = z.object({
  accountId: z.guid(),
  currency: z.string().regex(/^[a-z][a-z0-9_]{1,15}$/),
  amount: z.number().int().min(1).max(1_000_000_000),
  /** Chosen by the game server per spend; resending the same key never spends twice. */
  idempotencyKey: z.string().regex(/^[A-Za-z0-9_.:-]{8,64}$/),
  memo: z.string().trim().max(200).optional(),
});

export interface SpendResult {
  /** True when this key had already been applied: nothing was spent now. */
  replayed: boolean;
  balance: number;
}

/**
 * Spend from an account's wallet on behalf of a game server. Idempotent per (server, key). The
 * database refuses an overdraw (trigger), reported as 409 `insufficient_funds` with the balance.
 * Requires an open game session of the account on this server and an account that is not banned
 * (403 `account_banned`, checked first, also for a replayed key). The ban is re-checked under a
 * KEY SHARE lock on the account row, the lock a ban takes exclusively, so a spend cannot slip in
 * between a ban being decided and its game session being closed.
 */
export async function spend(deps: Deps, server: GameServerRow, input: z.infer<typeof spendSchema>): Promise<SpendResult> {
  await assertNotBanned(deps.db, input.accountId, deps.clock());
  if (!(await hasOpenSession(deps, server.id, input.accountId))) {
    throw new ApiError(409, 'no_active_session', 'The account has no open game session on this server.');
  }
  const key = `spend:${server.id}:${input.idempotencyKey}`;
  try {
    await deps.db.transaction(async (tx) => {
      const [locked] = await tx.select({ id: accounts.id }).from(accounts).where(eq(accounts.id, input.accountId)).for('key share');
      if (!locked) throw new ApiError(404, 'account_not_found', 'No such account.');
      await assertNotBanned(tx, input.accountId, deps.clock());
      await tx.insert(walletLedger).values({
        accountId: input.accountId, currency: input.currency, delta: -input.amount, reason: 'spend',
        idempotencyKey: key, serverId: server.id, memo: input.memo ?? null, createdAt: deps.clock(),
      });
    });
    return { replayed: false, balance: await getBalance(deps.db, input.accountId, input.currency) };
  } catch (err) {
    if (isCheckViolation(err, 'insufficient_funds')) {
      throw new ApiError(409, 'insufficient_funds', 'Not enough funds.', { details: { balance: await getBalance(deps.db, input.accountId, input.currency) } });
    }
    if (isUniqueViolation(err, 'wallet_ledger_idem_uq')) {
      const [prior] = await deps.db.select().from(walletLedger).where(eq(walletLedger.idempotencyKey, key)).limit(1);
      if (!prior || prior.accountId !== input.accountId || prior.currency !== input.currency || prior.delta !== -input.amount) {
        throw new ApiError(409, 'idempotency_mismatch', 'This idempotencyKey was used for a different spend.');
      }
      return { replayed: true, balance: await getBalance(deps.db, input.accountId, input.currency) };
    }
    throw err;
  }
}

/** Account lookup used by internal endpoints that take an accountId. */
export async function requireAccount(db: Db, accountId: string): Promise<AccountRow> {
  const a = await findAccountById(db, accountId);
  if (!a) throw new ApiError(404, 'account_not_found', 'No such account.');
  return a;
}

