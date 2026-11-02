import { createHmac, timingSafeEqual } from 'node:crypto';
import { z } from 'zod';
import { hashToken } from '../util/tokens.js';
import {
  WebhookVerificationError,
  type CheckoutInput,
  type CheckoutSession,
  type PaymentProvider,
  type RefundInput,
  type RefundResult,
  type WebhookEvent,
  type WebhookRequest,
} from './provider.js';

/** Header carrying the signature: `t=<unix seconds>,v1=<hex HMAC-SHA256 of "<t>.<raw body>">` (the Stripe scheme). */
export const FAKE_SIGNATURE_HEADER = 'x-fake-signature';
/** How far a signature timestamp may be from now. */
export const FAKE_SIGNATURE_TOLERANCE_SECONDS = 300;

const eventBody = z.object({
  id: z.string().min(1).max(100),
  type: z.enum(['payment.succeeded', 'payment.failed', 'refund.succeeded', 'chargeback.opened']),
  orderId: z.guid(),
  providerRef: z.string().max(200).nullable().default(null),
  amountMinor: z.number().int().positive().nullable().default(null),
  currency: z.string().regex(/^[A-Z]{3}$/).nullable().default(null),
  occurredAt: z.string().datetime().optional(),
});

/**
 * A payment provider for development and tests. Nothing is charged: "paying" means delivering a
 * correctly signed webhook (signWebhook builds one; the dev route /v1/dev/fake-checkout does it
 * for the launcher). Its webhooks are authenticated with HMAC-SHA256 like a real provider's, so
 * the verification path the shop depends on is exercised for real.
 *
 * It must never run in production: loadConfig() (src/config.ts) refuses PAYMENT_PROVIDER=fake
 * when NODE_ENV=production unless ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1 is set.
 */
export class FakeProvider implements PaymentProvider {
  readonly id = 'fake';
  /** Refunds issued, for assertions. */
  readonly refunds: RefundInput[] = [];
  /** Set to make the next refund call throw (to test provider failure handling). */
  failNextRefund = false;

  constructor(
    private readonly secret: string,
    private readonly publicBaseUrl: string,
    private readonly clock: () => Date,
  ) {}

  createCheckout(input: CheckoutInput): Promise<CheckoutSession> {
    // Deterministic from the order id: the same order always maps to the same session (idempotent).
    const providerRef = `fake_cs_${hashToken(input.orderId).slice(0, 24)}`;
    return Promise.resolve({ providerRef, checkoutUrl: `${this.publicBaseUrl}/v1/dev/fake-checkout/${input.orderId}` });
  }

  /** Build the header value for `rawBody` signed at `at` (default: now). */
  sign(rawBody: string, at: Date = this.clock()): string {
    const t = Math.floor(at.getTime() / 1000);
    const mac = createHmac('sha256', this.secret).update(`${t}.${rawBody}`).digest('hex');
    return `t=${t},v1=${mac}`;
  }

  verifyWebhook(req: WebhookRequest): WebhookEvent {
    const header = req.headers[FAKE_SIGNATURE_HEADER];
    if (header === undefined) throw new WebhookVerificationError('missing signature header');
    const parts = Object.fromEntries(header.split(',').map((p) => {
      const i = p.indexOf('=');
      return i < 0 ? [p.trim(), ''] : [p.slice(0, i).trim(), p.slice(i + 1).trim()];
    }));
    const t = Number(parts['t']);
    const v1 = parts['v1'];
    if (!Number.isInteger(t) || typeof v1 !== 'string' || !/^[0-9a-f]{64}$/.test(v1)) {
      throw new WebhookVerificationError('malformed signature header');
    }
    const expected = createHmac('sha256', this.secret).update(`${t}.${req.rawBody}`).digest();
    if (!timingSafeEqual(expected, Buffer.from(v1, 'hex'))) throw new WebhookVerificationError('signature mismatch');
    if (Math.abs(this.clock().getTime() / 1000 - t) > FAKE_SIGNATURE_TOLERANCE_SECONDS) {
      throw new WebhookVerificationError('timestamp outside the tolerance window');
    }

    let json: unknown;
    try {
      json = JSON.parse(req.rawBody);
    } catch {
      throw new WebhookVerificationError('body is not JSON');
    }
    const parsed = eventBody.safeParse(json);
    if (!parsed.success) throw new WebhookVerificationError('body does not match the event schema');
    const b = parsed.data;
    return {
      eventId: b.id,
      type: b.type,
      orderId: b.orderId,
      providerRef: b.providerRef,
      amountMinor: b.amountMinor,
      currency: b.currency,
      occurredAt: b.occurredAt ? new Date(b.occurredAt) : this.clock(),
    };
  }

  refund(input: RefundInput): Promise<RefundResult> {
    if (this.failNextRefund) {
      this.failNextRefund = false;
      return Promise.reject(new Error('fake provider: refund failed'));
    }
    // Idempotent: the same key is recorded once and always yields the same refund reference.
    if (!this.refunds.some((r) => r.idempotencyKey === input.idempotencyKey)) this.refunds.push(input);
    return Promise.resolve({ refundRef: `fake_re_${hashToken(input.idempotencyKey).slice(0, 24)}` });
  }
}
