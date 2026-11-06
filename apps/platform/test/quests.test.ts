import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { and, eq, sql } from 'drizzle-orm';
import { schema } from '../src/db/index.js';
import { adminActivateQuest, adminAddQuest, adminDeactivateQuest, adminListQuests, adminSetQuestReward, adminSetQuestUrl } from '../src/quests/admin.js';
import { creditQuestClaim, QUEST_VISIT_MIN_SECONDS } from '../src/quests/service.js';
import { isCheckViolation, pgErrorInfo } from '../src/util/errors.js';
import { createTestEnv, makeUser, type TestEnv, type TestUser } from './helpers.js';

// Quests pay coins into the wallet. Each rule is attacked from the HTTP side (what a player can do)
// and, where it is a database invariant, from below the application (what a race or a bug could do).

let env: TestEnv;
beforeAll(async () => {
  env = await createTestEnv();
});
afterAll(async () => {
  await env.close();
});

const CLI = { accountId: null, label: 'cli:test' };
let discordCounter = 0;

/** Attach a Discord identity the way a finished link flow does (test/discord.test.ts covers the flow itself). */
async function linkDiscord(user: TestUser, discordId?: string): Promise<string> {
  discordCounter++;
  const id = discordId ?? `90000000000000${String(discordCounter).padStart(4, '0')}`;
  await env.handle.db.insert(schema.oauthIdentities).values({ accountId: user.accountId, provider: 'discord', providerUserId: id, providerUsername: `discord_${discordCounter}` });
  return id;
}

async function unlinkDiscord(user: TestUser): Promise<void> {
  await env.handle.db.delete(schema.oauthIdentities).where(eq(schema.oauthIdentities.accountId, user.accountId));
}

interface QuestBody {
  slug: string;
  kind: string;
  status: string;
  lockReason: string | null;
  url: string | null;
  reward: { currency: string; amount: number };
  waitSeconds: number | null;
  startedAt: string | null;
  claimedAt: string | null;
}
interface ListBody { discordLinked: boolean; quests: QuestBody[] }
interface ErrorBody { error: { code: string; details?: Record<string, unknown> } }

const list = (u: TestUser) => env.call<ListBody>('/v1/me/quests', { token: u.token });
const start = (u: TestUser, slug: string) => env.call<{ quest: QuestBody } & ErrorBody>(`/v1/me/quests/${slug}/start`, { token: u.token, method: 'POST' });
const claim = (u: TestUser, slug: string) =>
  env.call<{ quest: QuestBody; credited: { currency: string; amount: number }; balance: number } & ErrorBody>(`/v1/me/quests/${slug}/claim`, { token: u.token, method: 'POST' });

async function coins(u: TestUser): Promise<number> {
  const w = await env.call<{ balances: { currency: string; balance: number }[] }>('/v1/me/wallet', { token: u.token });
  return w.body.balances.find((b) => b.currency === 'coins')?.balance ?? 0;
}

async function ledgerSum(u: TestUser): Promise<number> {
  const [row] = await env.handle.db
    .select({ s: sql<string>`coalesce(sum(${schema.walletLedger.delta}), 0)` })
    .from(schema.walletLedger)
    .where(and(eq(schema.walletLedger.accountId, u.accountId), eq(schema.walletLedger.currency, 'coins')));
  return Number(row?.s ?? 0);
}

let visitCounter = 0;
/** A fresh active visit quest (its own slug, so tests never share a claim). */
async function makeVisitQuest(reward = 100): Promise<string> {
  visitCounter++;
  const slug = `test_visit_${visitCounter}`;
  await adminAddQuest(env.deps, CLI, { slug, kind: 'visit', title: `Visit ${visitCounter}`, description: 'test', url: `https://example.test/channel/${visitCounter}`, reward, sort: 100 + visitCounter });
  await adminActivateQuest(env.deps, CLI, slug);
  return slug;
}

describe('seed', () => {
  it('ships Discord linking on at 500 coins and every social quest off with no URL', async () => {
    const all = await adminListQuests(env.deps);
    const bySlug = new Map(all.map((q) => [q.slug, q]));
    expect(bySlug.get('link_discord')).toMatchObject({ kind: 'discord_link', rewardAmount: 500, currency: 'coins', active: true, url: null });
    for (const slug of ['youtube_subscribe', 'youtube_like', 'facebook_follow', 'instagram_follow', 'tiktok_follow', 'x_follow']) {
      expect(bySlug.get(slug), slug).toMatchObject({ kind: 'visit', rewardAmount: 100, active: false, url: null });
    }
    expect(bySlug.get('join_discord_server')).toMatchObject({ kind: 'discord_guild', active: false });
  });

  it('lists only active quests to a player', async () => {
    const u = await makeUser(env);
    const res = await list(u);
    expect(res.status).toBe(200);
    expect(res.body.quests.map((q) => q.slug)).toContain('link_discord');
    expect(res.body.quests.map((q) => q.slug)).not.toContain('youtube_subscribe');
  });
});

describe('the Discord link quest', () => {
  it('is locked without a Discord identity and refuses a claim with discord_required', async () => {
    const u = await makeUser(env);
    const res = await list(u);
    expect(res.body.discordLinked).toBe(false);
    expect(res.body.quests.find((q) => q.slug === 'link_discord')).toMatchObject({ status: 'locked', lockReason: 'discord_required' });
    const c = await claim(u, 'link_discord');
    expect(c.status).toBe(403);
    expect(c.body.error.code).toBe('discord_required');
    expect(await coins(u)).toBe(0);
  });

  it('pays 500 once with an identity; a second claim is refused already_claimed', async () => {
    const u = await makeUser(env);
    await linkDiscord(u);
    expect((await list(u)).body.quests.find((q) => q.slug === 'link_discord')).toMatchObject({ status: 'claimable', lockReason: null });
    const first = await claim(u, 'link_discord');
    expect(first.status, JSON.stringify(first.body)).toBe(200);
    expect(first.body.credited).toEqual({ currency: 'coins', amount: 500 });
    expect(first.body.balance).toBe(500);
    expect(first.body.quest).toMatchObject({ status: 'claimed' });
    const second = await claim(u, 'link_discord');
    expect(second.status).toBe(409);
    expect(second.body.error.code).toBe('already_claimed');
    expect(await coins(u)).toBe(500);
    expect(await ledgerSum(u)).toBe(500);
    const ledger = await env.call<{ entries: { reason: string; delta: number }[] }>('/v1/me/wallet/ledger', { token: u.token });
    expect(ledger.body.entries).toEqual([expect.objectContaining({ reason: 'quest', delta: 500 })]);
  });

  it('concurrent claims pay once', async () => {
    const u = await makeUser(env);
    await linkDiscord(u);
    const results = await Promise.all(Array.from({ length: 6 }, () => claim(u, 'link_discord')));
    const statuses = results.map((r) => r.status).sort();
    expect(statuses).toEqual([200, 409, 409, 409, 409, 409]);
    for (const r of results.filter((x) => x.status === 409)) expect(r.body.error.code).toBe('already_claimed');
    expect(await coins(u)).toBe(500);
    expect(await ledgerSum(u)).toBe(500);
  });

  it('one Discord account claims once: unlinking and linking it to another account pays nothing', async () => {
    const a = await makeUser(env);
    const discordId = await linkDiscord(a);
    expect((await claim(a, 'link_discord')).status).toBe(200);
    await unlinkDiscord(a);
    const b = await makeUser(env);
    await linkDiscord(b, discordId);
    const res = await claim(b, 'link_discord');
    expect(res.status).toBe(409);
    expect(res.body.error.code).toBe('discord_already_claimed');
    expect(await coins(b)).toBe(0);
  });
});

describe('visit quests (honor system, Discord-gated)', () => {
  it('start records the time, returns the url and is idempotent', async () => {
    const slug = await makeVisitQuest();
    const u = await makeUser(env);
    expect((await list(u)).body.quests.find((q) => q.slug === slug)).toMatchObject({ status: 'available', url: `https://example.test/channel/${visitCounter}` });
    const s1 = await start(u, slug);
    expect(s1.status, JSON.stringify(s1.body)).toBe(200);
    expect(s1.body.quest.url).toMatch(/^https:\/\/example\.test\/channel\//);
    expect(s1.body.quest.startedAt).not.toBeNull();
    env.clock.advanceSeconds(10);
    const s2 = await start(u, slug);
    expect(s2.body.quest.startedAt).toBe(s1.body.quest.startedAt); // a second start does not restart the wait
  });

  it('a claim before the quest was started is refused quest_not_started', async () => {
    const slug = await makeVisitQuest();
    const u = await makeUser(env);
    await linkDiscord(u);
    const res = await claim(u, slug);
    expect(res.status).toBe(409);
    expect(res.body.error.code).toBe('quest_not_started');
  });

  it(`a claim before ${QUEST_VISIT_MIN_SECONDS} s is refused quest_wait with the seconds left; at ${QUEST_VISIT_MIN_SECONDS} s it pays`, async () => {
    const slug = await makeVisitQuest(100);
    const u = await makeUser(env);
    await linkDiscord(u);
    await start(u, slug);
    env.clock.advanceSeconds(QUEST_VISIT_MIN_SECONDS - 1);
    const listed = (await list(u)).body.quests.find((q) => q.slug === slug);
    expect(listed).toMatchObject({ status: 'started', waitSeconds: 1 });
    const early = await claim(u, slug);
    expect(early.status).toBe(409);
    expect(early.body.error.code).toBe('quest_wait');
    expect(early.body.error.details).toMatchObject({ waitSeconds: 1 });
    expect(early.headers.get('retry-after')).toBe('1');
    expect(await coins(u)).toBe(0);
    env.clock.advanceSeconds(1);
    expect((await list(u)).body.quests.find((q) => q.slug === slug)).toMatchObject({ status: 'claimable' });
    const ok = await claim(u, slug);
    expect(ok.status, JSON.stringify(ok.body)).toBe(200);
    expect(ok.body.credited.amount).toBe(100);
    expect(await coins(u)).toBe(100);
  });

  it('a claim without a linked Discord is refused discord_required, even after the wait', async () => {
    const slug = await makeVisitQuest();
    const u = await makeUser(env);
    await start(u, slug);
    env.clock.advanceSeconds(QUEST_VISIT_MIN_SECONDS + 5);
    expect((await list(u)).body.quests.find((q) => q.slug === slug)).toMatchObject({ status: 'locked', lockReason: 'discord_required' });
    const res = await claim(u, slug);
    expect(res.status).toBe(403);
    expect(res.body.error.code).toBe('discord_required');
    expect(await coins(u)).toBe(0);
  });

  it('a double claim (sequential and concurrent) pays once', async () => {
    const slug = await makeVisitQuest(100);
    const u = await makeUser(env);
    await linkDiscord(u);
    await start(u, slug);
    env.clock.advanceSeconds(QUEST_VISIT_MIN_SECONDS);
    const burst = await Promise.all(Array.from({ length: 5 }, () => claim(u, slug)));
    expect(burst.filter((r) => r.status === 200)).toHaveLength(1);
    expect(burst.filter((r) => r.status === 409 && r.body.error.code === 'already_claimed')).toHaveLength(4);
    const again = await claim(u, slug);
    expect(again.body.error.code).toBe('already_claimed');
    expect(await coins(u)).toBe(100);
    expect(await ledgerSum(u)).toBe(100);
  });

  it('an inactive quest cannot be started or claimed', async () => {
    const slug = await makeVisitQuest();
    const u = await makeUser(env);
    await linkDiscord(u);
    await start(u, slug);
    env.clock.advanceSeconds(QUEST_VISIT_MIN_SECONDS);
    await adminDeactivateQuest(env.deps, CLI, slug);
    const s = await start(u, slug);
    expect(s.status).toBe(409);
    expect(s.body.error.code).toBe('quest_inactive');
    const c = await claim(u, slug);
    expect(c.status).toBe(409);
    expect(c.body.error.code).toBe('quest_inactive');
    // a seeded social quest (off, no URL) is refused the same way
    expect((await start(u, 'youtube_subscribe')).body.error.code).toBe('quest_inactive');
    expect((await claim(u, 'youtube_subscribe')).body.error.code).toBe('quest_inactive');
    expect(await coins(u)).toBe(0);
  });

  it('unknown slugs are 404 quest_not_found; malformed ones 400', async () => {
    const u = await makeUser(env);
    expect((await start(u, 'no_such_quest')).body.error.code).toBe('quest_not_found');
    expect((await claim(u, 'no_such_quest')).body.error.code).toBe('quest_not_found');
    expect((await claim(u, 'BAD-SLUG')).status).toBe(400);
  });

  it('start on a quest that is not a visit quest is refused not_a_visit_quest', async () => {
    const u = await makeUser(env);
    const res = await start(u, 'link_discord');
    expect(res.status).toBe(409);
    expect(res.body.error.code).toBe('not_a_visit_quest');
  });

  it('needs a session', async () => {
    expect((await env.call('/v1/me/quests')).status).toBe(401);
    expect((await env.call('/v1/me/quests/link_discord/claim', { method: 'POST' })).status).toBe(401);
  });

  it('a banned account cannot claim', async () => {
    const slug = await makeVisitQuest();
    const u = await makeUser(env);
    await linkDiscord(u);
    await start(u, slug);
    env.clock.advanceSeconds(QUEST_VISIT_MIN_SECONDS);
    await env.handle.db.insert(schema.bans).values({ accountId: u.accountId, reason: 'test ban' });
    const res = await claim(u, slug);
    expect(res.status).toBe(403);
    expect(res.body.error.code).toBe('account_banned');
    expect(await coins(u)).toBe(0);
  });
});

describe('the Discord server quest', () => {
  it('cannot be switched on and is refused quest_inactive while off', async () => {
    await expect(adminActivateQuest(env.deps, CLI, 'join_discord_server')).rejects.toMatchObject({ code: 'quest_unavailable' });
    const u = await makeUser(env);
    await linkDiscord(u);
    expect((await claim(u, 'join_discord_server')).body.error.code).toBe('quest_inactive');
  });

  it('even if a database edit switches it on, it lists as not available and refuses a claim', async () => {
    await env.handle.db.update(schema.quests).set({ active: true }).where(eq(schema.quests.slug, 'join_discord_server'));
    try {
      const u = await makeUser(env);
      await linkDiscord(u);
      expect((await list(u)).body.quests.find((q) => q.slug === 'join_discord_server')).toMatchObject({ status: 'locked', lockReason: 'not_available' });
      const res = await claim(u, 'join_discord_server');
      expect(res.status).toBe(409);
      expect(res.body.error.code).toBe('quest_unavailable');
      expect(await coins(u)).toBe(0);
    } finally {
      await env.handle.db.update(schema.quests).set({ active: false }).where(eq(schema.quests.slug, 'join_discord_server'));
    }
  });
});

describe('database invariants (below the application)', () => {
  it('the credit cannot run twice for one (account, quest), even when both callers passed every check', async () => {
    const slug = await makeVisitQuest(100);
    const u = await makeUser(env);
    const discordId = await linkDiscord(u);
    const [quest] = await env.handle.db.select().from(schema.quests).where(eq(schema.quests.slug, slug));
    if (!quest) throw new Error('quest missing');
    const now = env.clock.now();
    // Two callers that both "saw" an unclaimed quest: the second must fail in the database.
    await creditQuestClaim(env.handle.db, { accountId: u.accountId, quest, discordUserId: discordId, now });
    await expect(creditQuestClaim(env.handle.db, { accountId: u.accountId, quest, discordUserId: discordId, now })).rejects.toSatisfy((e) => pgErrorInfo(e)?.code === '23505');
    expect(await ledgerSum(u)).toBe(100);
    expect(await coins(u)).toBe(100);
  });

  it('the ledger takes a quest credit only with a positive delta', async () => {
    const u = await makeUser(env);
    await expect(
      env.handle.db.insert(schema.walletLedger).values({ accountId: u.accountId, currency: 'coins', delta: -5, reason: 'quest', idempotencyKey: `quest-neg-${u.accountId}` }),
    ).rejects.toSatisfy((e) => isCheckViolation(e, 'wallet_ledger_sign'));
  });

  it('a claimed progress row must name its ledger entry and Discord account', async () => {
    const slug = await makeVisitQuest();
    const u = await makeUser(env);
    const [quest] = await env.handle.db.select().from(schema.quests).where(eq(schema.quests.slug, slug));
    await expect(
      env.handle.db.insert(schema.questProgress).values({ accountId: u.accountId, questId: quest?.id ?? '', claimedAt: new Date() }),
    ).rejects.toSatisfy((e) => isCheckViolation(e, 'quest_progress_claim_complete'));
  });

  it('an active visit quest needs an https URL; only visit quests carry one', async () => {
    await expect(adminAddQuest(env.deps, CLI, { slug: 'bad_url_quest', kind: 'visit', title: 'x', description: '', url: 'http://example.test', reward: 100, sort: 0 })).rejects.toMatchObject({ code: 'invalid_url' });
    await expect(
      env.handle.db.insert(schema.quests).values({ slug: 'no_url_active', kind: 'visit', title: 'x', rewardAmount: 100, active: true }),
    ).rejects.toSatisfy((e) => isCheckViolation(e, 'quests_active_visit_has_url'));
    await expect(
      env.handle.db.insert(schema.quests).values({ slug: 'link_with_url', kind: 'discord_link', title: 'x', rewardAmount: 100, url: 'https://example.test' }),
    ).rejects.toSatisfy((e) => isCheckViolation(e, 'quests_url_only_visit'));
  });
});

describe('admin', () => {
  it('set-url, set-reward, activate: the new reward is what the next claim pays', async () => {
    await adminSetQuestUrl(env.deps, CLI, 'tiktok_follow', 'https://www.tiktok.example/@solar');
    await adminSetQuestReward(env.deps, CLI, 'tiktok_follow', 250);
    await adminActivateQuest(env.deps, CLI, 'tiktok_follow');
    try {
      const u = await makeUser(env);
      await linkDiscord(u);
      await start(u, 'tiktok_follow');
      env.clock.advanceSeconds(QUEST_VISIT_MIN_SECONDS);
      const res = await claim(u, 'tiktok_follow');
      expect(res.status, JSON.stringify(res.body)).toBe(200);
      expect(res.body.credited.amount).toBe(250);
      const audit = await env.handle.db.select().from(schema.auditLog).where(eq(schema.auditLog.targetId, 'tiktok_follow'));
      expect(audit.map((a) => a.action).sort()).toEqual(['quest.activate', 'quest.set_reward', 'quest.set_url']);
    } finally {
      await adminDeactivateQuest(env.deps, CLI, 'tiktok_follow');
    }
  });

  it('activating a visit quest without a URL is refused url_required', async () => {
    await expect(adminActivateQuest(env.deps, CLI, 'x_follow')).rejects.toMatchObject({ code: 'url_required' });
  });

  it('refuses a duplicate slug and a reward out of range', async () => {
    await expect(adminAddQuest(env.deps, CLI, { slug: 'link_discord', kind: 'discord_link', title: 'x', description: '', url: null, reward: 1, sort: 0 })).rejects.toMatchObject({ code: 'quest_exists' });
    await expect(adminSetQuestReward(env.deps, CLI, 'x_follow', 0)).rejects.toMatchObject({ code: 'invalid_reward' });
  });
});
