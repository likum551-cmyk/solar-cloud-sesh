import { asc, eq } from 'drizzle-orm';
import { isIP } from 'node:net';
import { z } from 'zod';
import type { Deps } from '../deps.js';
import type { Db } from '../db/index.js';
import { gameServers } from '../db/schema.js';
import { writeAudit, type AdminCtx } from '../admin/service.js';
import { ApiError, isUniqueViolation } from '../util/errors.js';
import { normalizeIp } from '../util/ip.js';
import { hashToken, randomToken, safeEqual } from '../util/tokens.js';
import type { ServerStatus } from './status.js';

export type GameServerRow = typeof gameServers.$inferSelect;

export const SERVER_SECRET_PREFIX = 'gss_';

const hostSchema = z
  .string()
  .trim()
  .max(253)
  .refine((h) => isIP(h) !== 0 || /^(?=.{1,253}$)[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/.test(h), 'must be an IP address or a host name');

const ipSchema = z
  .string()
  .trim()
  .transform((v, ctx) => {
    const n = normalizeIp(v);
    if (n === null) {
      ctx.addIssue({ code: 'custom', message: 'must be an IP address' });
      return z.NEVER;
    }
    return n;
  });

const serverFields = {
  name: z.string().trim().min(1).max(64),
  region: z.string().trim().min(1).max(32),
  host: hostSchema,
  port: z.number().int().min(1).max(65535),
  tags: z.array(z.string().regex(/^[a-z0-9][a-z0-9-]{0,23}$/)).max(10),
  sort: z.number().int().min(-1_000_000).max(1_000_000),
  visible: z.boolean(),
  maintenance: z.boolean(),
  maintenanceMessage: z.string().trim().max(200).nullable(),
  /** IPs the internal API accepts this server's calls from. */
  allowedIps: z.array(ipSchema).max(16),
};

export const serverCreateSchema = z.object({
  id: z.string().regex(/^[a-z0-9][a-z0-9-]{1,31}$/),
  ...serverFields,
  // Defaults only on create: on update an absent key must mean "leave unchanged" (zod 4 applies defaults inside .partial()).
  tags: serverFields.tags.default([]),
  sort: serverFields.sort.default(0),
  visible: serverFields.visible.default(true),
  maintenance: serverFields.maintenance.default(false),
  maintenanceMessage: serverFields.maintenanceMessage.default(null),
  allowedIps: serverFields.allowedIps.default([]),
});
export const serverUpdateSchema = z
  .object(serverFields)
  .partial()
  .refine((o) => Object.keys(o).length > 0, 'at least one field is required');

/** The row as admins see it (never the secret hash). */
export interface AdminServerView {
  id: string;
  name: string;
  region: string;
  host: string;
  port: number;
  tags: string[];
  sort: number;
  visible: boolean;
  maintenance: boolean;
  maintenanceMessage: string | null;
  allowedIps: string[];
  createdAt: Date;
  updatedAt: Date;
}

function adminView(r: GameServerRow): AdminServerView {
  return {
    id: r.id, name: r.name, region: r.region, host: r.host, port: r.port, tags: r.tags, sort: r.sort,
    visible: r.visible, maintenance: r.maintenance, maintenanceMessage: r.maintenanceMessage,
    allowedIps: r.allowedIps, createdAt: r.createdAt, updatedAt: r.updatedAt,
  };
}

/** What GET /v1/servers returns per server. */
export interface PublicServerView {
  id: string;
  name: string;
  region: string;
  host: string;
  port: number;
  tags: string[];
  sort: number;
  maintenance: { enabled: boolean; message: string | null };
  status: ServerStatus;
}

export async function getServer(db: Db, id: string): Promise<GameServerRow | null> {
  const [row] = await db.select().from(gameServers).where(eq(gameServers.id, id)).limit(1);
  return row ?? null;
}

/** Visible servers (all servers when `includeHidden`) in sort order, with their live status. Never blocks on a slow server (see ServerStatusService). */
export async function listPublicServers(deps: Deps, opts: { includeHidden?: boolean } = {}): Promise<PublicServerView[]> {
  const rows = await deps.db.select().from(gameServers).orderBy(asc(gameServers.sort), asc(gameServers.name));
  const shown = rows.filter((r) => r.visible || opts.includeHidden === true);
  const statuses = await deps.serverStatus.statuses(shown.map((r) => ({ id: r.id, host: r.host, port: r.port })));
  return shown.map((r) => ({
    id: r.id, name: r.name, region: r.region, host: r.host, port: r.port, tags: r.tags, sort: r.sort,
    maintenance: { enabled: r.maintenance, message: r.maintenance ? r.maintenanceMessage : null },
    status: statuses.get(r.id) as ServerStatus,
  }));
}

export async function listAdminServers(db: Db): Promise<AdminServerView[]> {
  const rows = await db.select().from(gameServers).orderBy(asc(gameServers.sort), asc(gameServers.name));
  return rows.map(adminView);
}

/** Create a server. The returned `secret` is shown once; only its hash is stored. */
export async function createServer(deps: Deps, ctx: AdminCtx, input: z.infer<typeof serverCreateSchema>): Promise<{ server: AdminServerView; secret: string }> {
  const now = deps.clock();
  const secret = SERVER_SECRET_PREFIX + randomToken(32);
  try {
    const row = await deps.db.transaction(async (tx) => {
      const [created] = await tx
        .insert(gameServers)
        .values({ ...input, secretHash: hashToken(secret), createdAt: now, updatedAt: now })
        .returning();
      if (!created) throw new Error('server insert returned no row');
      await writeAudit(tx, ctx.actor, { action: 'server.create', targetType: 'server', targetId: created.id, details: { name: created.name, host: created.host, port: created.port }, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
      return created;
    });
    return { server: adminView(row), secret };
  } catch (err) {
    if (isUniqueViolation(err)) throw new ApiError(409, 'server_exists', 'A server with that id already exists.');
    throw err;
  }
}

export async function updateServer(deps: Deps, ctx: AdminCtx, id: string, input: z.infer<typeof serverUpdateSchema>): Promise<AdminServerView> {
  const now = deps.clock();
  return deps.db.transaction(async (tx) => {
    const [row] = await tx.update(gameServers).set({ ...input, updatedAt: now }).where(eq(gameServers.id, id)).returning();
    if (!row) throw new ApiError(404, 'server_not_found', 'No such server.');
    await writeAudit(tx, ctx.actor, { action: 'server.update', targetType: 'server', targetId: id, details: { fields: Object.keys(input) }, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
    return adminView(row);
  });
}

/** Issue a new secret (the old one stops working at once). Returned once. */
export async function rotateServerSecret(deps: Deps, ctx: AdminCtx, id: string): Promise<{ secret: string }> {
  const now = deps.clock();
  const secret = SERVER_SECRET_PREFIX + randomToken(32);
  await deps.db.transaction(async (tx) => {
    const rows = await tx.update(gameServers).set({ secretHash: hashToken(secret), updatedAt: now }).where(eq(gameServers.id, id)).returning({ id: gameServers.id });
    if (rows.length === 0) throw new ApiError(404, 'server_not_found', 'No such server.');
    await writeAudit(tx, ctx.actor, { action: 'server.rotate_secret', targetType: 'server', targetId: id, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
  });
  return { secret };
}

/** Delete a server and (by cascade) its join tokens and game sessions. */
export async function deleteServer(deps: Deps, ctx: AdminCtx, id: string): Promise<void> {
  const now = deps.clock();
  await deps.db.transaction(async (tx) => {
    const rows = await tx.delete(gameServers).where(eq(gameServers.id, id)).returning({ id: gameServers.id });
    if (rows.length === 0) throw new ApiError(404, 'server_not_found', 'No such server.');
    await writeAudit(tx, ctx.actor, { action: 'server.delete', targetType: 'server', targetId: id, ip: ctx.ip ?? null, requestId: ctx.requestId ?? null }, now);
  });
}

// A hash no real secret has, compared against when the server id is unknown so that "unknown id"
// and "wrong secret" take the same code path.
const NO_SERVER_HASH = hashToken('no-such-server');

/**
 * Authenticate a game server call: the id from X-Gage-Server-Id, the secret from
 * X-Gage-Server-Secret (constant-time comparison of SHA-256 digests) and the caller's IP against
 * the server's allowlist. An unknown id and a wrong secret both give 401 `invalid_server_credentials`;
 * a right secret from an address outside the allowlist gives 403 `ip_not_allowed` (an empty
 * allowlist allows nobody).
 */
export async function authenticateServer(db: Db, serverId: string | undefined, secret: string | undefined, peerIp: string | null): Promise<GameServerRow> {
  const row = serverId !== undefined && serverId.length <= 64 ? await getServer(db, serverId) : null;
  const presented = hashToken(secret ?? '');
  const secretOk = safeEqual(presented, row?.secretHash ?? NO_SERVER_HASH);
  if (!row || !secretOk || secret === undefined || !secret.startsWith(SERVER_SECRET_PREFIX)) {
    throw new ApiError(401, 'invalid_server_credentials', 'Unknown server or wrong secret.');
  }
  if (peerIp === null || !row.allowedIps.includes(peerIp)) {
    throw new ApiError(403, 'ip_not_allowed', 'This server may not call the platform from that address.');
  }
  return row;
}
