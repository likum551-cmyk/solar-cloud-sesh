// Apply pending database migrations (drizzle/ folder) to DATABASE_URL. Idempotent: applied
// migrations are recorded in drizzle's __drizzle_migrations table and skipped. Run once per
// deploy, before starting the new version; the server never migrates a Postgres database itself.
//
//   npm run db:migrate
import { ConfigError, loadConfig } from '../src/config.js';
import { openDatabase } from '../src/db/index.js';
import { createLogger, errorFields } from '../src/util/logger.js';

async function main(): Promise<number> {
  const logger = createLogger({ level: 'info' });
  let url: string;
  try {
    const config = loadConfig(process.env);
    if (process.env['DATABASE_URL'] === undefined || process.env['DATABASE_URL'] === '') {
      logger.error('DATABASE_URL is not set: refusing to migrate the default in-memory database');
      return 1;
    }
    url = config.databaseUrl;
  } catch (err) {
    if (err instanceof ConfigError) logger.error('invalid configuration', { issues: err.issues });
    else logger.error('cannot load configuration', errorFields(err));
    return 1;
  }

  const handle = await openDatabase(url);
  try {
    await handle.migrate();
    logger.info('migrations applied', { db: handle.kind });
    return 0;
  } catch (err) {
    logger.error('migration failed', errorFields(err));
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
