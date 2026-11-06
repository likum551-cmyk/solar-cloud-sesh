// Quest administration (used by scripts/quests.ts, `npm run admin:quests`). Every change is audited.
// There is no HTTP admin route for quests yet: the catalogue changes rarely and only from the shell.
import { asc, eq } from 'drizzle-orm';
import { z } from 'zod';
import { writeAudit } from '../admin/service.js';
import { QUEST_KINDS, quests, type QuestKind } from '../db/schema.js';
import type { Actor, Deps } from '../deps.js';
import { ApiError, isUniqueViolation } from '../util/errors.js';
import { QUEST_SLUG_PATTERN, type QuestRow } from './service.js';

export const QUEST_REWARD_MAX = 1_000_000;

const urlSchema = z
  .string()
  .max(2048)
  .refine((u) => {
    try {
      const parsed = new URL(u);
      return parsed.protocol === 'https:' && parsed.hostname.includes('.') && !/\s/.test(u);
    } catch {
      return false;
    }
  });

function checkSlug(slug: string): void {
  if (!QUEST_SLUG_PATTERN.test(slug)) throw new ApiError(400, 'invalid_slug', 'A slug is 2-48 characters of a-z 0-9 _ and starts with a letter or digit.');
}

function checkUrl(url: string): void {
  if (!urlSchema.safeParse(url).success) throw new ApiError(400, 'invalid_url', 'The URL must be an https:// address of at most 2048 characters.');
}

function checkReward(amount: number): void {
  if (!Number.isInteger(amount) || amount < 1 || amount > QUEST_REWARD_MAX) {
    throw new ApiError(400, 'invalid_reward', `The reward must be a whole number from 1 to ${QUEST_REWARD_MAX}.`);
  }
}

export async function adminListQuests(deps: Deps): Promise<QuestRow[]> {
  return deps.db.select().from(quests).orderBy(asc(quests.sort), asc(quests.slug));
}

async function requireQuest(deps: Deps, slug: string): Promise<QuestRow> {
  checkSlug(slug);
  const [quest] = await deps.db.select().from(quests).where(eq(quests.slug, slug)).limit(1);
  if (!quest) throw new ApiError(404, 'quest_not_found', `No quest "${slug}".`);
  return quest;
}

export interface AddQuestInput {
  slug: string;
  kind: QuestKind;
  title: string;
  description: string;
  url: string | null;
  reward: number;
  sort: number;
}

/** Add a quest. It starts INACTIVE: switch it on with adminActivateQuest once it is right. */
export async function adminAddQuest(deps: Deps, actor: Actor, input: AddQuestInput): Promise<QuestRow> {
  checkSlug(input.slug);
  if (!(QUEST_KINDS as readonly string[]).includes(input.kind)) throw new ApiError(400, 'invalid_kind', `kind is one of ${QUEST_KINDS.join(', ')}.`);
  const title = input.title.trim();
  if (title.length < 1 || title.length > 80) throw new ApiError(400, 'invalid_title', 'The title is 1 to 80 characters.');
  const description = input.description.trim();
  if (description.length > 500) throw new ApiError(400, 'invalid_description', 'The description is at most 500 characters.');
  if (input.url !== null) {
    if (input.kind !== 'visit') throw new ApiError(400, 'url_not_allowed', 'Only a visit quest has a URL.');
    checkUrl(input.url);
  }
  checkReward(input.reward);
  if (!Number.isInteger(input.sort)) throw new ApiError(400, 'invalid_sort', 'sort is a whole number.');
  const now = deps.clock();
  try {
    return await deps.db.transaction(async (tx) => {
      const [row] = await tx
        .insert(quests)
        .values({ slug: input.slug, kind: input.kind, title, description, url: input.url, rewardAmount: input.reward, active: false, sort: input.sort, createdAt: now, updatedAt: now })
        .returning();
      if (!row) throw new Error('quest insert returned no row');
      await writeAudit(tx, actor, { action: 'quest.add', targetType: 'quest', targetId: input.slug, details: { kind: input.kind, reward: input.reward, url: input.url } }, now);
      return row;
    });
  } catch (err) {
    if (isUniqueViolation(err, 'quests_slug_uq')) throw new ApiError(409, 'quest_exists', `A quest "${input.slug}" already exists.`);
    throw err;
  }
}

async function update(deps: Deps, actor: Actor, quest: QuestRow, patch: Partial<Pick<QuestRow, 'url' | 'rewardAmount' | 'active'>>, action: string, details: Record<string, unknown>): Promise<QuestRow> {
  const now = deps.clock();
  return deps.db.transaction(async (tx) => {
    const [row] = await tx.update(quests).set({ ...patch, updatedAt: now }).where(eq(quests.id, quest.id)).returning();
    if (!row) throw new ApiError(404, 'quest_not_found', `No quest "${quest.slug}".`);
    await writeAudit(tx, actor, { action, targetType: 'quest', targetId: quest.slug, details }, now);
    return row;
  });
}

export async function adminSetQuestUrl(deps: Deps, actor: Actor, slug: string, url: string): Promise<QuestRow> {
  const quest = await requireQuest(deps, slug);
  if (quest.kind !== 'visit') throw new ApiError(400, 'url_not_allowed', 'Only a visit quest has a URL.');
  checkUrl(url);
  return update(deps, actor, quest, { url }, 'quest.set_url', { before: quest.url, after: url });
}

/** Change the reward. Claims already paid keep what they paid; the next claim pays the new amount. */
export async function adminSetQuestReward(deps: Deps, actor: Actor, slug: string, amount: number): Promise<QuestRow> {
  checkReward(amount);
  const quest = await requireQuest(deps, slug);
  return update(deps, actor, quest, { rewardAmount: amount }, 'quest.set_reward', { before: quest.rewardAmount, after: amount });
}

export async function adminActivateQuest(deps: Deps, actor: Actor, slug: string): Promise<QuestRow> {
  const quest = await requireQuest(deps, slug);
  if (quest.kind === 'discord_guild') {
    throw new ApiError(409, 'quest_unavailable', 'The Discord server quest cannot be switched on yet: its membership check is not built (see apps/platform/src/quests/service.ts).');
  }
  if (quest.kind === 'visit' && quest.url === null) throw new ApiError(409, 'url_required', `Set the URL of "${slug}" first (set-url).`);
  return update(deps, actor, quest, { active: true }, 'quest.activate', {});
}

/** Switch a quest off: it disappears from the players' list and cannot be started or claimed. Claims already paid stay. */
export async function adminDeactivateQuest(deps: Deps, actor: Actor, slug: string): Promise<QuestRow> {
  const quest = await requireQuest(deps, slug);
  return update(deps, actor, quest, { active: false }, 'quest.deactivate', {});
}
