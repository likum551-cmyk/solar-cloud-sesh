import { z } from 'zod';
import type { Deps } from '../../deps.js';
import { endGameSession, sessionEndSchema, verifyJoinToken, verifySchema } from '../../game/join.js';
import { ackGrant, getBalances, pullGrants, requireAccount, spend, spendSchema } from '../../shop/service.js';
import { hasOpenSession } from '../../game/join.js';
import { ApiError } from '../../util/errors.js';
import { requireServer } from '../helpers.js';
import { uuidParam, type Router } from '../router.js';

const accountQuery = z.object({ accountId: z.guid() });
const accountBody = z.object({ accountId: z.guid() });

/**
 * The game server contract (docs/game-server-contract.md). Served only on the internal listener
 * (INTERNAL_HOST:INTERNAL_PORT) unless INTERNAL_ON_PUBLIC_LISTENER=1. Every route needs
 * X-Gage-Server-Id and X-Gage-Server-Secret from an allowlisted address. Logical outcomes
 * (a rejected token) are HTTP 200 with ok:false so a C++ client only has to treat non-200 as
 * infrastructure failure.
 */
export function registerInternalRoutes(r: Router, deps: Deps): void {
  const common = { auth: 'server' as const, tags: ['internal'] };

  r.add(
    {
      ...common, method: 'post', path: '/internal/game/verify', summary: 'Redeem a join token', body: verifySchema,
      description: 'Consumes the token (single use) and opens the account\'s game session on this server. A ban deletes the account\'s unused tokens, so those come back invalid_token. 200 { ok: true, accountId, username, roles, emailVerified, sessionId, ban: null } or 200 { ok: false, reason: invalid_token | expired | used } or 200 { ok: false, reason: banned, accountId, username, ban: { reason, expiresAt } }.',
      response: '200 verdict (see description)',
    },
    async (c, { body }) => c.json(await verifyJoinToken(deps, requireServer(c), body.token)),
  );

  r.add(
    {
      ...common, method: 'post', path: '/internal/game/session-end', summary: 'The player left', body: sessionEndSchema,
      description: 'Closes the account\'s open session on this server. Idempotent (ended: 0 when nothing was open).',
      response: '200 { ended: number }',
    },
    async (c, { body }) => c.json(await endGameSession(deps, requireServer(c), body)),
  );

  const pull = async (server: ReturnType<typeof requireServer>, accountId: string) => ({ grants: await pullGrants(deps, server, accountId) });

  r.add(
    {
      ...common, method: 'get', path: '/internal/game/grants', summary: 'Pending item/entitlement grants and revokes of an account', query: accountQuery,
      description: 'Apply them in the order returned, then POST .../ack for each. Not consumed by reading. Needs an open session of the account on this server and an account that is not banned. Pulling binds the entries to this server: only its ack counts.',
      response: '200 { grants: [{ id, seq, kind, revokesGrantId, type, sku, payload, quantity, orderId, createdAt }] }', errors: ['no_active_session', 'account_banned'],
    },
    async (c, { query }) => c.json(await pull(requireServer(c), query.accountId)),
  );

  r.add(
    {
      ...common, method: 'post', path: '/internal/game/grants/pull', summary: 'Same as GET /internal/game/grants (for clients that only POST)', body: accountBody,
      response: '200 { grants: [...] }', errors: ['no_active_session', 'account_banned'],
    },
    async (c, { body }) => c.json(await pull(requireServer(c), body.accountId)),
  );

  r.add(
    {
      ...common, method: 'post', path: '/internal/game/grants/:id/ack', summary: 'The grant or revoke was applied',
      description: 'Only the server the entry was delivered to (the one that pulled it) may ack it; any other server, and any ack of an entry nobody pulled, is 409 grant_not_pulled. Idempotent for that server: a repeated ack returns alreadyAcked: true. After an ack the entry is never delivered again.',
      response: '200 { id, alreadyAcked }', errors: ['grant_not_found', 'grant_not_pulled'],
    },
    async (c) => c.json(await ackGrant(deps, requireServer(c), uuidParam(c, 'id'))),
  );

  const balances = async (server: ReturnType<typeof requireServer>, accountId: string) => {
    if (!(await hasOpenSession(deps, server.id, accountId))) {
      throw new ApiError(409, 'no_active_session', 'The account has no open game session on this server.');
    }
    await requireAccount(deps.db, accountId);
    return { balances: await getBalances(deps.db, accountId) };
  };

  r.add(
    {
      ...common, method: 'get', path: '/internal/game/wallet', summary: 'Wallet balances of an account', query: accountQuery,
      response: '200 { balances: [{ currency, balance }] }', errors: ['no_active_session'],
    },
    async (c, { query }) => c.json(await balances(requireServer(c), query.accountId)),
  );

  r.add(
    {
      ...common, method: 'post', path: '/internal/game/wallet/balance', summary: 'Same as GET /internal/game/wallet (for clients that only POST)', body: accountBody,
      response: '200 { balances: [...] }', errors: ['no_active_session'],
    },
    async (c, { body }) => c.json(await balances(requireServer(c), body.accountId)),
  );

  r.add(
    {
      ...common, method: 'post', path: '/internal/game/wallet/spend', summary: 'Spend in-game currency', body: spendSchema,
      description: 'Idempotent per (server, idempotencyKey): resend the same request after a timeout and it is applied once. Refused with 409 insufficient_funds when the balance is too low, and with 403 account_banned for a banned account (also for a replayed key).',
      response: '200 { replayed, balance }', errors: ['no_active_session', 'insufficient_funds', 'idempotency_mismatch', 'account_banned'],
    },
    async (c, { body }) => c.json(await spend(deps, requireServer(c), body)),
  );
}
