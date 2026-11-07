import type { EmailMessage, EmailSender } from './email.js';
import type { Logger } from './util/logger.js';

/**
 * EMAIL_DRIVER=resend: delivers through Resend's HTTP API (POST <apiBase>/emails with a Bearer key,
 * https://resend.com/docs/api-reference/emails/send-email). No SDK: one fetch per message.
 *
 * `send` only QUEUES (EmailSender's contract: the forgot-password path awaits it, and waiting on the
 * provider would let response time reveal whether an address has an account). Delivery runs in the
 * background with retries on 429 / 5xx / network errors; a 4xx other than 429 is final (a bad key, an
 * unverified sender domain, a refused address) and is logged as an error. The log carries the
 * recipient, the template, Resend's message id and the failure, NEVER the body or subject text
 * (they hold live verification / reset tokens).
 *
 * `record` (optional) is called first with the message, e.g. the outbox sender, so support still sees
 * a redacted copy of every mail in the database.
 */
export interface ResendOptions {
  apiKey: string;
  apiBase: string;
  from: string;
  logger: Logger;
  fetchImpl?: typeof fetch;
  record?: EmailSender;
  /** Waits between attempts (ms); the number of attempts is this length + 1. */
  retryDelaysMs?: readonly number[];
  /** Per-attempt timeout (ms). */
  timeoutMs?: number;
  sleep?: (ms: number) => Promise<void>;
}

export const RESEND_RETRY_DELAYS_MS = [2_000, 10_000, 30_000] as const;

export class ResendEmailSender implements EmailSender {
  private readonly fetchImpl: typeof fetch;
  private readonly delays: readonly number[];
  private readonly timeoutMs: number;
  private readonly sleep: (ms: number) => Promise<void>;
  private readonly inFlight = new Set<Promise<void>>();

  constructor(private readonly opts: ResendOptions) {
    this.fetchImpl = opts.fetchImpl ?? fetch;
    this.delays = opts.retryDelaysMs ?? RESEND_RETRY_DELAYS_MS;
    this.timeoutMs = opts.timeoutMs ?? 10_000;
    this.sleep = opts.sleep ?? ((ms) => new Promise((r) => setTimeout(r, ms)));
  }

  async send(m: EmailMessage): Promise<void> {
    if (this.opts.record) await this.opts.record.send(m);
    const p = this.deliver(m).finally(() => this.inFlight.delete(p));
    this.inFlight.add(p);
  }

  /** Resolves when every queued delivery has finished (tests, graceful shutdown). */
  async drain(): Promise<void> {
    while (this.inFlight.size > 0) await Promise.allSettled([...this.inFlight]);
  }

  private async deliver(m: EmailMessage): Promise<void> {
    const url = `${this.opts.apiBase}/emails`;
    const body = JSON.stringify({ from: this.opts.from, to: [m.to], subject: m.subject, text: m.text });
    const attempts = this.delays.length + 1;
    let last = '';
    for (let i = 0; i < attempts; i++) {
      if (i > 0) await this.sleep(this.delays[i - 1] ?? 0);
      const ac = new AbortController();
      const timer = setTimeout(() => ac.abort(), this.timeoutMs);
      try {
        const res = await this.fetchImpl(url, {
          method: 'POST',
          headers: { Authorization: `Bearer ${this.opts.apiKey}`, 'Content-Type': 'application/json' },
          body,
          signal: ac.signal,
        });
        const data = (await res.json().catch(() => null)) as { id?: unknown; message?: unknown; name?: unknown } | null;
        if (res.ok) {
          this.opts.logger.info('email sent (resend)', { to: m.to, template: m.template, id: typeof data?.id === 'string' ? data.id : null, attempt: i + 1 });
          return;
        }
        last = `HTTP ${res.status}${typeof data?.name === 'string' ? ` ${data.name}` : ''}${typeof data?.message === 'string' ? `: ${data.message}` : ''}`;
        if (res.status !== 429 && res.status < 500) break;   // final: retrying a refused request cannot help
      } catch (err) {
        last = err instanceof Error ? `${err.name}: ${err.message}` : String(err);
      } finally {
        clearTimeout(timer);
      }
    }
    this.opts.logger.error('email NOT delivered (resend)', { to: m.to, template: m.template, error: last });
  }
}
