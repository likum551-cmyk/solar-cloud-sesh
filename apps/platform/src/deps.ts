import type { Config } from './config.js';
import type { Db } from './db/index.js';
import type { DiscordClient } from './auth/discord-client.js';
import type { EmailSender } from './email.js';
import type { PaymentProvider } from './shop/provider.js';
import type { ServerStatusService } from './servers/status.js';
import type { Logger } from './util/logger.js';

/**
 * Everything a service function needs, injected so tests can swap the clock, the mail sender, the
 * Discord client, the payment provider and the UDP prober without touching the code under test.
 */
export interface Deps {
  config: Config;
  db: Db;
  /** Resolves when the database answers (the /health check). */
  dbPing: () => Promise<void>;
  logger: Logger;
  /** The only source of "now" in application code (tests advance it). */
  clock: () => Date;
  email: EmailSender;
  /** null when Discord login is not configured. */
  discord: DiscordClient | null;
  /** Every payment provider that may deliver webhooks; `config.paymentProvider` names the one used for new checkouts. */
  payments: PaymentProvider[];
  serverStatus: ServerStatusService;
}

/** Who performed an admin action (for the audit log). */
export interface Actor {
  accountId: string | null;
  /** "account:<id>", "cli:<os user>" or "system". */
  label: string;
}
