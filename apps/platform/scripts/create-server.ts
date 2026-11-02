// Register an official game server and print its secret once (the database keeps only a hash).
// Audited as "cli:<os user>". Refuses an id that already exists (use the admin API to edit or
// rotate).
//
//   npm run admin:create-server -- --id eu-1 --name "GAGE EU 1" --region eu --host 203.0.113.10 \
//       --port 24800 --allowed-ip 203.0.113.10 [--tag roleplay] [--hidden] [--dry-run]
//
// Needs DATABASE_URL.
import { userInfo } from 'node:os';
import { parseArgs } from 'node:util';
import { ConfigError, loadConfig } from '../src/config.js';
import { openDatabase } from '../src/db/index.js';
import { buildDeps } from '../src/platform.js';
import { createServer, serverCreateSchema } from '../src/servers/service.js';
import { ApiError } from '../src/util/errors.js';
import { createLogger, errorFields } from '../src/util/logger.js';

async function main(): Promise<number> {
  const logger = createLogger({ level: 'info' });
  const { values } = parseArgs({
    options: {
      id: { type: 'string' },
      name: { type: 'string' },
      region: { type: 'string' },
      host: { type: 'string' },
      port: { type: 'string' },
      'allowed-ip': { type: 'string', multiple: true },
      tag: { type: 'string', multiple: true },
      hidden: { type: 'boolean', default: false },
      'dry-run': { type: 'boolean', default: false },
    },
  });
  const parsed = serverCreateSchema.safeParse({
    id: values.id,
    name: values.name,
    region: values.region,
    host: values.host,
    port: values.port === undefined ? undefined : Number(values.port),
    allowedIps: values['allowed-ip'] ?? [],
    tags: values.tag ?? [],
    visible: !values.hidden,
  });
  if (!parsed.success) {
    logger.error('invalid arguments', { issues: parsed.error.issues.map((i) => `${i.path.join('.')}: ${i.message}`) });
    return 2;
  }
  if (values['dry-run']) {
    logger.info('dry run: would create the server', { ...parsed.data });
    return 0;
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
    const { server, secret } = await createServer(deps, { actor: { accountId: null, label: `cli:${userInfo().username}` }, actorRoles: ['admin'] }, parsed.data);
    logger.info('server created', { id: server.id, host: server.host, port: server.port, allowedIps: server.allowedIps });
    // Plain stdout, not the JSON log: this is the one time the secret exists.
    process.stdout.write(`\nGAGE_SERVER_ID=${server.id}\nGAGE_SERVER_SECRET=${secret}\n\nStore the secret now; it cannot be shown again (rotate it via the admin API if lost).\n`);
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
