import { ConfigError, loadConfig, type Config } from './config.js';
import { createLogger, errorFields } from './logger.js';
import { startServer } from './server.js';

async function main(): Promise<number> {
  let config: Config;
  try {
    config = loadConfig(process.env);
  } catch (err) {
    // The configured log level is not known yet: report on stdout at error level.
    const boot = createLogger({ level: 'error' });
    if (err instanceof ConfigError) boot.error('invalid configuration', { issues: err.issues });
    else boot.error('cannot load configuration', errorFields(err));
    return 1;
  }

  const logger = createLogger({ level: config.logLevel });

  let server;
  try {
    server = await startServer(config, logger);
  } catch (err) {
    logger.error('cannot start', { host: config.host, port: config.port, ...errorFields(err) });
    return 1;
  }

  return new Promise<number>((resolve) => {
    let stopping = false;
    const stop = (signal: string): void => {
      if (stopping) {
        logger.warn('second signal, exiting now', { signal });
        resolve(1);
        return;
      }
      stopping = true;
      logger.info('shutting down', { signal });
      server.close().then(
        () => {
          logger.info('stopped');
          resolve(0);
        },
        (err: unknown) => {
          logger.error('shutdown failed', errorFields(err));
          resolve(1);
        },
      );
    };
    // SIGTERM is what systemd sends; SIGINT is Ctrl-C (the only one Windows delivers).
    process.on('SIGINT', () => stop('SIGINT'));
    process.on('SIGTERM', () => stop('SIGTERM'));
  });
}

process.on('uncaughtException', (err) => {
  createLogger({ level: 'error' }).error('uncaught exception', errorFields(err));
  process.exit(1);
});
process.on('unhandledRejection', (reason) => {
  createLogger({ level: 'error' }).error('unhandled rejection', errorFields(reason));
  process.exit(1);
});

main().then(
  (code) => process.exit(code),
  (err: unknown) => {
    createLogger({ level: 'error' }).error('fatal', errorFields(err));
    process.exit(1);
  },
);
