// Grant or remove a role on an account from the server's shell (the bootstrap path for the first
// admin, and the break-glass when no admin can log in). Audited as "cli:<os user>".
//
//   npm run admin:grant-role -- --user <username-or-email> --role admin [--remove] [--dry-run]
//
// Needs DATABASE_URL. Idempotent: granting a role the account already has changes nothing.
import { userInfo } from 'node:os';
import { parseArgs } from 'node:util';
import { setRoles } from '../src/admin/service.js';
import { findAccountByIdentifier, getRoles } from '../src/auth/accounts.js';
import { ConfigError, loadConfig } from '../src/config.js';
import { openDatabase } from '../src/db/index.js';
import { ROLES, type Role } from '../src/db/schema.js';
import { buildDeps } from '../src/platform.js';
import { ApiError } from '../src/util/errors.js';
import { createLogger, errorFields } from '../src/util/logger.js';

async function main(): Promise<number> {
  const logger = createLogger({ level: 'info' });
  const { values } = parseArgs({
    options: {
      user: { type: 'string' },
      role: { type: 'string' },
      remove: { type: 'boolean', default: false },
      'dry-run': { type: 'boolean', default: false },
    },
  });
  if (!values.user || !values.role || !(ROLES as readonly string[]).includes(values.role)) {
    logger.error(`usage: admin:grant-role -- --user <username-or-email> --role <${ROLES.join('|')}> [--remove] [--dry-run]`);
    return 2;
  }
  const role = values.role as Role;

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
    const account = await findAccountByIdentifier(handle.db, values.user);
    if (!account) {
      logger.error('no such account', { user: values.user });
      return 1;
    }
    const current = await getRoles(handle.db, account.id);
    const next = values.remove ? current.filter((r) => r !== role) : [...new Set([...current, role])];
    logger.info(values['dry-run'] ? 'dry run: would set roles' : 'setting roles', { username: account.username, before: current, after: next });
    if (values['dry-run']) return 0;
    if (current.length === next.length && current.every((r) => next.includes(r))) {
      logger.info('nothing to change');
      return 0;
    }
    const label = `cli:${userInfo().username}`;
    await setRoles(deps, { actor: { accountId: null, label }, actorRoles: ['admin'] }, account.id, { roles: next });
    logger.info('roles updated', { username: account.username, roles: next });
    return 0;
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
