import type { HttpBindings } from '@hono/node-server';
import type { AccountRow } from '../auth/accounts.js';
import type { Role } from '../db/schema.js';
import type { GameServerRow } from '../servers/service.js';

export interface SessionInfo {
  id: string;
  account: AccountRow;
  roles: Role[];
}

/** Hono environment: the Node bindings and the per-request values the middleware sets. */
export type AppEnv = {
  Bindings: HttpBindings;
  Variables: {
    requestId: string;
    /** Client address as the rate limits see it (X-Forwarded-For when TRUST_PROXY=1, else the socket peer). */
    clientIp: string;
    /** ipGroup(clientIp): the IPv4 address or the IPv6 /64. */
    clientGroup: string;
    /** The socket peer, never taken from a header (the game-server allowlist uses this). */
    peerIp: string | null;
    session?: SessionInfo;
    server?: GameServerRow;
  };
};
