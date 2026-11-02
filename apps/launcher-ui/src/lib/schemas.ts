import { z } from 'zod';

// Response shapes of the GAGE-MP platform API, as the launcher reads them. They mirror the
// platform routes (platform/src/http/routes/*.ts, docs in the platform's docs/api.md once written).
// Enum-like fields the platform may extend (roles, product kinds, order states) are read as
// strings so a new value degrades one row instead of failing the whole list; the UI maps the
// values it knows (see shop/stamps.ts) and shows the rest verbatim.

const isoDate = z.iso.datetime({ offset: true });

export const errorBodySchema = z.object({
  error: z.object({
    code: z.string(),
    message: z.string(),
    requestId: z.string().optional(),
    details: z.unknown().optional(),
  }),
});

export const banSchema = z.object({
  id: z.string(),
  reason: z.string(),
  createdAt: isoDate,
  expiresAt: isoDate.nullable(),
});

export const accountSchema = z.object({
  id: z.string(),
  username: z.string(),
  email: z.string().nullable(),
  emailVerified: z.boolean(),
  roles: z.array(z.string()),
  hasPassword: z.boolean(),
  discord: z.object({ id: z.string(), username: z.string().nullable() }).nullable(),
  ban: banSchema.nullable(),
  createdAt: isoDate,
});
export type Account = z.infer<typeof accountSchema>;

export const loginResultSchema = z.object({
  token: z.string().min(16),
  expiresAt: isoDate,
  sessionId: z.string(),
  account: accountSchema,
});
export type LoginResult = z.infer<typeof loginResultSchema>;

export const registerResultSchema = z.object({ verificationRequired: z.boolean() });
export const meSchema = z.object({ account: accountSchema });
export const sentSchema = z.object({ sent: z.literal(true) });
export const okSchema = z.object({ ok: z.literal(true) });
export const passwordChangedSchema = z.object({ revokedSessions: z.number().int().min(0) });

export const discordStartSchema = z.object({
  flowId: z.string(),
  authorizeUrl: z.url(),
  pollSecret: z.string().min(16),
  expiresAt: isoDate,
});
export type DiscordStart = z.infer<typeof discordStartSchema>;

export const discordPollSchema = z.discriminatedUnion('status', [
  z.object({ status: z.literal('pending') }),
  z.object({ status: z.literal('failed'), code: z.string() }),
  z.object({ status: z.literal('needs_username'), suggestedUsername: z.string(), discordUsername: z.string() }),
  z.object({ status: z.literal('linked') }),
  loginResultSchema.extend({ status: z.literal('complete') }),
]);
export type DiscordPoll = z.infer<typeof discordPollSchema>;

export const sessionRowSchema = z.object({
  id: z.string(),
  deviceName: z.string().nullable(),
  userAgent: z.string().nullable(),
  ip: z.string().nullable(),
  createdAt: isoDate,
  lastSeenAt: isoDate.nullable(),
  expiresAt: isoDate,
  current: z.boolean(),
});
export type SessionRow = z.infer<typeof sessionRowSchema>;
export const sessionsSchema = z.object({ sessions: z.array(sessionRowSchema) });
export const revokedSchema = z.object({ revoked: z.number().int().min(0) });

export const serverStatusSchema = z.object({
  state: z.enum(['online', 'offline', 'unknown']),
  players: z.number().int().min(0).nullable(),
  maxPlayers: z.number().int().min(0).nullable(),
  pingMs: z.number().nullable(),
  protocol: z.number().nullable(),
  checkedAt: isoDate.nullable(),
  detail: z.string().optional(),
  // true when these numbers were really measured (a native UDP query in demo mode, or the
  // platform), not the fabricated demo values -- the UI drops the "Demo counts" chip for it.
  live: z.boolean().optional(),
});
export const serverSchema = z.object({
  id: z.string(),
  name: z.string(),
  region: z.string(),
  host: z.string(),
  port: z.number().int().min(0).max(65535),
  tags: z.array(z.string()),
  sort: z.number(),
  maintenance: z.object({ enabled: z.boolean(), message: z.string().nullable() }),
  // An announced server that is not live yet: shown as a "Coming soon" card, never joinable.
  // Defaults false so a platform response that omits it degrades to an ordinary live server.
  comingSoon: z.boolean().default(false),
  status: serverStatusSchema,
});
export type Server = z.infer<typeof serverSchema>;
export const serversSchema = z.object({ servers: z.array(serverSchema) });

export const joinResultSchema = z.object({
  token: z.string().min(8),
  expiresAt: isoDate,
  server: z.object({ id: z.string(), name: z.string(), host: z.string(), port: z.number().int().min(1).max(65535) }),
});
export type JoinResult = z.infer<typeof joinResultSchema>;

export const grantSchema = z.union([
  z.object({ type: z.literal('wallet_credit'), currency: z.string(), amount: z.number() }),
  z.object({ type: z.literal('entitlement'), key: z.string(), durationDays: z.number().optional() }),
  z.object({ type: z.literal('item'), key: z.string(), quantity: z.number().optional() }),
  z.object({ type: z.string() }).loose(),
]);
export type Grant = z.infer<typeof grantSchema>;

export const productSchema = z.object({
  id: z.string(),
  sku: z.string(),
  name: z.string(),
  description: z.string(),
  priceMinor: z.number().int().min(0),
  currency: z.string().length(3),
  kind: z.string(),
  grants: z.array(grantSchema),
  sort: z.number(),
  imageUrl: z.string().nullable(),
  maxQuantity: z.number().int().min(1),
});
export type Product = z.infer<typeof productSchema>;
export const productsSchema = z.object({ products: z.array(productSchema) });

export const orderSchema = z.object({
  id: z.string(),
  status: z.string(),
  currency: z.string(),
  totalMinor: z.number().int(),
  provider: z.string(),
  checkoutUrl: z.string().nullable(),
  failureReason: z.string().nullable(),
  createdAt: isoDate,
  expiresAt: isoDate,
  paidAt: isoDate.nullable(),
  fulfilledAt: isoDate.nullable(),
  refundedAt: isoDate.nullable(),
  items: z.array(z.object({
    lineNo: z.number().int(),
    sku: z.string(),
    name: z.string(),
    kind: z.string(),
    quantity: z.number().int(),
    unitPriceMinor: z.number().int(),
  })),
});
export type Order = z.infer<typeof orderSchema>;
export const ordersSchema = z.object({ orders: z.array(orderSchema) });
export const orderEnvelopeSchema = z.object({ order: orderSchema });

export const balancesSchema = z.object({ balances: z.array(z.object({ currency: z.string(), balance: z.number() })) });
export type Balance = z.infer<typeof balancesSchema>['balances'][number];

export const ownedItemSchema = z.object({
  id: z.string(),
  sku: z.string(),
  type: z.string(),
  payload: z.record(z.string(), z.unknown()),
  quantity: z.number().int(),
  orderId: z.string().nullable(),
  delivered: z.boolean(),
  createdAt: isoDate,
});
export type OwnedItem = z.infer<typeof ownedItemSchema>;
export const ownedItemsSchema = z.object({ items: z.array(ownedItemSchema) });

export const settingsViewSchema = z.object({
  settings: z.record(z.string(), z.unknown()),
  version: z.number().int().min(0),
  updatedAt: isoDate.nullable(),
});
