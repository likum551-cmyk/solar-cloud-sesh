// The Solar RP platform's account API (apps/platform/docs/api.md), reached same-origin under /api:
// Caddy proxies https://solar-rp.com/api/* to the platform's public listener with /api stripped, and the
// Vite dev server does the same (vite.config.ts). Every response is checked at this boundary: a body that
// is not the documented shape is a failure, never a half-filled object.

export interface Ban {
  reason: string;
  /** null = permanent */
  expiresAt: string | null;
}

export interface DiscordLink {
  id: string;
  /** null when Discord did not report one at link time */
  username: string | null;
}

export interface Account {
  id: string;
  username: string;
  email: string | null;
  emailVerified: boolean;
  hasPassword: boolean;
  /** the linked Discord account; null = none */
  discord: DiscordLink | null;
  ban: Ban | null;
  createdAt: string;
}

/** POST /v1/me/discord/link: where to send the browser, and the secret this tab keeps until it comes back. */
export interface DiscordLinkStart {
  flowId: string;
  authorizeUrl: string;
  pollSecret: string;
  expiresAt: string;
}

/** POST /v1/me/discord/link/finish */
export type DiscordLinkFinish = { status: 'linked' } | { status: 'failed'; code: string };

export interface SessionInfo {
  id: string;
  deviceName: string | null;
  userAgent: string | null;
  ip: string | null;
  createdAt: string;
  lastSeenAt: string;
  expiresAt: string;
  current: boolean;
}

export type QuestKind = 'discord_link' | 'discord_guild' | 'visit';
export type QuestStatus = 'available' | 'started' | 'claimable' | 'claimed' | 'locked';
export type QuestLockReason = 'discord_required' | 'discord_already_claimed' | 'not_available';

/** One quest as GET /v1/me/quests describes it for this account (apps/platform/docs/api.md, "Quests"). */
export interface Quest {
  slug: string;
  kind: QuestKind;
  title: string;
  description: string;
  /** the page a visit quest opens (https only); null for the other kinds */
  url: string | null;
  reward: { currency: string; amount: number };
  status: QuestStatus;
  lockReason: QuestLockReason | null;
  /** seconds until a started visit quest can be claimed (0 once it can); null when not started */
  waitSeconds: number | null;
  claimedAt: string | null;
}

export interface QuestList {
  discordLinked: boolean;
  visitMinSeconds: number;
  quests: Quest[];
}

export interface QuestClaim {
  quest: Quest;
  credited: { currency: string; amount: number };
  balance: number;
}

export interface Balance {
  currency: string;
  balance: number;
}

export interface LoginResult {
  token: string;
  account: Account;
}

export type ApiFailure =
  /** The platform answered with its documented error body. */
  | { kind: 'api'; status: number; code: string; message: string; requestId: string | null; details: Record<string, unknown> | null; retryAfterSeconds: number | null }
  /** No answer at all (offline, DNS, connection refused, aborted). */
  | { kind: 'network' }
  /** An answer that is not the platform's (a proxy's 502 page, a 404 from the static site, a changed shape). */
  | { kind: 'bad-response'; status: number };

export type Result<T> = { ok: true; value: T } | { ok: false; failure: ApiFailure };

export interface AccountApi {
  register(input: { username: string; email: string; password: string }): Promise<Result<{ verificationRequired: boolean }>>;
  login(input: { identifier: string; password: string; deviceName?: string }): Promise<Result<LoginResult>>;
  logout(token: string): Promise<Result<null>>;
  me(token: string): Promise<Result<Account>>;
  sessions(token: string): Promise<Result<SessionInfo[]>>;
  revokeSession(token: string, id: string): Promise<Result<null>>;
  revokeOtherSessions(token: string): Promise<Result<{ revoked: number }>>;
  changePassword(token: string, input: { currentPassword?: string; newPassword: string }): Promise<Result<{ revokedSessions: number }>>;
  wallet(token: string): Promise<Result<Balance[]>>;
  quests(token: string): Promise<Result<QuestList>>;
  startQuest(token: string, slug: string): Promise<Result<Quest>>;
  claimQuest(token: string, slug: string): Promise<Result<QuestClaim>>;
  discordLinkStart(token: string, password: string): Promise<Result<DiscordLinkStart>>;
  discordLinkFinish(token: string, input: { flowId: string; pollSecret: string; claim: string }): Promise<Result<DiscordLinkFinish>>;
}

type Json = Record<string, unknown>;
const isObj = (v: unknown): v is Json => typeof v === 'object' && v !== null && !Array.isArray(v);
const str = (v: unknown): v is string => typeof v === 'string';
const strOrNull = (v: unknown): v is string | null => v === null || typeof v === 'string';
const TOKEN = /^gms_[A-Za-z0-9_-]{20,200}$/;

export function parseAccount(v: unknown): Account | null {
  if (!isObj(v)) return null;
  const { id, username, email, emailVerified, hasPassword, discord, ban, createdAt } = v;
  if (!str(id) || !str(username) || !strOrNull(email) || typeof emailVerified !== 'boolean' || typeof hasPassword !== 'boolean' || !str(createdAt)) return null;
  let b: Ban | null = null;
  if (ban !== null && ban !== undefined) {
    if (!isObj(ban) || !str(ban.reason) || !strOrNull(ban.expiresAt)) return null;
    b = { reason: ban.reason, expiresAt: ban.expiresAt };
  }
  let d: DiscordLink | null = null;
  if (discord !== null && discord !== undefined) {
    if (!isObj(discord) || !str(discord.id) || !strOrNull(discord.username ?? null)) return null;
    d = { id: discord.id, username: (discord.username as string | null | undefined) ?? null };
  }
  return { id, username, email, emailVerified, hasPassword, discord: d, ban: b, createdAt };
}

const FLOW_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const SECRET = /^[A-Za-z0-9_-]{16,200}$/;

function parseLinkStart(v: unknown): DiscordLinkStart | null {
  if (!isObj(v)) return null;
  const { flowId, authorizeUrl, pollSecret, expiresAt } = v;
  if (!str(flowId) || !FLOW_ID.test(flowId) || !str(authorizeUrl) || !str(pollSecret) || !SECRET.test(pollSecret) || !str(expiresAt)) return null;
  return { flowId, authorizeUrl, pollSecret, expiresAt };
}

function parseLinkFinish(v: unknown): DiscordLinkFinish | null {
  if (!isObj(v)) return null;
  if (v.status === 'linked') return { status: 'linked' };
  if (v.status === 'failed' && str(v.code)) return { status: 'failed', code: v.code };
  return null;
}

export function parseSession(v: unknown): SessionInfo | null {
  if (!isObj(v)) return null;
  const { id, deviceName, userAgent, ip, createdAt, lastSeenAt, expiresAt, current } = v;
  if (!str(id) || !strOrNull(deviceName) || !strOrNull(userAgent) || !strOrNull(ip)) return null;
  if (!str(createdAt) || !str(lastSeenAt) || !str(expiresAt) || typeof current !== 'boolean') return null;
  return { id, deviceName, userAgent, ip, createdAt, lastSeenAt, expiresAt, current };
}

const QUEST_KINDS: readonly string[] = ['discord_link', 'discord_guild', 'visit'];
const QUEST_STATUSES: readonly string[] = ['available', 'started', 'claimable', 'claimed', 'locked'];
const LOCK_REASONS: readonly string[] = ['discord_required', 'discord_already_claimed', 'not_available'];
const SLUG = /^[a-z0-9][a-z0-9_]{1,47}$/;
const count = (v: unknown): v is number => typeof v === 'number' && Number.isSafeInteger(v) && v >= 0;

/** Only an https URL is ever put on a link: anything else from the API is dropped (the quest then has no page). */
function httpsUrl(v: unknown): string | null {
  if (typeof v !== 'string') return null;
  try {
    return new URL(v).protocol === 'https:' ? v : null;
  } catch {
    return null;
  }
}

export function parseQuest(v: unknown): Quest | null {
  if (!isObj(v)) return null;
  const { slug, kind, title, description, url, reward, status, lockReason, waitSeconds, claimedAt } = v;
  if (!str(slug) || !SLUG.test(slug) || !str(kind) || !QUEST_KINDS.includes(kind) || !str(title) || !str(description)) return null;
  if (!str(status) || !QUEST_STATUSES.includes(status)) return null;
  if (lockReason !== null && (!str(lockReason) || !LOCK_REASONS.includes(lockReason))) return null;
  if (!isObj(reward) || !str(reward.currency) || !count(reward.amount)) return null;
  if (waitSeconds !== null && !count(waitSeconds)) return null;
  if (!strOrNull(claimedAt) || !(url === null || str(url))) return null;
  return {
    slug,
    kind: kind as QuestKind,
    title,
    description,
    url: httpsUrl(url),
    reward: { currency: reward.currency, amount: reward.amount },
    status: status as QuestStatus,
    lockReason: lockReason as QuestLockReason | null,
    waitSeconds,
    claimedAt,
  };
}

function parseQuestList(v: unknown): QuestList | null {
  if (!isObj(v) || typeof v.discordLinked !== 'boolean' || !count(v.visitMinSeconds) || !Array.isArray(v.quests)) return null;
  const quests = v.quests.map(parseQuest);
  return quests.every((q): q is Quest => q !== null) ? { discordLinked: v.discordLinked, visitMinSeconds: v.visitMinSeconds, quests } : null;
}

function retryAfter(h: string | null): number | null {
  if (h === null) return null;
  const n = Number(h);
  return Number.isFinite(n) && n >= 0 ? Math.ceil(n) : null;
}

/**
 * The account API on `base` (default `/api`). `fetchImpl` is injectable for tests; by default it is the
 * page's own fetch, called through a wrapper so it is never invoked detached from `window`.
 */
export function createAccountApi(base = '/api', fetchImpl: typeof fetch = (input, init) => fetch(input, init)): AccountApi {
  async function call<T>(method: string, path: string, opts: { token?: string; body?: unknown }, read: (status: number, body: unknown) => T | null): Promise<Result<T>> {
    const headers: Record<string, string> = { Accept: 'application/json' };
    if (opts.token !== undefined) headers.Authorization = `Bearer ${opts.token}`;
    if (opts.body !== undefined) headers['Content-Type'] = 'application/json';
    let res: Response;
    try {
      res = await fetchImpl(`${base}${path}`, {
        method,
        headers,
        body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
        cache: 'no-store',
        credentials: 'omit', // bearer tokens only; the platform never uses cookies
      });
    } catch {
      return { ok: false, failure: { kind: 'network' } };
    }
    let body: unknown = null;
    const text = await res.text().catch(() => null);
    if (text === null) return { ok: false, failure: { kind: 'network' } };
    if (text.length > 0) {
      try {
        body = JSON.parse(text);
      } catch {
        return { ok: false, failure: { kind: 'bad-response', status: res.status } };
      }
    }
    if (!res.ok) {
      const e = isObj(body) ? body.error : undefined;
      if (!isObj(e) || !str(e.code) || !str(e.message)) return { ok: false, failure: { kind: 'bad-response', status: res.status } };
      return {
        ok: false,
        failure: {
          kind: 'api',
          status: res.status,
          code: e.code,
          message: e.message,
          requestId: str(e.requestId) ? e.requestId : res.headers.get('X-Request-Id'),
          details: isObj(e.details) ? e.details : null,
          retryAfterSeconds: retryAfter(res.headers.get('Retry-After')),
        },
      };
    }
    const value = read(res.status, body);
    if (value === null) return { ok: false, failure: { kind: 'bad-response', status: res.status } };
    return { ok: true, value };
  }

  // 204 answers carry no body: the reader returns `true` (any non-null) so `null` still means "bad shape".
  async function callNoBody(method: string, path: string, token: string): Promise<Result<null>> {
    const r = await call(method, path, { token }, (status) => (status === 204 ? true : null));
    return r.ok ? { ok: true, value: null } : r;
  }

  return {
    register: (input) =>
      call('POST', '/v1/auth/register', { body: input }, (_s, b) =>
        isObj(b) && typeof b.verificationRequired === 'boolean' ? { verificationRequired: b.verificationRequired } : null,
      ),
    login: (input) =>
      call('POST', '/v1/auth/login', { body: input }, (_s, b) => {
        if (!isObj(b) || !str(b.token) || !TOKEN.test(b.token)) return null;
        const account = parseAccount(b.account);
        return account ? { token: b.token, account } : null;
      }),
    logout: (token) => callNoBody('POST', '/v1/auth/logout', token),
    me: (token) => call('GET', '/v1/me', { token }, (_s, b) => (isObj(b) ? parseAccount(b.account) : null)),
    sessions: (token) =>
      call('GET', '/v1/me/sessions', { token }, (_s, b) => {
        if (!isObj(b) || !Array.isArray(b.sessions)) return null;
        const list = b.sessions.map(parseSession);
        return list.every((s): s is SessionInfo => s !== null) ? list : null;
      }),
    revokeSession: (token, id) => callNoBody('DELETE', `/v1/me/sessions/${encodeURIComponent(id)}`, token),
    revokeOtherSessions: (token) =>
      call('DELETE', '/v1/me/sessions', { token }, (_s, b) => (isObj(b) && typeof b.revoked === 'number' ? { revoked: b.revoked } : null)),
    changePassword: (token, input) =>
      call('POST', '/v1/me/password', { token, body: input }, (_s, b) =>
        isObj(b) && typeof b.revokedSessions === 'number' ? { revokedSessions: b.revokedSessions } : null,
      ),
    wallet: (token) =>
      call('GET', '/v1/me/wallet', { token }, (_s, b) => {
        if (!isObj(b) || !Array.isArray(b.balances)) return null;
        const list = b.balances.map((x) => (isObj(x) && str(x.currency) && typeof x.balance === 'number' && Number.isSafeInteger(x.balance) ? { currency: x.currency, balance: x.balance } : null));
        return list.every((x): x is Balance => x !== null) ? list : null;
      }),
    quests: (token) => call('GET', '/v1/me/quests', { token }, (_s, b) => parseQuestList(b)),
    startQuest: (token, slug) => call('POST', `/v1/me/quests/${encodeURIComponent(slug)}/start`, { token }, (_s, b) => (isObj(b) ? parseQuest(b.quest) : null)),
    claimQuest: (token, slug) =>
      call('POST', `/v1/me/quests/${encodeURIComponent(slug)}/claim`, { token }, (_s, b) => {
        if (!isObj(b) || !isObj(b.credited) || !str(b.credited.currency) || !count(b.credited.amount) || typeof b.balance !== 'number') return null;
        const quest = parseQuest(b.quest);
        return quest ? { quest, credited: { currency: b.credited.currency, amount: b.credited.amount }, balance: b.balance } : null;
      }),
    discordLinkStart: (token, password) => call('POST', '/v1/me/discord/link', { token, body: { password, handoff: 'web' } }, (_s, b) => parseLinkStart(b)),
    discordLinkFinish: (token, input) => call('POST', '/v1/me/discord/link/finish', { token, body: input }, (_s, b) => parseLinkFinish(b)),
  };
}
