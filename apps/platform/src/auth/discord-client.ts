import { z } from 'zod';

/** What the platform needs to know about a Discord user (from `identify email`). */
export interface DiscordUser {
  id: string;
  username: string;
  globalName: string | null;
  /** Only meaningful together with `emailVerified`. */
  email: string | null;
  /** Discord's own `verified` flag for that email. An unverified email is never trusted. */
  emailVerified: boolean;
}

/**
 * The Discord side of OAuth2, behind an interface so tests (and a future provider swap) never
 * touch the network. The production implementation is HttpDiscordClient.
 */
export interface DiscordClient {
  /** The URL the player opens to approve the login (authorization code flow with PKCE S256). */
  authorizeUrl(params: { state: string; codeChallenge: string }): string;
  /** Redeem an authorization code (sending the PKCE verifier) for an access token. Rejects on any failure. */
  exchangeCode(params: { code: string; codeVerifier: string }): Promise<{ accessToken: string }>;
  fetchUser(accessToken: string): Promise<DiscordUser>;
}

export interface HttpDiscordConfig {
  clientId: string;
  clientSecret: string;
  redirectUri: string;
  /** e.g. https://discord.com/api/v10 */
  apiBase: string;
}

const tokenResponse = z.object({ access_token: z.string().min(1), token_type: z.string() });
const userResponse = z.object({
  id: z.string().regex(/^\d{1,25}$/),
  username: z.string().min(1).max(64),
  global_name: z.string().nullish(),
  email: z.string().nullish(),
  verified: z.boolean().optional(),
});

/** Real Discord over HTTPS. `fetchImpl` is injectable: tests pass a function that plays Discord. */
export class HttpDiscordClient implements DiscordClient {
  constructor(
    private readonly cfg: HttpDiscordConfig,
    private readonly fetchImpl: typeof fetch = fetch,
  ) {}

  authorizeUrl(p: { state: string; codeChallenge: string }): string {
    const url = new URL('/oauth2/authorize', new URL(this.cfg.apiBase).origin);
    url.searchParams.set('response_type', 'code');
    url.searchParams.set('client_id', this.cfg.clientId);
    url.searchParams.set('scope', 'identify email');
    url.searchParams.set('redirect_uri', this.cfg.redirectUri);
    url.searchParams.set('state', p.state);
    url.searchParams.set('code_challenge', p.codeChallenge);
    url.searchParams.set('code_challenge_method', 'S256');
    url.searchParams.set('prompt', 'consent');
    return url.toString();
  }

  async exchangeCode(p: { code: string; codeVerifier: string }): Promise<{ accessToken: string }> {
    const res = await this.fetchImpl(`${this.cfg.apiBase}/oauth2/token`, {
      method: 'POST',
      headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'application/json' },
      body: new URLSearchParams({
        client_id: this.cfg.clientId,
        client_secret: this.cfg.clientSecret,
        grant_type: 'authorization_code',
        code: p.code,
        redirect_uri: this.cfg.redirectUri,
        code_verifier: p.codeVerifier,
      }),
      signal: AbortSignal.timeout(8000),
    });
    if (!res.ok) throw new Error(`discord token endpoint answered ${res.status}`);
    const parsed = tokenResponse.safeParse(await res.json());
    if (!parsed.success) throw new Error('discord token response has an unexpected shape');
    return { accessToken: parsed.data.access_token };
  }

  async fetchUser(accessToken: string): Promise<DiscordUser> {
    const res = await this.fetchImpl(`${this.cfg.apiBase}/users/@me`, {
      headers: { authorization: `Bearer ${accessToken}`, accept: 'application/json' },
      signal: AbortSignal.timeout(8000),
    });
    if (!res.ok) throw new Error(`discord users/@me answered ${res.status}`);
    const parsed = userResponse.safeParse(await res.json());
    if (!parsed.success) throw new Error('discord user response has an unexpected shape');
    const u = parsed.data;
    return {
      id: u.id,
      username: u.username,
      globalName: u.global_name ?? null,
      email: u.email ? u.email.toLowerCase() : null,
      emailVerified: u.verified === true && Boolean(u.email),
    };
  }
}
