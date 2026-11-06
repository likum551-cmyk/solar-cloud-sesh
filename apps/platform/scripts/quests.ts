// Manage the quest catalogue from the server's shell. Every change is audited as "cli:<os user>".
//
//   npm run admin:quests -- list
//   npm run admin:quests -- add --slug <slug> --kind visit --title "Follow on X" [--description "..."] [--url https://...] --reward 100 [--sort 60]
//   npm run admin:quests -- set-url --slug <slug> --url https://...
//   npm run admin:quests -- set-reward --slug <slug> --amount 100
//   npm run admin:quests -- activate --slug <slug>
//   npm run admin:quests -- deactivate --slug <slug>
//
// Every command accepts --dry-run (prints what would change, changes nothing). From a release build:
//   node --env-file=/opt/gage/platform.env dist/scripts/quests.js <command> [flags]
// Needs DATABASE_URL. A new quest starts switched off; activate it once its URL and reward are right.
import { userInfo } from 'node:os';
import { parseArgs } from 'node:util';
import { ConfigError, loadConfig } from '../src/config.js';
import { openDatabase } from '../src/db/index.js';
import { QUEST_KINDS, type QuestKind } from '../src/db/schema.js';
import { buildDeps } from '../src/platform.js';
import {
  adminActivateQuest,
  adminAddQuest,
  adminDeactivateQuest,
  adminListQuests,
  adminSetQuestReward,
  adminSetQuestUrl,
} from '../src/quests/admin.js';
import { ApiError } from '../src/util/errors.js';
import { createLogger, errorFields } from '../src/util/logger.js';

const COMMANDS = ['list', 'add', 'set-url', 'set-reward', 'activate', 'deactivate'] as const;
type Command = (typeof COMMANDS)[number];

const USAGE = `usage: admin:quests -- <${COMMANDS.join('|')}> [--slug <slug>] [--kind <${QUEST_KINDS.join('|')}>] [--title <t>] [--description <d>] [--url <https://...>] [--reward <n>] [--amount <n>] [--sort <n>] [--dry-run]`;

function int(v: string | undefined): number | null {
  if (v === undefined || !/^-?\d+$/.test(v)) return null;
  return Number(v);
}

async function main(): Promise<number> {
  const logger = createLogger({ level: 'info' });
  let parsed;
  try {
    parsed = parseArgs({
      allowPositionals: true,
      options: {
        slug: { type: 'string' },
        kind: { type: 'string' },
        title: { type: 'string' },
        description: { type: 'string' },
        url: { type: 'string' },
        reward: { type: 'string' },
        amount: { type: 'string' },
        sort: { type: 'string' },
        'dry-run': { type: 'boolean', default: false },
      },
    });
  } catch (err) {
    logger.error(USAGE, { problem: (err as Error).message });
    return 2;
  }
  const { values, positionals } = parsed;
  const command = positionals[0] as Command | undefined;
  if (command === undefined || !(COMMANDS as readonly string[]).includes(command) || positionals.length > 1) {
    logger.error(USAGE);
    return 2;
  }
  const dryRun = values['dry-run'];
  const slug = values.slug;
  if (command !== 'list' && !slug) {
    logger.error(`${command} needs --slug`);
    return 2;
  }

  let config;
  try {
    config = loadConfig(process.env);
  } catch (err) {
    logger.error('invalid configuration', err instanceof ConfigError ? { issues: err.issues } : errorFields(err));
    return 1;
  }
  if (!process.env['DATABASE_URL']) {
    logger.error('DATABASE_URL is not set');
    return 1;
  }

  const handle = await openDatabase(config.databaseUrl);
  try {
    const { deps } = await buildDeps({ config, logger, handle });
    const actor = { accountId: null, label: `cli:${userInfo().username}` };
    const all = await adminListQuests(deps);
    const current = slug === undefined ? undefined : all.find((q) => q.slug === slug);

    if (command === 'list') {
      for (const q of all) {
        process.stdout.write(`${q.active ? 'ON ' : 'off'}  ${q.slug.padEnd(24)} ${q.kind.padEnd(13)} ${String(q.rewardAmount).padStart(7)} ${q.currency}  sort ${String(q.sort).padStart(3)}  ${q.url ?? '(no url)'}  "${q.title}"\n`);
      }
      return 0;
    }
    if (command !== 'add' && current === undefined) {
      logger.error('no such quest', { slug });
      return 1;
    }
    const s = slug as string;

    switch (command) {
      case 'add': {
        const reward = int(values.reward);
        const sort = values.sort === undefined ? 0 : int(values.sort);
        if (!values.kind || !(QUEST_KINDS as readonly string[]).includes(values.kind) || !values.title || reward === null || sort === null) {
          logger.error('add needs --kind, --title and --reward (whole number); --sort must be a whole number');
          return 2;
        }
        const input = { slug: s, kind: values.kind as QuestKind, title: values.title, description: values.description ?? '', url: values.url ?? null, reward, sort };
        if (current !== undefined) {
          logger.error('a quest with this slug already exists', { slug: s });
          return 1;
        }
        if (dryRun) {
          logger.info('dry run: would add (switched off)', input);
          return 0;
        }
        const row = await adminAddQuest(deps, actor, input);
        logger.info('quest added (switched off: run activate when it is ready)', { slug: row.slug, kind: row.kind, reward: row.rewardAmount });
        return 0;
      }
      case 'set-url': {
        if (!values.url) {
          logger.error('set-url needs --url https://...');
          return 2;
        }
        if (dryRun) {
          logger.info('dry run: would set url', { slug: s, before: current?.url ?? null, after: values.url });
          return 0;
        }
        const row = await adminSetQuestUrl(deps, actor, s, values.url);
        logger.info('url set', { slug: row.slug, url: row.url });
        return 0;
      }
      case 'set-reward': {
        const amount = int(values.amount);
        if (amount === null) {
          logger.error('set-reward needs --amount <whole number>');
          return 2;
        }
        if (dryRun) {
          logger.info('dry run: would set reward', { slug: s, before: current?.rewardAmount, after: amount });
          return 0;
        }
        const row = await adminSetQuestReward(deps, actor, s, amount);
        logger.info('reward set', { slug: row.slug, reward: row.rewardAmount });
        return 0;
      }
      case 'activate':
      case 'deactivate': {
        const on = command === 'activate';
        if (current?.active === on) {
          logger.info(`nothing to change: already ${on ? 'on' : 'off'}`, { slug: s });
          return 0;
        }
        if (dryRun) {
          // The same refusals adminActivateQuest makes, reported without writing.
          const refusal = !on ? null : current?.kind === 'discord_guild' ? 'quest_unavailable' : current?.kind === 'visit' && current.url === null ? 'url_required' : null;
          if (refusal) {
            logger.error('dry run: activate would be refused', { slug: s, code: refusal });
            return 1;
          }
          logger.info(`dry run: would switch ${on ? 'on' : 'off'}`, { slug: s, url: current?.url ?? null, kind: current?.kind });
          return 0;
        }
        const row = on ? await adminActivateQuest(deps, actor, s) : await adminDeactivateQuest(deps, actor, s);
        logger.info(on ? 'quest switched on' : 'quest switched off', { slug: row.slug });
        return 0;
      }
    }
  } catch (err) {
    if (err instanceof ApiError) logger.error('refused', { code: err.code, message: err.message });
    else logger.error('failed', errorFields(err));
    return 1;
  } finally {
    await handle.close();
  }
}

main().then(
  (code) => process.exit(code),
  (err: unknown) => {
    createLogger({ level: 'error' }).error('fatal', errorFields(err));
    process.exit(1);
  },
);
