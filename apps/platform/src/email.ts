import type { Db } from './db/index.js';
import { emailOutbox } from './db/schema.js';
import type { Logger } from './util/logger.js';

/**
 * What the platform sends. `template` identifies the message; `meta` carries non-secret machine-
 * readable facts. `secrets` lists every live single-use value the text contains (a verification or
 * reset token): a sender that stores or logs the message must go through `redactSecrets` first.
 */
export interface EmailMessage {
  to: string;
  subject: string;
  text: string;
  template: 'verify_email' | 'password_reset' | 'account_exists' | 'password_changed' | 'discord_linked' | 'discord_unlinked';
  meta?: Record<string, unknown>;
  secrets?: readonly string[];
}

/**
 * Delivery seam. A real provider (SES, Postmark, SMTP...) implements this and is passed to
 * createPlatform(); nothing else in the code knows how mail leaves the box. `send` should resolve
 * once the message is accepted for delivery (queued) and reject on failure. Implementations
 * should enqueue rather than wait on the provider: the forgot-password path awaits `send`, so a
 * slow provider would otherwise reveal whether an address has an account through response time.
 */
export interface EmailSender {
  send(message: EmailMessage): Promise<void>;
}

export const REDACTED = '[redacted]';

/** `text` with every secret (and its URL-encoded form) replaced by "[redacted]". */
export function redactSecrets(text: string, secrets: readonly string[] | undefined): string {
  let out = text;
  for (const s of secrets ?? []) {
    if (s === '') continue;
    out = out.split(s).join(REDACTED).split(encodeURIComponent(s)).join(REDACTED);
  }
  return out;
}

export interface OutboxOptions {
  /** Where the development link is logged. */
  logger?: Logger;
  /** Development only: also log the whole message (link and code included) so a developer can click it. Off by default. */
  logLinks?: boolean;
}

/**
 * Dev/test sender: stores every message in the `email_outbox` table with the live token REDACTED
 * (body and meta), so a database read, a backup or a support export holds nothing that takes over
 * an account. In development (`logLinks`) the full message, link included, goes to the log
 * instead, which is how a developer follows a verification or reset link; never enable that where
 * the log is readable by others. It delivers nothing: plug a real EmailSender in before launch.
 */
export class OutboxEmailSender implements EmailSender {
  constructor(
    private readonly db: Db,
    private readonly clock: () => Date,
    private readonly opts: OutboxOptions = {},
  ) {}

  async send(m: EmailMessage): Promise<void> {
    const meta = JSON.parse(redactSecrets(JSON.stringify(m.meta ?? {}), m.secrets)) as Record<string, unknown>;
    await this.db.insert(emailOutbox).values({
      toAddress: m.to,
      subject: redactSecrets(m.subject, m.secrets),
      body: redactSecrets(m.text, m.secrets),
      template: m.template,
      meta,
      createdAt: this.clock(),
    });
    if (this.opts.logLinks === true && this.opts.logger) {
      this.opts.logger.info('email (outbox driver, development: not delivered, link logged)', { to: m.to, subject: m.subject, template: m.template, body: m.text });
    }
  }
}

/**
 * Dev sender: prints the message to the structured log. Live tokens are redacted unless `logLinks`
 * (development) is set, so a production log never holds a working verification or reset link.
 */
export class ConsoleEmailSender implements EmailSender {
  constructor(
    private readonly logger: Logger,
    private readonly opts: { logLinks?: boolean } = {},
  ) {}

  send(m: EmailMessage): Promise<void> {
    const body = this.opts.logLinks === true ? m.text : redactSecrets(m.text, m.secrets);
    this.logger.info('email (console driver, not delivered)', { to: m.to, subject: m.subject, template: m.template, body });
    return Promise.resolve();
  }
}

/** Collects messages in memory (for unit tests that do not need a database). */
export class MemoryEmailSender implements EmailSender {
  readonly sent: EmailMessage[] = [];
  send(m: EmailMessage): Promise<void> {
    this.sent.push(m);
    return Promise.resolve();
  }
}

/** A third party's display name made safe for a plain-text mail: no control characters or line breaks, bounded. */
function plain(name: string): string {
  return [...name].filter((ch) => ch.charCodeAt(0) >= 0x20 && ch.charCodeAt(0) !== 0x7f).join('').slice(0, 64);
}

export function verifyEmailMessage(linkBase: string, to: string, username: string, token: string, ttlHours: number): EmailMessage {
  return {
    to,
    template: 'verify_email',
    subject: 'Verify your GAGE-MP email',
    text:
      `Hi ${username},\n\nConfirm your email address to finish setting up your GAGE-MP account:\n\n` +
      `${linkBase}/verify-email?token=${encodeURIComponent(token)}\n\n` +
      `If the link does not open the launcher, paste this code into the verification screen:\n${token}\n\n` +
      `The code is valid for ${ttlHours} hours and works once. If you did not create this account, ignore this email.\n`,
    secrets: [token],
  };
}

export function passwordResetMessage(linkBase: string, to: string, username: string, token: string, ttlMinutes: number): EmailMessage {
  return {
    to,
    template: 'password_reset',
    subject: 'Reset your GAGE-MP password',
    text:
      `Hi ${username},\n\nSomeone asked to reset the password of your GAGE-MP account. To choose a new one open:\n\n` +
      `${linkBase}/reset-password?token=${encodeURIComponent(token)}\n\n` +
      `or paste this code into the reset screen:\n${token}\n\n` +
      `The code is valid for ${ttlMinutes} minutes and works once. If this was not you, ignore this email: your password has not changed.\n`,
    secrets: [token],
  };
}

export function accountExistsMessage(to: string, username: string): EmailMessage {
  return {
    to,
    template: 'account_exists',
    subject: 'You already have a GAGE-MP account',
    text:
      `Hi ${username},\n\nSomeone tried to register a GAGE-MP account with this email address, but it already has one. ` +
      `If that was you, log in or use "Forgot password". If not, no action is needed.\n`,
  };
}

export function passwordChangedMessage(to: string, username: string): EmailMessage {
  return {
    to,
    template: 'password_changed',
    subject: 'Your GAGE-MP password was changed',
    text:
      `Hi ${username},\n\nThe password of your GAGE-MP account was just changed and every other device was signed out. ` +
      `If this was not you, reset your password immediately and contact support.\n`,
  };
}

export function discordLinkedMessage(to: string, username: string, discordName: string | null): EmailMessage {
  return {
    to,
    template: 'discord_linked',
    subject: 'A Discord account was linked to your GAGE-MP account',
    text:
      `Hi ${username},\n\nThe Discord account ${discordName === null ? '' : `"${plain(discordName)}" `}was just linked to your GAGE-MP account. ` +
      `From now on it can be used to sign in to it.\n\n` +
      `If this was you, nothing more is needed. If it was not, someone else has access to your account: change your password at once ` +
      `(this signs your other devices out), unlink the Discord account in the launcher settings, and contact support.\n`,
    meta: { discord: discordName === null ? null : plain(discordName) },
  };
}

export function discordUnlinkedMessage(to: string, username: string, discordName: string | null): EmailMessage {
  return {
    to,
    template: 'discord_unlinked',
    subject: 'A Discord account was unlinked from your GAGE-MP account',
    text:
      `Hi ${username},\n\nThe Discord account ${discordName === null ? '' : `"${plain(discordName)}" `}was just unlinked from your GAGE-MP account, ` +
      `so it can no longer be used to sign in to it.\n\n` +
      `If this was you, nothing more is needed. If it was not, change your password at once (this signs your other devices out) and contact support.\n`,
    meta: { discord: discordName === null ? null : plain(discordName) },
  };
}
