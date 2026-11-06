import { z } from 'zod';
import { getAccountView } from '../../auth/accounts.js';
import {
  completeDiscordSignup,
  discordCompleteSchema,
  discordLinkSchema,
  discordPollSchema,
  discordUnlinkSchema,
  discordWebFinishSchema,
  finishWebLink,
  handleDiscordCallback,
  pollDiscordFlow,
  startDiscordLink,
  startDiscordLogin,
  unlinkDiscord,
} from '../../auth/discord.js';
import {
  changePasswordSchema,
  changePassword,
  forgotPassword,
  forgotPasswordSchema,
  login,
  loginSchema,
  registerAccount,
  registerSchema,
  resendVerification,
  resetPassword,
  resetPasswordSchema,
  tokenSchema,
  verifyEmail,
} from '../../auth/service.js';
import { listSessions, revokeAllSessions, revokeSession } from '../../auth/sessions.js';
import type { Deps } from '../../deps.js';
import { ApiError } from '../../util/errors.js';
import { reqCtx, requireSession, sessionMeta } from '../helpers.js';
import { uuidParam, type Router } from '../router.js';

const callbackQuery = z.object({
  code: z.string().max(512).optional(),
  state: z.string().max(200).optional(),
  error: z.string().max(100).optional(),
});

const FLOW_ID_SHAPE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const CLAIM_SHAPE = /^[A-Za-z0-9_-]{43}$/;

/**
 * The page the browser lands on after Discord. Static text only; nothing from the request is
 * echoed. With a handoff it redirects to the launcher's URL scheme (gage-mp://auth/discord?flow&claim)
 * and offers the same URL as a button; the claim is in the URL only, never in visible text.
 */
function callbackPage(ok: boolean, handoff: { flowId: string; claim: string } | undefined): string {
  const safe = handoff !== undefined && FLOW_ID_SHAPE.test(handoff.flowId) && CLAIM_SHAPE.test(handoff.claim) ? handoff : undefined;
  const url = safe === undefined ? null : `gage-mp://auth/discord?flow=${safe.flowId}&amp;claim=${safe.claim}`;
  const heading = ok ? 'Approved' : 'Discord sign-in did not finish';
  const body =
    url === null
      ? 'Close this window, return to GAGE-MP and try again.'
      : ok
        ? 'Return to GAGE to finish signing in. You can close this window afterwards.'
        : 'Return to GAGE to see what went wrong.';
  const refresh = url === null ? '' : `<meta http-equiv="refresh" content="0;url=${url}">`;
  const button = url === null ? '' : `<p><a class="btn" href="${url}">Return to GAGE</a></p>`;
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="referrer" content="no-referrer">${refresh}<title>GAGE-MP</title><style>body{font:16px system-ui,sans-serif;margin:0;min-height:100vh;display:grid;place-items:center;background:#111;color:#eee}main{max-width:28rem;padding:2rem;text-align:center}h1{font-size:1.25rem}.btn{display:inline-block;padding:.7rem 1.4rem;border-radius:.5rem;background:#eee;color:#111;text-decoration:none;font-weight:600}</style></head><body><main><h1>${heading}</h1><p>${body}</p>${button}</main></body></html>`;
}

/**
 * Where a web link flow's callback sends the browser: the website's account page on the CONFIGURED
 * origin (SITE_ORIGIN; nothing from the request), flow and claim in the fragment so no server or
 * proxy log ever sees the claim. Null when the handoff does not have the expected shape.
 */
export function webHandoffUrl(siteOrigin: string, handoff: { flowId: string; claim: string }): string | null {
  if (!FLOW_ID_SHAPE.test(handoff.flowId) || !CLAIM_SHAPE.test(handoff.claim)) return null;
  return `${siteOrigin}/account/#discord_flow=${handoff.flowId}&discord_claim=${handoff.claim}`;
}

export function registerAuthRoutes(r: Router, deps: Deps): void {
  const { db } = deps;

  r.add(
    {
      method: 'post', path: '/v1/auth/register', summary: 'Create an account', auth: 'none', tags: ['auth'], body: registerSchema,
      description: 'Sends a verification email. Does not log in. The answer is the same whether or not the email already has an account.',
      response: '201 { verificationRequired: boolean }', successStatus: 201,
      errors: ['weak_password', 'username_taken', 'busy'],
      limits: [{ name: 'register', scope: 'ip', limit: 10, windowSeconds: 3600 }],
    },
    async (c, { body }) => c.json(await registerAccount(deps, body), 201),
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/login', summary: 'Log in with email or username and password', auth: 'none', tags: ['auth'], body: loginSchema,
      description: 'Failed attempts are counted per client address and per typed identifier (never per account, so a 429 reveals nothing about which accounts exist); a locked key is refused with 429 before the password is checked. See "Login lockout" above.',
      response: '200 { token, expiresAt, sessionId, account }', errors: ['invalid_credentials', 'busy'],
      limits: [{ name: 'login', scope: 'ip', limit: 120, windowSeconds: 600 }],
    },
    async (c, { body }) => {
      const result = await login(deps, body, reqCtx(c));
      return c.json({ token: result.session.token, expiresAt: result.session.expiresAt, sessionId: result.session.id, account: result.account });
    },
  );

  r.add(
    { method: 'post', path: '/v1/auth/logout', summary: 'Revoke the current session', auth: 'session', tags: ['auth'], response: '204 (no body)', successStatus: 204 },
    async (c) => {
      const s = requireSession(c);
      await revokeSession(db, s.account.id, s.id, 'logout', deps.clock());
      return c.body(null, 204);
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/verify-email', summary: 'Confirm an email address with the emailed token', auth: 'none', tags: ['auth'], body: tokenSchema,
      description: 'The token is single use and expires (EMAIL_VERIFY_TTL_HOURS). POST, not GET, so mail scanners that open links cannot consume it.',
      response: '200 { account }', errors: ['invalid_token'],
      limits: [{ name: 'verify-email', scope: 'ip', limit: 20, windowSeconds: 600 }],
    },
    async (c, { body }) => c.json({ account: await verifyEmail(deps, body.token) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/resend-verification', summary: 'Send a new verification email', auth: 'session', tags: ['auth'],
      response: '202 { sent: true }', successStatus: 202, errors: ['already_verified', 'no_email'],
    },
    async (c) => {
      await resendVerification(deps, requireSession(c).account.id);
      return c.json({ sent: true }, 202);
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/forgot-password', summary: 'Email a password reset token', auth: 'none', tags: ['auth'], body: forgotPasswordSchema,
      description: 'Always 202, whether or not the address has an account. Limited per client IP (checked first) and per email address (3 per hour): a client that is already limited cannot spend the address\'s budget.',
      response: '202 { sent: true }', successStatus: 202,
      limits: [{ name: 'forgot-password', scope: 'ip', limit: 10, windowSeconds: 3600 }],
    },
    async (c, { body }) => {
      await forgotPassword(deps, body.email);
      return c.json({ sent: true }, 202);
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/reset-password', summary: 'Set a new password with a reset token', auth: 'none', tags: ['auth'], body: resetPasswordSchema,
      description: 'Single-use token. Revokes every session of the account, verifies the email and clears the login lockout. If the email was not verified before, any Discord account attached to the account is removed (and the owner is emailed).',
      response: '200 { ok: true }', errors: ['invalid_token', 'weak_password', 'busy'],
      limits: [{ name: 'reset-password', scope: 'ip', limit: 20, windowSeconds: 600 }],
    },
    async (c, { body }) => {
      await resetPassword(deps, body);
      return c.json({ ok: true });
    },
  );

  // --- Discord ---------------------------------------------------------------------------------

  r.add(
    {
      method: 'post', path: '/v1/auth/discord/start', summary: 'Start a Discord login', auth: 'none', tags: ['auth', 'discord'], body: z.object({}).optional().default({}),
      description: 'Returns the URL to open in a browser and the secret to poll with. The poll secret stays in the launcher; the result is only readable with the claim the callback page hands to the launcher (see docs/launcher-discord-flow.md).',
      response: '200 { flowId, authorizeUrl, pollSecret, expiresAt }', errors: ['discord_disabled'],
      limits: [{ name: 'discord-start', scope: 'ip', limit: 20, windowSeconds: 600 }],
    },
    async (c) => c.json(await startDiscordLogin(deps)),
  );

  r.add(
    {
      method: 'get', path: '/v1/auth/discord/callback', summary: 'Discord redirect target (browser page)', auth: 'none', tags: ['auth', 'discord'], query: callbackQuery,
      description: 'Discord sends the player\'s browser here. Validates the single-use state, redeems the code with the PKCE verifier, records the outcome and puts a random claim on the flow (only its hash is stored). A launcher flow gets a small HTML page (200 when the sign-in was approved, 400 otherwise) that redirects to gage-mp://auth/discord?flow=<flowId>&claim=<claim> and shows a "Return to GAGE" button for the same URL. A web link flow (started with handoff "web") gets 303 to <SITE_ORIGIN>/account/#discord_flow=<flowId>&discord_claim=<claim>, whatever the outcome (the account page reads why it failed); the target origin is configuration, never taken from the request, and the claim is in the fragment so no server log sees it. The claim is never displayed. A callback whose state is invalid, used or expired is 400 with no handoff.',
      response: '200 text/html | 303 Location: <SITE_ORIGIN>/account/#discord_flow&discord_claim',
      limits: [{ name: 'discord-callback', scope: 'ip', limit: 60, windowSeconds: 600 }],
    },
    async (c, { query }) => {
      const outcome = await handleDiscordCallback(deps, query);
      c.header('Referrer-Policy', 'no-referrer');
      c.header('Cache-Control', 'no-store');
      if (outcome.handoff?.to === 'web') {
        const target = webHandoffUrl(deps.config.siteOrigin, outcome.handoff);
        if (target !== null) return c.redirect(target, 303);
      }
      c.header('Content-Security-Policy', "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
      return c.html(callbackPage(outcome.ok, outcome.handoff?.to === 'launcher' ? outcome.handoff : undefined), outcome.ok ? 200 : 400);
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/discord/poll', summary: 'Poll a Discord flow', auth: 'none', tags: ['auth', 'discord'], body: discordPollSchema,
      description: 'status pending | failed {code} | needs_username {suggestedUsername, discordUsername} | linked | complete {token, account}. Send the claim from the gage-mp:// URL: without it (or with a wrong one) the answer is always {status: "pending"}, exactly as for a flow nobody approved. complete and linked are returned once and spend the flow and its claim; a link is attached to the account only by this call. A web link flow (handoff "web") is not readable here (404 flow_not_found): the account page finishes it with POST /v1/me/discord/link/finish.',
      response: '200 { status, ... }', errors: ['flow_not_found', 'flow_expired', 'flow_used'],
      limits: [{ name: 'discord-poll', scope: 'ip', limit: 300, windowSeconds: 600 }],
    },
    async (c, { body }) => {
      const result = await pollDiscordFlow(deps, body, sessionMeta(c));
      if (result.status === 'complete') {
        return c.json({ status: 'complete', token: result.session.token, expiresAt: result.session.expiresAt, sessionId: result.session.id, account: result.account });
      }
      return c.json(result);
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/auth/discord/complete', summary: 'Choose a username to finish a first Discord login', auth: 'none', tags: ['auth', 'discord'], body: discordCompleteSchema,
      description: 'Needs the claim as well as the poll secret (a wrong claim is 404 flow_not_found). Spends the flow and its claim.',
      response: '200 { token, expiresAt, sessionId, account }', errors: ['flow_not_found', 'flow_expired', 'flow_used', 'flow_state', 'username_taken', 'email_in_use'],
      limits: [{ name: 'discord-complete', scope: 'ip', limit: 20, windowSeconds: 600 }],
    },
    async (c, { body }) => {
      const result = await completeDiscordSignup(deps, body, sessionMeta(c));
      return c.json({ token: result.session.token, expiresAt: result.session.expiresAt, sessionId: result.session.id, account: result.account });
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/me/discord/link', summary: 'Start linking Discord to the signed-in account', auth: 'session', tags: ['me', 'discord'], body: discordLinkSchema,
      description: 'Re-authenticates with the account\'s current password (`password`; wrong passwords count towards the same lockout as a password change, keyed on the account and the client address: 401 invalid_credentials, then 429 rate_limited). `handoff` "launcher" (default) finishes like a Discord login (callback, gage-mp:// handoff, claimed poll answering "linked"); "web" makes the callback redirect to the website\'s account page, which finishes with POST /v1/me/discord/link/finish. The identity is attached only by that last step, and is refused there (failed, code password_changed) when the password changed after the start. The owner is emailed.',
      response: '200 { flowId, authorizeUrl, pollSecret, expiresAt }', errors: ['discord_disabled', 'already_has_discord', 'password_required', 'invalid_credentials', 'rate_limited', 'busy'],
      limits: [{ name: 'discord-link', scope: 'account', limit: 10, windowSeconds: 600 }],
    },
    async (c, { body }) => c.json(await startDiscordLink(deps, requireSession(c).account.id, body, reqCtx(c))),
  );

  r.add(
    {
      method: 'post', path: '/v1/me/discord/link/finish', summary: 'Finish a web Discord link on the account page', auth: 'session', tags: ['me', 'discord'], body: discordWebFinishSchema,
      description: 'For a flow started with handoff "web": `flowId` and `claim` come from the account page\'s URL fragment (#discord_flow&discord_claim), `pollSecret` from the tab that started the flow. The session must belong to the account that started it. Any mismatch (wrong or missing secret, claim of another flow, another account\'s session, a launcher flow) is 404 flow_not_found. Answers { status: "linked" } once (the identity is attached by this call; the same pair again is a harmless no-op) or { status: "failed", code } (access_denied, discord_error, invalid_request, discord_already_linked, already_has_discord, password_changed).',
      response: '200 { status: "linked" } | { status: "failed", code }', errors: ['flow_not_found', 'flow_expired', 'flow_used'],
      limits: [{ name: 'discord-link-finish', scope: 'account', limit: 30, windowSeconds: 600 }],
    },
    async (c, { body }) => c.json(await finishWebLink(deps, requireSession(c).account.id, body)),
  );

  r.add(
    {
      method: 'delete', path: '/v1/me/discord', summary: 'Unlink Discord', auth: 'session', tags: ['me', 'discord'], body: discordUnlinkSchema,
      description: 'Refused (409 last_login_method) when Discord is the account\'s only way in (no password). Re-authenticates with the current password (`password`; the same lockout as linking and changing the password). The owner is emailed.',
      response: '204 (no body)', successStatus: 204, errors: ['last_login_method', 'not_linked', 'invalid_credentials', 'rate_limited', 'busy'],
      limits: [{ name: 'discord-unlink', scope: 'account', limit: 10, windowSeconds: 600 }],
    },
    async (c, { body }) => {
      await unlinkDiscord(deps, requireSession(c).account.id, body, reqCtx(c));
      return c.body(null, 204);
    },
  );

  // --- The signed-in account -------------------------------------------------------------------

  r.add(
    { method: 'get', path: '/v1/me', summary: 'The signed-in account', auth: 'session', tags: ['me'], response: '200 { account }' },
    async (c) => c.json({ account: await getAccountView(db, requireSession(c).account, deps.clock()) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/me/password', summary: 'Change or set the password', auth: 'session', tags: ['me'], body: changePasswordSchema,
      description: 'Revokes every other session and voids outstanding password-reset links. A Discord-only account may omit currentPassword to set its first password (it needs a verified email). Limited to 10 calls per hour per account.',
      response: '200 { revokedSessions }', errors: ['invalid_credentials', 'weak_password', 'current_password_required', 'email_required', 'email_not_verified', 'busy'],
      limits: [{ name: 'change-password', scope: 'account', limit: 10, windowSeconds: 3600 }],
    },
    async (c, { body }) => {
      const s = requireSession(c);
      return c.json(await changePassword(deps, s.account.id, s.id, body, reqCtx(c)));
    },
  );

  r.add(
    { method: 'get', path: '/v1/me/sessions', summary: 'List signed-in devices', auth: 'session', tags: ['me'], response: '200 { sessions: [{ id, deviceName, userAgent, ip, createdAt, lastSeenAt, expiresAt, current }] }' },
    async (c) => {
      const s = requireSession(c);
      return c.json({ sessions: await listSessions(db, s.account.id, s.id, deps.clock()) });
    },
  );

  r.add(
    { method: 'delete', path: '/v1/me/sessions/:id', summary: 'Sign out one device', auth: 'session', tags: ['me'], response: '204 (no body)', successStatus: 204, errors: ['session_not_found'] },
    async (c) => {
      const s = requireSession(c);
      const ok = await revokeSession(db, s.account.id, uuidParam(c, 'id'), 'revoked_by_user', deps.clock());
      if (!ok) throw new ApiError(404, 'session_not_found', 'No such active session.');
      return c.body(null, 204);
    },
  );

  r.add(
    { method: 'delete', path: '/v1/me/sessions', summary: 'Sign out every other device', auth: 'session', tags: ['me'], response: '200 { revoked }' },
    async (c) => {
      const s = requireSession(c);
      return c.json({ revoked: await revokeAllSessions(db, s.account.id, { exceptSessionId: s.id, reason: 'revoked_by_user' }, deps.clock()) });
    },
  );

}
