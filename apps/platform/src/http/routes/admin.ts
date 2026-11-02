import {
  accountSearchSchema,
  auditQuerySchema,
  banAccount,
  banSchema,
  getAdminAccount,
  listAudit,
  rolesSchema,
  searchAccounts,
  setRoles,
  unbanAccount,
  unbanSchema,
} from '../../admin/service.js';
import type { Deps } from '../../deps.js';
import {
  createServer,
  deleteServer,
  listAdminServers,
  rotateServerSecret,
  serverCreateSchema,
  serverUpdateSchema,
  updateServer,
} from '../../servers/service.js';
import {
  adminOrdersQuerySchema,
  createProduct,
  deleteProduct,
  getOrder,
  listAdminOrders,
  listProducts,
  productCreateSchema,
  productUpdateSchema,
  refundOrder,
  refundSchema,
  updateProduct,
} from '../../shop/service.js';
import { adminCtx } from '../helpers.js';
import { ApiError } from '../../util/errors.js';
import { uuidParam, type Ctx, type Router } from '../router.js';

const SERVER_ID = /^[a-z0-9][a-z0-9-]{1,31}$/;

function serverIdParam(c: Ctx): string {
  const id = c.req.param('id') ?? '';
  if (!SERVER_ID.test(id)) throw new ApiError(400, 'invalid_request', 'Path parameter "id" must be a server id (lower-case letters, digits, dashes).');
  return id;
}

/** Moderator and admin routes. Every mutation writes an audit_log row. */
export function registerAdminRoutes(r: Router, deps: Deps): void {
  // Accounts, bans, roles -----------------------------------------------------------------------
  r.add(
    { method: 'get', path: '/v1/admin/accounts', summary: 'Search accounts by username prefix or exact email', auth: 'mod', tags: ['admin'], query: accountSearchSchema, response: '200 { accounts: [{ id, username, email, emailVerified, roles, ban, createdAt, lastLoginAt }] }' },
    async (c, { query }) => c.json({ accounts: await searchAccounts(deps, query) }),
  );

  r.add(
    { method: 'get', path: '/v1/admin/accounts/:id', summary: 'One account', auth: 'mod', tags: ['admin'], response: '200 { account }', errors: ['account_not_found'] },
    async (c) => c.json({ account: await getAdminAccount(deps, uuidParam(c, 'id')) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/admin/accounts/:id/ban', summary: 'Ban an account', auth: 'mod', tags: ['admin'], body: banSchema,
      description: 'Mods may ban players; admins may also ban mods; admins cannot be banned. Omit durationSeconds for a permanent ban. Takes effect at once: the open game session is closed (end reason banned) and unused join tokens are deleted.',
      response: '201 { ban }', successStatus: 201, errors: ['account_not_found', 'forbidden_target', 'already_banned'],
    },
    async (c, { body }) => c.json({ ban: await banAccount(deps, adminCtx(c), uuidParam(c, 'id'), body) }, 201),
  );

  r.add(
    {
      method: 'post', path: '/v1/admin/accounts/:id/unban', summary: 'Lift an account\'s ban', auth: 'mod', tags: ['admin'], body: unbanSchema.optional().default({}),
      description: 'You cannot lift your own ban, and only an admin may lift the ban of a mod or an admin (403 forbidden_target); refusals are audited as account.unban_refused. A banned mod or admin cannot call any admin route (403 account_banned).',
      response: '200 { ok: true }', errors: ['not_banned', 'forbidden_target'],
    },
    async (c, { body }) => {
      await unbanAccount(deps, adminCtx(c), uuidParam(c, 'id'), body);
      return c.json({ ok: true });
    },
  );

  r.add(
    {
      method: 'put', path: '/v1/admin/accounts/:id/roles', summary: 'Set an account\'s roles', auth: 'admin', tags: ['admin'], body: rolesSchema,
      description: 'Replaces the role set (player is always kept). The last admin cannot lose the admin role.',
      response: '200 { roles }', errors: ['account_not_found', 'last_admin'],
    },
    async (c, { body }) => c.json({ roles: await setRoles(deps, adminCtx(c), uuidParam(c, 'id'), body) }),
  );

  r.add(
    { method: 'get', path: '/v1/admin/audit', summary: 'The audit log, newest first', auth: 'admin', tags: ['admin'], query: auditQuerySchema, response: '200 { entries: [{ id, at, actorAccountId, actorLabel, action, targetType, targetId, details, ip, requestId }] }' },
    async (c, { query }) => c.json({ entries: await listAudit(deps, query) }),
  );

  // Servers -------------------------------------------------------------------------------------
  r.add(
    { method: 'get', path: '/v1/admin/servers', summary: 'All servers including hidden ones (no secrets)', auth: 'admin', tags: ['admin', 'servers'], response: '200 { servers: [...] }' },
    async (c) => c.json({ servers: await listAdminServers(deps.db) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/admin/servers', summary: 'Create an official server', auth: 'admin', tags: ['admin', 'servers'], body: serverCreateSchema,
      description: 'The response contains the server\'s secret exactly once; only its hash is stored. Put it in the game server\'s config together with the id.',
      response: '201 { server, secret }', successStatus: 201, errors: ['server_exists'],
    },
    async (c, { body }) => c.json(await createServer(deps, adminCtx(c), body), 201),
  );

  r.add(
    { method: 'patch', path: '/v1/admin/servers/:id', summary: 'Edit a server (name, host, port, tags, sort, visible, maintenance, allowedIps)', auth: 'admin', tags: ['admin', 'servers'], body: serverUpdateSchema, response: '200 { server }', errors: ['server_not_found'] },
    async (c, { body }) => c.json({ server: await updateServer(deps, adminCtx(c), serverIdParam(c), body) }),
  );

  r.add(
    { method: 'post', path: '/v1/admin/servers/:id/rotate-secret', summary: 'Issue a new server secret (the old one stops working)', auth: 'admin', tags: ['admin', 'servers'], response: '200 { secret }', errors: ['server_not_found'] },
    async (c) => c.json(await rotateServerSecret(deps, adminCtx(c), serverIdParam(c))),
  );

  r.add(
    { method: 'delete', path: '/v1/admin/servers/:id', summary: 'Delete a server (and its join tokens and sessions)', auth: 'admin', tags: ['admin', 'servers'], response: '204 (no body)', successStatus: 204, errors: ['server_not_found'] },
    async (c) => {
      await deleteServer(deps, adminCtx(c), serverIdParam(c));
      return c.body(null, 204);
    },
  );

  // Products ------------------------------------------------------------------------------------
  r.add(
    { method: 'get', path: '/v1/admin/products', summary: 'All products including inactive', auth: 'admin', tags: ['admin', 'shop'], response: '200 { products: [...] }' },
    async (c) => c.json({ products: await listProducts(deps.db, { activeOnly: false }) }),
  );

  r.add(
    { method: 'post', path: '/v1/admin/products', summary: 'Create a product', auth: 'admin', tags: ['admin', 'shop'], body: productCreateSchema, response: '201 { product }', successStatus: 201, errors: ['sku_exists'] },
    async (c, { body }) => c.json({ product: await createProduct(deps, adminCtx(c), body) }, 201),
  );

  r.add(
    { method: 'patch', path: '/v1/admin/products/:id', summary: 'Edit a product (existing orders keep their snapshot)', auth: 'admin', tags: ['admin', 'shop'], body: productUpdateSchema, response: '200 { product }', errors: ['product_not_found', 'sku_exists'] },
    async (c, { body }) => c.json({ product: await updateProduct(deps, adminCtx(c), uuidParam(c, 'id'), body) }),
  );

  r.add(
    { method: 'delete', path: '/v1/admin/products/:id', summary: 'Delete a product nobody ordered', auth: 'admin', tags: ['admin', 'shop'], response: '204 (no body)', successStatus: 204, errors: ['product_not_found', 'product_in_use'] },
    async (c) => {
      await deleteProduct(deps, adminCtx(c), uuidParam(c, 'id'));
      return c.body(null, 204);
    },
  );

  // Orders --------------------------------------------------------------------------------------
  r.add(
    { method: 'get', path: '/v1/admin/orders', summary: 'Orders, newest first (filter by status or account)', auth: 'admin', tags: ['admin', 'shop'], query: adminOrdersQuerySchema, response: '200 { orders: [order + accountId] }' },
    async (c, { query }) => c.json({ orders: await listAdminOrders(deps, query) }),
  );

  r.add(
    { method: 'get', path: '/v1/admin/orders/:id', summary: 'One order', auth: 'admin', tags: ['admin', 'shop'], response: '200 { order }', errors: ['order_not_found'] },
    async (c) => c.json({ order: await getOrder(deps, uuidParam(c, 'id'), null) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/admin/orders/:id/refund', summary: 'Refund a paid order and revoke what it granted', auth: 'admin', tags: ['admin', 'shop'], body: refundSchema.optional().default({}),
      response: '200 { order }', errors: ['order_not_found', 'order_not_refundable', 'refund_failed'],
    },
    async (c, { body }) => c.json({ order: await refundOrder(deps, adminCtx(c), uuidParam(c, 'id'), body) }),
  );

}
