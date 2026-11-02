import type { ReqCtx } from '../auth/service.js';
import type { SessionMeta } from '../auth/sessions.js';
import type { AdminCtx } from '../admin/service.js';
import { ApiError } from '../util/errors.js';
import type { Ctx } from './router.js';
import type { SessionInfo } from './types.js';

/** The signed-in session of a route registered with auth session/mod/admin. */
export function requireSession(c: Ctx): SessionInfo {
  const s = c.get('session');
  if (!s) throw new ApiError(401, 'unauthorized', 'Sign in required.');
  return s;
}

export function reqCtx(c: Ctx): ReqCtx {
  return {
    ip: c.get('clientIp'),
    ipGroup: c.get('clientGroup'),
    userAgent: c.req.header('user-agent') ?? null,
    requestId: c.get('requestId'),
  };
}

export function sessionMeta(c: Ctx, deviceName?: string | null): SessionMeta {
  return { ip: c.get('clientIp'), userAgent: c.req.header('user-agent') ?? null, deviceName: deviceName ?? null };
}

/** Who is acting, for the audit log of an admin route. */
export function adminCtx(c: Ctx): AdminCtx {
  const s = requireSession(c);
  return {
    actor: { accountId: s.account.id, label: `account:${s.account.id}` },
    actorRoles: s.roles,
    ip: c.get('clientIp'),
    requestId: c.get('requestId'),
  };
}

export function requireServer(c: Ctx) {
  const s = c.get('server');
  if (!s) throw new ApiError(401, 'invalid_server_credentials', 'Game server authentication required.');
  return s;
}
