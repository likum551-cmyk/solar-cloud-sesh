// Quests: small tasks that pay coins into the wallet (reason `quest`).
//
//   discord_link   verifiable: pays when the account has a linked Discord identity
//   discord_guild  "join the Solar RP Discord server": NOT AVAILABLE YET. Verifying it needs a fresh
//                  Discord authorization with the guilds.members.read scope at claim time (the
//                  platform keeps no Discord tokens) and the launcher cannot finish a Discord flow
//                  yet. It lists as locked/not_available and every claim is refused.
//   visit          honor system (no API can prove a follow or a like): the player starts the quest
//                  (the URL opens) and may claim QUEST_VISIT_MIN_SECONDS later, and only with a
//                  linked Discord identity
//
// Rules that keep a quest from paying twice live in the database (drizzle/0003_quests.sql): the
// ledger entry's idempotency key is quest:<quest id>:<account id>, quest_progress has one row per
// (account, quest) and one claim per (quest, Discord user id), and the credit and the claim row are
// written in one transaction. The checks below only give a clear refusal before that.
import { and, asc, eq, isNotNull, isNull } from 'drizzle-orm';
import { z } from 'zod';
import { assertNotBanned } from '../auth/accounts.js';
import type { Db } from '../db/index.js';
import { oauthIdentities, questProgress, quests, walletLedger, type QuestKind } from '../db/schema.js';
import type { Deps } from '../deps.js';
import { getBalance } from '../shop/service.js';
import { ApiError, isUniqueViolation } from '../util/errors.js';

/** A visit quest can be claimed this long after it was started (GI's rule, 2026-11-06). */
export const QUEST_VISIT_MIN_SECONDS = 30;

export const QUEST_SLUG_PATTERN = /^[a-z0-9][a-z0-9_]{1,47}$/;
export const questSlugSchema = z.string().regex(QUEST_SLUG_PATTERN);

export type QuestRow = typeof quests.$inferSelect;
type ProgressRow = typeof questProgress.$inferSelect;

export type QuestStatus = 'available' | 'started' | 'claimable' | 'claimed' | 'locked';
/** Why a quest cannot be claimed now (status locked). */
export type QuestLockReason = 'discord_required' | 'discord_already_claimed' | 'not_available';

export interface QuestView {
  slug: string;
  kind: QuestKind;
  title: string;
  description: string;
  /** The page a visit quest opens; null for the other kinds. */
  url: string | null;
  reward: { currency: string; amount: number };
  sort: number;
  status: QuestStatus;
  lockReason: QuestLockReason | null;
  startedAt: Date | null;
  /** Visit quests: when the claim opens (startedAt + QUEST_VISIT_MIN_SECONDS). */
  claimableAt: Date | null;
  /** Visit quests: whole seconds until claimableAt, 0 once it passed; null when not started. */
  waitSeconds: number | null;
  claimedAt: Date | null;
}

export interface QuestList {
  discordLinked: boolean;
  /** Seconds a visit quest must stay started before its claim. */
  visitMinSeconds: number;
  quests: QuestView[];
}

function assertKind(kind: string): QuestKind {
  if (kind === 'discord_link' || kind === 'discord_guild' || kind === 'visit') return kind;
  throw new Error(`unknown quest kind in the database: ${kind}`);
}

/** Whole seconds left until `at` (0 when it passed). */
function secondsUntil(at: Date, now: Date): number {
  return Math.max(0, Math.ceil((at.getTime() - now.getTime()) / 1000));
}

interface ViewInput {
  quest: QuestRow;
  progress: ProgressRow | undefined;
  discordUserId: string | null;
  /** True when this account's Discord already claimed the quest on another account. */
  discordUsedElsewhere: boolean;
  now: Date;
}

/** The status rules, in one place: the list shows exactly what a claim would decide. */
export function questView({ quest, progress, discordUserId, discordUsedElsewhere, now }: ViewInput): QuestView {
  const kind = assertKind(quest.kind);
  const startedAt = progress?.startedAt ?? null;
  const claimableAt = kind === 'visit' && startedAt ? new Date(startedAt.getTime() + QUEST_VISIT_MIN_SECONDS * 1000) : null;
  const base = {
    slug: quest.slug,
    kind,
    title: quest.title,
    description: quest.description,
    url: kind === 'visit' ? quest.url : null,
    reward: { currency: quest.currency, amount: quest.rewardAmount },
    sort: quest.sort,
    startedAt,
    claimableAt,
    waitSeconds: claimableAt ? secondsUntil(claimableAt, now) : null,
    claimedAt: progress?.claimedAt ?? null,
  };
  const as = (status: QuestStatus, lockReason: QuestLockReason | null = null): QuestView => ({ ...base, status, lockReason });

  if (progress?.claimedAt) return as('claimed');
  if (kind === 'discord_guild') return as('locked', 'not_available');
  if (kind === 'visit' && !startedAt) return as('available');
  if (discordUserId === null) return as('locked', 'discord_required');
  if (discordUsedElsewhere) return as('locked', 'discord_already_claimed');
  if (kind === 'visit' && claimableAt && claimableAt.getTime() > now.getTime()) return as('started');
  return as('claimable');
}

async function discordIdOf(db: Db, accountId: string): Promise<string | null> {
  const [row] = await db
    .select({ id: oauthIdentities.providerUserId })
    .from(oauthIdentities)
    .where(and(eq(oauthIdentities.accountId, accountId), eq(oauthIdentities.provider, 'discord')))
    .limit(1);
  return row?.id ?? null;
}

/** Quest ids this Discord user already claimed on any account. */
async function questsClaimedByDiscord(db: Db, discordUserId: string | null): Promise<Set<string>> {
  if (discordUserId === null) return new Set();
  const rows = await db
    .select({ questId: questProgress.questId })
    .from(questProgress)
    .where(and(eq(questProgress.discordUserId, discordUserId), isNotNull(questProgress.claimedAt)));
  return new Set(rows.map((r) => r.questId));
}

/** The active quests with this account's status, in display order. */
export async function listQuests(deps: Deps, accountId: string): Promise<QuestList> {
  const { db } = deps;
  const now = deps.clock();
  const rows = await db.select().from(quests).where(eq(quests.active, true)).orderBy(asc(quests.sort), asc(quests.slug));
  const progress = await db.select().from(questProgress).where(eq(questProgress.accountId, accountId));
  const byQuest = new Map(progress.map((p) => [p.questId, p]));
  const discordUserId = await discordIdOf(db, accountId);
  const usedByDiscord = await questsClaimedByDiscord(db, discordUserId);
  return {
    discordLinked: discordUserId !== null,
    visitMinSeconds: QUEST_VISIT_MIN_SECONDS,
    quests: rows.map((quest) => {
      const p = byQuest.get(quest.id);
      return questView({ quest, progress: p, discordUserId, discordUsedElsewhere: !p?.claimedAt && usedByDiscord.has(quest.id), now });
    }),
  };
}

async function findQuest(db: Db, slug: string): Promise<QuestRow> {
  const [quest] = await db.select().from(quests).where(eq(quests.slug, slug)).limit(1);
  if (!quest) throw new ApiError(404, 'quest_not_found', 'No such quest.');
  if (!quest.active) throw new ApiError(409, 'quest_inactive', 'This quest is not running.');
  return quest;
}

/**
 * Start a visit quest: records when (once; a repeated start keeps the first time, so it never
 * restarts or shortens the wait) and returns the quest with its URL for the client to open.
 */
export async function startQuest(deps: Deps, accountId: string, slug: string): Promise<QuestView> {
  const { db } = deps;
  const quest = await findQuest(db, slug);
  if (quest.kind !== 'visit') throw new ApiError(409, 'not_a_visit_quest', 'Only a visit quest is started; this one is claimed directly.');
  const now = deps.clock();
  // The first start wins: a row that already has a start keeps it (a claimed row is left alone too).
  await db
    .insert(questProgress)
    .values({ accountId, questId: quest.id, startedAt: now, createdAt: now })
    .onConflictDoUpdate({
      target: [questProgress.accountId, questProgress.questId],
      set: { startedAt: now },
      setWhere: and(isNull(questProgress.startedAt), isNull(questProgress.claimedAt)),
    });
  const [progress] = await db.select().from(questProgress).where(and(eq(questProgress.accountId, accountId), eq(questProgress.questId, quest.id)));
  const discordUserId = await discordIdOf(db, accountId);
  const used = await questsClaimedByDiscord(db, discordUserId);
  return questView({ quest, progress, discordUserId, discordUsedElsewhere: !progress?.claimedAt && used.has(quest.id), now });
}

/**
 * Write the credit and the claim, in one transaction. Callers have checked the rules; this function
 * relies only on the database: a second credit for the same (account, quest) or the same (quest,
 * Discord user) fails with a unique violation and rolls the whole claim back. Exported for the
 * test that proves exactly that.
 */
export async function creditQuestClaim(db: Db, input: { accountId: string; quest: QuestRow; discordUserId: string; now: Date }): Promise<void> {
  const { accountId, quest, discordUserId, now } = input;
  await db.transaction(async (tx) => {
    const [entry] = await tx
      .insert(walletLedger)
      .values({
        accountId,
        currency: quest.currency,
        delta: quest.rewardAmount,
        reason: 'quest',
        idempotencyKey: `quest:${quest.id}:${accountId}`,
        memo: quest.title,
        createdAt: now,
      })
      .returning({ id: walletLedger.id });
    if (!entry) throw new Error('ledger insert returned no row');
    const claimed = await tx
      .insert(questProgress)
      .values({ accountId, questId: quest.id, claimedAt: now, discordUserId, ledgerId: entry.id, createdAt: now })
      .onConflictDoUpdate({
        target: [questProgress.accountId, questProgress.questId],
        set: { claimedAt: now, discordUserId, ledgerId: entry.id },
        setWhere: isNull(questProgress.claimedAt),
      })
      .returning({ questId: questProgress.questId });
    if (claimed.length === 0) throw new ApiError(409, 'already_claimed', 'You already claimed this quest.');
  });
}

export interface ClaimResult {
  quest: QuestView;
  credited: { currency: string; amount: number };
  balance: number;
}

/** Claim a quest's reward. Every refusal has its own code; see the route's error list. */
export async function claimQuest(deps: Deps, accountId: string, slug: string): Promise<ClaimResult> {
  const { db } = deps;
  const now = deps.clock();
  const quest = await findQuest(db, slug);
  const kind = assertKind(quest.kind);
  await assertNotBanned(db, accountId, now);

  const [progress] = await db.select().from(questProgress).where(and(eq(questProgress.accountId, accountId), eq(questProgress.questId, quest.id)));
  if (progress?.claimedAt) throw new ApiError(409, 'already_claimed', 'You already claimed this quest.');
  if (kind === 'discord_guild') {
    throw new ApiError(409, 'quest_unavailable', 'This quest cannot be completed yet: joining the Discord server cannot be checked yet.');
  }
  if (kind === 'visit' && !progress?.startedAt) throw new ApiError(409, 'quest_not_started', 'Start the quest first.');

  const discordUserId = await discordIdOf(db, accountId);
  if (discordUserId === null) throw new ApiError(403, 'discord_required', 'Link your Discord account first.');
  if ((await questsClaimedByDiscord(db, discordUserId)).has(quest.id)) {
    throw new ApiError(409, 'discord_already_claimed', 'Your Discord account already claimed this quest on another Solar RP account.');
  }
  if (kind === 'visit' && progress?.startedAt) {
    const opensAt = new Date(progress.startedAt.getTime() + QUEST_VISIT_MIN_SECONDS * 1000);
    const wait = secondsUntil(opensAt, now);
    if (opensAt.getTime() > now.getTime()) {
      throw new ApiError(409, 'quest_wait', `You can claim this in ${wait} s.`, { retryAfterSeconds: wait, details: { waitSeconds: wait, claimableAt: opensAt.toISOString() } });
    }
  }

  try {
    await creditQuestClaim(db, { accountId, quest, discordUserId, now });
  } catch (err) {
    if (isUniqueViolation(err, 'wallet_ledger_idem_uq')) throw new ApiError(409, 'already_claimed', 'You already claimed this quest.');
    if (isUniqueViolation(err, 'quest_progress_discord_uq')) {
      throw new ApiError(409, 'discord_already_claimed', 'Your Discord account already claimed this quest on another Solar RP account.');
    }
    throw err;
  }

  const [after] = await db.select().from(questProgress).where(and(eq(questProgress.accountId, accountId), eq(questProgress.questId, quest.id)));
  return {
    quest: questView({ quest, progress: after, discordUserId, discordUsedElsewhere: false, now }),
    credited: { currency: quest.currency, amount: quest.rewardAmount },
    balance: await getBalance(db, accountId, quest.currency),
  };
}
