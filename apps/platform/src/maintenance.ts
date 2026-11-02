import { and, isNull, lt, or, sql } from 'drizzle-orm';
import { purgeOauthFlows } from './auth/discord.js';
import { purgeSessions } from './auth/sessions.js';
import type { Deps } from './deps.js';
import { authThrottle, authTokens, rateLimits } from './db/schema.js';
import { purgeGameData } from './game/join.js';
import { expirePendingOrders } from './shop/service.js';
import { errorFields } from './util/logger.js';

export interface MaintenanceReport {
  sessions: number;
  authTokens: number;
  rateLimits: number;
  throttles: number;
  oauthFlows: number;
  joinTokens: number;
  staleGameSessions: number;
  expiredOrders: number;
}

const DAY_MS = 86_400_000;

/**
 * One housekeeping pass: deletes rows that can never matter again and closes what expired. It
 * never touches money records (orders, grants, ledger, payment events) except to fail PENDING
 * orders past their expiry, and never the audit log. Safe to run any time and from several
 * processes (every statement is a plain conditional delete/update).
 */
export async function runMaintenance(deps: Deps): Promise<MaintenanceReport> {
  const now = deps.clock();
  const dayAgo = new Date(now.getTime() - DAY_MS);
  const weekAgo = new Date(now.getTime() - 7 * DAY_MS);

  const sessions = await purgeSessions(deps.db, now);
  const tokens = await deps.db.delete(authTokens).where(or(lt(authTokens.expiresAt, weekAgo), lt(authTokens.usedAt, weekAgo))).returning({ id: authTokens.id });
  const limits = await deps.db.delete(rateLimits).where(lt(rateLimits.windowStart, dayAgo)).returning({ key: rateLimits.key });
  // A throttle row is dead when it is quiet for a day and its lock (if any) has ended.
  const throttles = await deps.db
    .delete(authThrottle)
    .where(and(lt(authThrottle.updatedAt, dayAgo), or(isNull(authThrottle.lockedUntil), sql`${authThrottle.lockedUntil} < ${now.toISOString()}::timestamptz`)))
    .returning({ key: authThrottle.key });
  const oauthFlows = await purgeOauthFlows(deps, now);
  const game = await purgeGameData(deps, now);
  const expiredOrders = await expirePendingOrders(deps, now);

  return {
    sessions,
    authTokens: tokens.length,
    rateLimits: limits.length,
    throttles: throttles.length,
    oauthFlows,
    joinTokens: game.tokens,
    staleGameSessions: game.staleSessions,
    expiredOrders,
  };
}

/** Run runMaintenance every `intervalSeconds`; returns a stop function. Errors are logged, never thrown. */
export function startMaintenance(deps: Deps, intervalSeconds: number): () => void {
  let running = false;
  const tick = async (): Promise<void> => {
    if (running) return;
    running = true;
    try {
      const r = await runMaintenance(deps);
      deps.logger.debug('maintenance pass', { ...r });
    } catch (err) {
      deps.logger.error('maintenance pass failed', errorFields(err));
    } finally {
      running = false;
    }
  };
  const timer = setInterval(() => void tick(), intervalSeconds * 1000);
  timer.unref();
  void tick();
  return () => clearInterval(timer);
}
