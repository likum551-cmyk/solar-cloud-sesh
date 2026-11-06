import { and, eq, gt, isNull, lt } from 'drizzle-orm';
import { z } from 'zod';
import type { Deps } from '../deps.js';
import { accounts, oauthFlows, oauthIdentities } from '../db/schema.js';
import { discordLinkedMessage, discordUnlinkedMessage } from '../email.js';
import { ApiError, isUniqueViolation } from '../util/errors.js';
import { errorFields } from '../util/logger.js';
import { hashToken, pkceChallenge, randomToken, safeEqual } from '../util/tokens.js';
import { findAccountByEmail, findAccountById, findAccountByUsername, isReservedUsername, passwordSchema, usernameSchema } from './accounts.js';
import { createAccountRow, sendSafely, startSession, verifyCurrentPassword, type LoginResult, type ReqCtx } from './service.js';
import type { SessionMeta } from './sessions.js';
import type { DiscordUser } from './discord-client.js';

// Discord OAuth2 (authorization code + PKCE S256) for a launcher that cannot receive a redirect:
//
//   1. launcher  POST /v1/auth/discord/start {intent}         -> flowId, pollSecret, authorizeUrl
//   2. player    opens authorizeUrl in the browser, approves
//   3. Discord   redirects the browser to GET /v1/auth/discord/callback?code&state
//   4. platform  checks state (single use, 10 min), redeems the code with the PKCE verifier, reads
//                the Discord profile, stores the outcome on the flow and puts a random CLAIM on it
//                (only its hash is stored). The browser page redirects to
//                gage-mp://auth/discord?flow=<id>&claim=<claim>
//   5. launcher  receives that URL (the gage-mp scheme is registered per user), then
//                POST /v1/auth/discord/poll {flowId, pollSecret, claim} until it is not "pending"
//                (a new player gets "needs_username" and finishes with /discord/complete, which also
//                needs the claim)
//
// Two secrets, two channels. The poll secret never leaves the launcher that started the flow; the
// claim only ever reaches the browser of whoever approved, and from there the launcher on the same
// machine. Whoever starts a flow and sends the authorize URL to somebody else therefore cannot read
// the result of that person's approval (login phishing): without the claim, /poll answers
// "pending" exactly as it does for an unapproved flow and /complete is refused. The claim is
// single use (cleared when the flow is consumed), compared in constant time and dies with the flow.
// docs/launcher-discord-flow.md says what the launcher has to do.
//
// Linking from the website (handoff "web", link flows only) is the same flow with the same two
// secrets; only the handoff channel differs:
//
//   1. website   POST /v1/me/discord/link {password, handoff: "web"} with the session
//                -> flowId, pollSecret, authorizeUrl; the poll secret stays in the tab's sessionStorage
//   2. player    the tab navigates to authorizeUrl and approves
//   3. platform  the callback redirects (303) to <SITE_ORIGIN>/account/#discord_flow=<id>&discord_claim=<claim>.
//                SITE_ORIGIN is configuration, never a request parameter (no open redirect); the claim
//                rides in the FRAGMENT, which browsers never send to a server, so no access log holds it
//   4. website   POST /v1/me/discord/link/finish {flowId, pollSecret, claim} with the SAME account's session
//
// A web flow cannot be read through the unauthenticated poll, and the finish needs the claim (only the
// approving browser has it), the poll secret (only the starting tab has it) AND a session of the
// account that started the flow. Whoever starts a web flow and sends the authorize URL to a victim
// gets the claim delivered to the victim's browser, where there is no poll secret and the session (if
// any) is the victim's: nothing gets linked.
//
// Account safety rules:
//   - A Discord identity is matched on its immutable Discord user id, never on email or name.
//   - Never auto-merge by email: if the Discord email (verified or not) belongs to an existing
//     GAGE account, login fails with `email_in_use`; that player logs in with their password and
//     links Discord from settings (intent "link").
//   - An unverified Discord email is never stored on the account; a verified one is stored and
//     counts as verified.
//   - Linking and unlinking change how the account can be signed in to, so both re-authenticate with
//     the account's current password (throttled like a login; Solar RP sends no email, so a verified
//     email cannot be the proof). A link happens only when the claimed poll / web finish completes
//     it (never as a side effect of somebody else's approval), and is refused there when the
//     password changed after the flow started (a reset by the real owner of a squatted address
//     voids every link the squatter had in flight; the reset itself strips identities from an
//     account whose email was never verified). The owner is emailed whenever an identity is linked
//     or unlinked.
//   - The state is random, hashed at rest and consumed by one conditional UPDATE, so a replayed or
//     forged callback matches nothing.

const flowRef = {
  flowId: z.guid(),
  pollSecret: z.string().min(16).max(200),
};
const claimField = z.string().min(16).max(200);

/** `claim` is optional on purpose: a poll without it (or with a wrong one) is answered "pending". */
export const discordPollSchema = z.object({ ...flowRef, claim: claimField.optional() });
export const discordCompleteSchema = z.object({ ...flowRef, claim: claimField, username: usernameSchema });
/** Start a link: the current password (re-authentication) and where the claim goes back to. */
export const discordLinkSchema = z.object({
  password: passwordSchema,
  handoff: z.enum(['launcher', 'web']).default('launcher'),
});
/** Finish a web link from the account page: both secrets, with the session of the account that started it. */
export const discordWebFinishSchema = z.object({ ...flowRef, claim: claimField });
export const discordUnlinkSchema = z.object({ password: passwordSchema });

export type Handoff = 'launcher' | 'web';

export interface DiscordStart {
  flowId: string;
  authorizeUrl: string;
  pollSecret: string;
  expiresAt: Date;
}

function requireDiscord(deps: Deps): NonNullable<Deps['discord']> {
  if (!deps.discord || !deps.config.discord) throw new ApiError(404, 'discord_disabled', 'Discord login is not configured on this server.');
  return deps.discord;
}

/** Begin a Discord login (launcher only). */
export async function startDiscordLogin(deps: Deps): Promise<DiscordStart> {
  return createFlow(deps, { intent: 'login', accountId: null, handoff: 'launcher' });
}

/**
 * Begin linking Discord to the signed-in account. Refused when the account already has a Discord
 * identity (409 `already_has_discord`) or has no password to re-authenticate with (409
 * `password_required`; such an account signs in through its Discord already), then the current
 * password is checked (401 `invalid_credentials`, 429 `rate_limited`; see verifyCurrentPassword).
 */
export async function startDiscordLink(
  deps: Deps,
  accountId: string,
  input: z.infer<typeof discordLinkSchema>,
  ctx: ReqCtx,
): Promise<DiscordStart> {
  requireDiscord(deps);
  const account = await findAccountById(deps.db, accountId);
  if (!account) throw new ApiError(404, 'account_not_found', 'No such account.');
  const [mine] = await deps.db
    .select({ id: oauthIdentities.id })
    .from(oauthIdentities)
    .where(and(eq(oauthIdentities.accountId, accountId), eq(oauthIdentities.provider, 'discord')))
    .limit(1);
  if (mine) throw new ApiError(409, 'already_has_discord', 'A Discord account is already linked. Unlink it first.');
  if (account.passwordHash === null) throw new ApiError(409, 'password_required', 'Set a password before linking Discord.');
  await verifyCurrentPassword(deps, { id: account.id, passwordHash: account.passwordHash }, input.password, ctx, deps.clock());
  return createFlow(deps, { intent: 'link', accountId, handoff: input.handoff });
}

async function createFlow(deps: Deps, f: { intent: 'login' | 'link'; accountId: string | null; handoff: Handoff }): Promise<DiscordStart> {
  const discord = requireDiscord(deps);
  const { intent, accountId } = f;
  if (f.handoff === 'web' && intent !== 'link') throw new Error('a web handoff is for link flows only');
  const now = deps.clock();
  const state = randomToken(32);
  const verifier = randomToken(48); // 64 chars, inside RFC 7636's 43..128
  const pollSecret = randomToken(32);
  const expiresAt = new Date(now.getTime() + (deps.config.discord?.flowTtlSeconds ?? 600) * 1000);
  const [row] = await deps.db
    .insert(oauthFlows)
    .values({
      provider: 'discord',
      intent,
      stateHash: hashToken(state),
      codeVerifier: verifier,
      pollSecretHash: hashToken(pollSecret),
      accountId: intent === 'link' ? accountId : null,
      handoff: f.handoff,
      createdAt: now,
      expiresAt,
    })
    .returning({ id: oauthFlows.id });
  if (!row) throw new Error('oauth flow insert returned no row');
  return { flowId: row.id, authorizeUrl: discord.authorizeUrl({ state, codeChallenge: pkceChallenge(verifier) }), pollSecret, expiresAt };
}

export interface CallbackOutcome {
  ok: boolean;
  /** Stable code for the browser page and logs. */
  code: string;
  /**
   * What the browser is handed: to the launcher (gage-mp://auth/discord?flow&claim) or, for a web
   * flow, to the website's account page (a redirect with flow and claim in the fragment). Absent
   * when the state was not valid. Never logged, never shown.
   */
  handoff?: { flowId: string; claim: string; to: Handoff };
}

interface StoredProfile {
  id: string;
  username: string;
  globalName: string | null;
  /** Present only when Discord says it is verified (the email an account created from this profile gets). */
  email: string | null;
  /** The email Discord reported, verified or not, and whether it is verified (stored on a linked identity as provider data only). */
  providerEmail?: string | null;
  emailVerified?: boolean;
}

/**
 * Handle Discord's redirect. Never throws for a bad request: it reports the outcome (the browser
 * only sees a generic page) and records it on the flow for the launcher's claimed poll. Any flow
 * whose state matched, successful or not, gets a claim, so the launcher (and only the launcher
 * the approving browser hands it to) can read the outcome.
 */
export async function handleDiscordCallback(
  deps: Deps,
  params: { code?: string | undefined; state?: string | undefined; error?: string | undefined },
): Promise<CallbackOutcome> {
  const discord = deps.discord;
  if (!discord) return { ok: false, code: 'discord_disabled' };
  if (!params.state || params.state.length > 200) return { ok: false, code: 'invalid_state' };
  const now = deps.clock();

  // Claim the state: only a flow that is pending, not yet called back and not expired matches.
  const [flow] = await deps.db
    .update(oauthFlows)
    .set({ callbackAt: now })
    .where(
      and(
        eq(oauthFlows.stateHash, hashToken(params.state)),
        eq(oauthFlows.status, 'pending'),
        isNull(oauthFlows.callbackAt),
        gt(oauthFlows.expiresAt, now),
      ),
    )
    .returning();
  if (!flow) return { ok: false, code: 'invalid_state' };

  const claim = randomToken(32);
  const claimHash = hashToken(claim);
  const handoff = { flowId: flow.id, claim, to: flow.handoff === 'web' ? ('web' as const) : ('launcher' as const) };

  const fail = async (code: string): Promise<CallbackOutcome> => {
    await deps.db.update(oauthFlows).set({ status: 'failed', errorCode: code, codeVerifier: null, claimHash }).where(eq(oauthFlows.id, flow.id));
    return { ok: false, code, handoff };
  };
  const finish = async (patch: { status: 'login_ready' | 'link_ready' | 'needs_username'; resultAccountId?: string; profile?: StoredProfile }): Promise<CallbackOutcome> => {
    await deps.db
      .update(oauthFlows)
      .set({
        status: patch.status,
        resultAccountId: patch.resultAccountId ?? null,
        profile: patch.profile ?? null,
        codeVerifier: null,
        claimHash,
      })
      .where(eq(oauthFlows.id, flow.id));
    return { ok: true, code: patch.status, handoff };
  };

  if (params.error) return fail(params.error === 'access_denied' ? 'access_denied' : 'discord_error');
  if (!params.code || params.code.length > 512 || !flow.codeVerifier) return fail('invalid_request');

  let user: DiscordUser;
  try {
    const { accessToken } = await discord.exchangeCode({ code: params.code, codeVerifier: flow.codeVerifier });
    user = await discord.fetchUser(accessToken);
  } catch (err) {
    deps.logger.warn('discord exchange failed', errorFields(err));
    return fail('discord_error');
  }

  const profile: StoredProfile = {
    id: user.id,
    username: user.username,
    globalName: user.globalName,
    email: user.emailVerified ? user.email : null,
    providerEmail: user.email,
    emailVerified: user.emailVerified,
  };

  if (flow.intent === 'link') {
    // Nothing is attached here: whoever approved this flow is not necessarily whoever started it.
    // The link happens when the launcher that started the flow presents the claim (pollDiscordFlow).
    if (!flow.accountId) return fail('invalid_request');
    return finish({ status: 'link_ready', resultAccountId: flow.accountId, profile });
  }

  // intent = login
  const [identity] = await deps.db
    .select()
    .from(oauthIdentities)
    .where(and(eq(oauthIdentities.provider, 'discord'), eq(oauthIdentities.providerUserId, user.id)))
    .limit(1);
  if (identity) return finish({ status: 'login_ready', resultAccountId: identity.accountId });

  if (user.email !== null && (await findAccountByEmail(deps.db, user.email))) return fail('email_in_use');
  return finish({ status: 'needs_username', profile });
}

export type PollResult =
  | { status: 'pending' }
  | { status: 'failed'; code: string }
  | { status: 'needs_username'; suggestedUsername: string; discordUsername: string }
  | { status: 'linked' }
  | ({ status: 'complete' } & LoginResult);

/** Load the flow for a poll/complete call. A wrong secret is indistinguishable from an unknown flow. */
async function loadFlow(deps: Deps, flowId: string, pollSecret: string) {
  const [flow] = await deps.db.select().from(oauthFlows).where(eq(oauthFlows.id, flowId)).limit(1);
  if (!flow || !safeEqual(flow.pollSecretHash, hashToken(pollSecret))) {
    throw new ApiError(404, 'flow_not_found', 'No such login flow.');
  }
  if (flow.status === 'consumed') throw new ApiError(410, 'flow_used', 'This login flow was already completed.');
  if (flow.expiresAt.getTime() <= deps.clock().getTime()) {
    throw new ApiError(410, 'flow_expired', 'This login flow expired. Start again.');
  }
  return flow;
}

/** True when `claim` is the one handed to the approving browser (constant-time; false when the flow has none yet or it was spent). */
function claimMatches(flow: { claimHash: string | null }, claim: string | undefined): boolean {
  if (claim === undefined) return false;
  const matches = safeEqual(flow.claimHash ?? '', hashToken(claim));
  return matches && flow.claimHash !== null;
}

/**
 * The launcher's poll. Without the claim (or with a wrong one) the answer is always "pending", so
 * an observer learns nothing about whether anyone approved. A finished login or link is returned
 * once and then the flow (and its claim) is spent.
 */
export async function pollDiscordFlow(deps: Deps, input: z.infer<typeof discordPollSchema>, meta: SessionMeta): Promise<PollResult> {
  const flow = await loadFlow(deps, input.flowId, input.pollSecret);
  // A web flow is finished only on the account page, with the account's session (finishWebLink).
  if (flow.handoff !== 'launcher') throw new ApiError(404, 'flow_not_found', 'No such login flow.');
  if (!claimMatches(flow, input.claim)) return { status: 'pending' };
  const now = deps.clock();
  switch (flow.status) {
    case 'pending':
      return { status: 'pending' };
    case 'failed':
      return { status: 'failed', code: flow.errorCode ?? 'discord_error' };
    case 'needs_username': {
      const profile = flow.profile as StoredProfile | null;
      return { status: 'needs_username', suggestedUsername: await suggestUsername(deps, profile?.username ?? 'player'), discordUsername: profile?.username ?? '' };
    }
    case 'login_ready': {
      const [spent] = await deps.db
        .update(oauthFlows)
        .set({ status: 'consumed', consumedAt: now, claimHash: null })
        .where(and(eq(oauthFlows.id, flow.id), eq(oauthFlows.status, 'login_ready')))
        .returning({ accountId: oauthFlows.resultAccountId });
      if (!spent) throw new ApiError(410, 'flow_used', 'This login flow was already completed.');
      if (!spent.accountId) throw new Error('login_ready flow without an account');
      return { status: 'complete', ...(await startSession(deps, spent.accountId, meta)) };
    }
    case 'link_ready':
      return finishLink(deps, flow, now);
    default:
      throw new ApiError(410, 'flow_used', 'This login flow was already completed.');
  }
}

export type WebFinishResult = { status: 'linked' } | { status: 'failed'; code: string };

/**
 * The account page finishes a web link flow: the poll secret (kept by the tab that started it), the
 * claim (handed to the browser that approved it) and a session of the account that started it. Any
 * mismatch is 404 `flow_not_found`, the same answer as for an unknown flow: the victim of a phished
 * authorize URL holds the claim but neither the attacker's poll secret nor the attacker's session,
 * and the attacker holds the poll secret and the session but not the claim.
 */
export async function finishWebLink(deps: Deps, accountId: string, input: z.infer<typeof discordWebFinishSchema>): Promise<WebFinishResult> {
  const flow = await loadFlow(deps, input.flowId, input.pollSecret);
  if (flow.handoff !== 'web' || flow.intent !== 'link' || flow.accountId !== accountId || !claimMatches(flow, input.claim)) {
    throw new ApiError(404, 'flow_not_found', 'No such login flow.');
  }
  switch (flow.status) {
    case 'failed':
      return { status: 'failed', code: flow.errorCode ?? 'discord_error' };
    case 'link_ready': {
      const result = await finishLink(deps, flow, deps.clock());
      if (result.status === 'linked' || result.status === 'failed') return result;
      throw new Error(`finishLink answered ${result.status}`);
    }
    default:
      // A claim exists only once the callback ran, so pending cannot carry one; anything else is spent.
      throw new ApiError(410, 'flow_used', 'This login flow was already completed.');
  }
}

/**
 * Attach the approved Discord identity to the account that started the link flow. Runs under the
 * account row lock (the same one a password reset takes) and refuses (`password_changed`) when the
 * password changed after the flow started: the re-authentication that started it no longer holds.
 * The flow is spent in the same transaction; a refusal is reported as `failed` with a stable code
 * and also spends it. The owner is emailed after the commit.
 */
async function finishLink(deps: Deps, flow: typeof oauthFlows.$inferSelect, now: Date): Promise<PollResult> {
  const profile = flow.profile as StoredProfile | null;
  const accountId = flow.accountId;
  if (!profile || !accountId) throw new Error('link_ready flow without a profile or account');

  type Outcome = { result: PollResult; linked: { email: string; username: string } | null };
  let outcome: Outcome;
  try {
    outcome = await deps.db.transaction(async (tx): Promise<Outcome> => {
      const [spent] = await tx
        .update(oauthFlows)
        .set({ status: 'consumed', consumedAt: now, claimHash: null, profile: null })
        .where(and(eq(oauthFlows.id, flow.id), eq(oauthFlows.status, 'link_ready')))
        .returning({ id: oauthFlows.id });
      if (!spent) throw new ApiError(410, 'flow_used', 'This login flow was already completed.');
      // A refusal is not a link: the flow reads "failed" (with the code) to the claimed launcher until it expires.
      const refused = async (code: string): Promise<Outcome> => {
        await tx.update(oauthFlows).set({ status: 'failed', errorCode: code, consumedAt: null, claimHash: flow.claimHash }).where(eq(oauthFlows.id, flow.id));
        return { result: { status: 'failed', code }, linked: null };
      };

      const [account] = await tx.select().from(accounts).where(eq(accounts.id, accountId)).for('update');
      if (!account) return refused('invalid_request');
      // The flow was started with the password of that moment. A change since then (the owner's
      // reset of a squatted address, or a change after a stolen session) voids it.
      if (account.passwordChangedAt !== null && account.passwordChangedAt.getTime() > flow.createdAt.getTime()) return refused('password_changed');

      const [identity] = await tx
        .select()
        .from(oauthIdentities)
        .where(and(eq(oauthIdentities.provider, 'discord'), eq(oauthIdentities.providerUserId, profile.id)))
        .limit(1);
      if (identity) return identity.accountId === accountId ? { result: { status: 'linked' }, linked: null } : refused('discord_already_linked');
      const [mine] = await tx
        .select({ id: oauthIdentities.id })
        .from(oauthIdentities)
        .where(and(eq(oauthIdentities.accountId, accountId), eq(oauthIdentities.provider, 'discord')))
        .limit(1);
      if (mine) return refused('already_has_discord');

      await tx.insert(oauthIdentities).values({
        accountId,
        provider: 'discord',
        providerUserId: profile.id,
        providerUsername: profile.username,
        providerEmail: profile.providerEmail ?? profile.email,
        emailVerified: profile.emailVerified ?? profile.email !== null,
        createdAt: now,
      });
      return { result: { status: 'linked' }, linked: account.email === null ? null : { email: account.email, username: account.username } };
    });
  } catch (err) {
    if (!isUniqueViolation(err)) throw err;
    // Somebody linked this Discord account between the check and the insert; the transaction rolled back, so spend the flow here.
    await deps.db
      .update(oauthFlows)
      .set({ status: 'failed', errorCode: 'discord_already_linked', profile: null })
      .where(and(eq(oauthFlows.id, flow.id), eq(oauthFlows.status, 'link_ready')));
    return { status: 'failed', code: 'discord_already_linked' };
  }

  if (outcome.linked) {
    const { email, username } = outcome.linked;
    await sendSafely(deps, () => deps.email.send(discordLinkedMessage(email, username, profile.username)), 'discord_linked');
  }
  return outcome.result;
}

/** A new Discord player picks their in-game name; creates the account, the identity and a session. Needs the claim. */
export async function completeDiscordSignup(
  deps: Deps,
  input: z.infer<typeof discordCompleteSchema>,
  meta: SessionMeta,
): Promise<LoginResult> {
  const flow = await loadFlow(deps, input.flowId, input.pollSecret);
  if (!claimMatches(flow, input.claim)) throw new ApiError(404, 'flow_not_found', 'No such login flow.');
  if (flow.status !== 'needs_username') throw new ApiError(409, 'flow_state', 'This login flow is not waiting for a username.');
  if (isReservedUsername(input.username)) throw new ApiError(409, 'username_taken', 'That username is not available.');
  const profile = flow.profile as StoredProfile | null;
  if (!profile) throw new Error('needs_username flow without a profile');
  const now = deps.clock();

  try {
    const accountId = await deps.db.transaction(async (tx) => {
      const [spent] = await tx
        .update(oauthFlows)
        .set({ status: 'consumed', consumedAt: now, profile: null, claimHash: null })
        .where(and(eq(oauthFlows.id, flow.id), eq(oauthFlows.status, 'needs_username')))
        .returning({ id: oauthFlows.id });
      if (!spent) throw new ApiError(410, 'flow_used', 'This login flow was already completed.');
      const account = await createAccountRow(
        tx,
        { email: profile.email, emailVerified: profile.email !== null, username: input.username, passwordHash: null },
        now,
      );
      await tx.insert(oauthIdentities).values({
        accountId: account.id,
        provider: 'discord',
        providerUserId: profile.id,
        providerUsername: profile.username,
        providerEmail: profile.email,
        emailVerified: profile.email !== null,
        createdAt: now,
      });
      return account.id;
    });
    return await startSession(deps, accountId, meta);
  } catch (err) {
    if (isUniqueViolation(err, 'accounts_username_lower_uq')) throw new ApiError(409, 'username_taken', 'That username is not available.');
    if (isUniqueViolation(err, 'accounts_email_lower_uq')) throw new ApiError(409, 'email_in_use', 'An account with your Discord email already exists. Log in with your password and link Discord instead.');
    if (isUniqueViolation(err)) throw new ApiError(409, 'discord_already_linked', 'This Discord account is already linked to a GAGE account.');
    throw err;
  }
}

/**
 * Remove the Discord link. Refused when it is the account's only way to log in (no password: 409
 * `last_login_method`, checked first), then re-authenticated with the current password (a
 * login-method change; 401 `invalid_credentials`, 429 `rate_limited`). The owner is emailed.
 */
export async function unlinkDiscord(deps: Deps, accountId: string, input: z.infer<typeof discordUnlinkSchema>, ctx: ReqCtx): Promise<void> {
  const account = await findAccountById(deps.db, accountId);
  if (!account) throw new ApiError(404, 'account_not_found', 'No such account.');
  if (account.passwordHash === null) {
    throw new ApiError(409, 'last_login_method', 'Set a password before unlinking Discord: it is your only way to log in.');
  }
  await verifyCurrentPassword(deps, { id: account.id, passwordHash: account.passwordHash }, input.password, ctx, deps.clock());
  const removed = await deps.db
    .delete(oauthIdentities)
    .where(and(eq(oauthIdentities.accountId, accountId), eq(oauthIdentities.provider, 'discord')))
    .returning({ id: oauthIdentities.id, providerUsername: oauthIdentities.providerUsername });
  if (removed.length === 0) throw new ApiError(404, 'not_linked', 'No Discord account is linked.');
  const email = account.email;
  if (email !== null) {
    for (const identity of removed) {
      await sendSafely(deps, () => deps.email.send(discordUnlinkedMessage(email, account.username, identity.providerUsername)), 'discord_unlinked');
    }
  }
}

/** A free username derived from a Discord name: invalid characters become "_", then a numeric suffix until unused. */
async function suggestUsername(deps: Deps, discordName: string): Promise<string> {
  let base = discordName.replace(/[^A-Za-z0-9_]/g, '_').replace(/_+/g, '_').slice(0, 16);
  if (base.length < 3) base = `${base}player`.slice(0, 16);
  for (let i = 0; i < 20; i++) {
    const candidate = i === 0 ? base : `${base}${Math.floor(Math.random() * 9000 + 1000)}`;
    if (!isReservedUsername(candidate) && !(await findAccountByUsername(deps.db, candidate))) return candidate;
  }
  return `${base.slice(0, 10)}${Date.now() % 100000}`;
}

/** Remove finished and abandoned flows (they hold a PKCE verifier and, briefly, a Discord profile). */
export async function purgeOauthFlows(deps: Deps, now: Date): Promise<number> {
  const cutoff = new Date(now.getTime() - 86_400_000);
  const rows = await deps.db.delete(oauthFlows).where(lt(oauthFlows.expiresAt, cutoff)).returning({ id: oauthFlows.id });
  return rows.length;
}
