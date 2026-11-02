// The payment seam. The shop never talks to a payment company directly: it talks to a
// PaymentProvider. FakeProvider (fake-provider.ts) implements it for dev and tests; Tebex, Paddle,
// Stripe or a crypto processor are written later as further implementations of the same
// interface and registered in createPlatform(). Nothing outside src/shop/ and the webhook route
// knows which provider is in use.
//
// Contract every implementation must keep:
//   - createCheckout is idempotent per `orderId` (the same order id returns the same session).
//   - verifyWebhook authenticates the request (signature over the RAW body, with a timestamp
//     tolerance if the provider offers one) and throws WebhookVerificationError otherwise. It
//     must never trust the body's own claims before the signature check.
//   - A WebhookEvent carries a provider-unique `eventId`: the platform stores (provider, eventId)
//     under a unique constraint, so a redelivered event is processed once.
//   - refund is idempotent per `idempotencyKey`.
//   - Amounts are integers in minor units (cents) and currencies are ISO 4217 upper case.

export interface CheckoutInput {
  /** Our order id: the provider's client reference and the idempotency key. */
  orderId: string;
  accountId: string;
  currency: string;
  totalMinor: number;
  items: { sku: string; name: string; quantity: number; unitPriceMinor: number }[];
  customerEmail: string | null;
  expiresAt: Date;
}

export interface CheckoutSession {
  /** The provider's id for this payment/checkout; stored on the order. */
  providerRef: string;
  /** Where the launcher sends the player to pay. */
  checkoutUrl: string;
}

export type WebhookEventType = 'payment.succeeded' | 'payment.failed' | 'refund.succeeded' | 'chargeback.opened';

export interface WebhookEvent {
  eventId: string;
  type: WebhookEventType;
  /** Our order id, echoed back by the provider. */
  orderId: string;
  providerRef: string | null;
  /** Amount the event is about (paid, refunded or disputed), minor units. null when the provider does not say. */
  amountMinor: number | null;
  currency: string | null;
  occurredAt: Date;
}

export interface WebhookRequest {
  /** The body exactly as received (signatures are computed over these bytes). */
  rawBody: string;
  /** Header names lower-cased. */
  headers: Record<string, string>;
}

export interface RefundInput {
  orderId: string;
  providerRef: string;
  amountMinor: number;
  currency: string;
  idempotencyKey: string;
  reason: string | null;
}

export interface RefundResult {
  refundRef: string;
}

export class WebhookVerificationError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'WebhookVerificationError';
  }
}

export interface PaymentProvider {
  /** Stable id: stored on orders and used in the webhook URL (/v1/webhooks/:id). Lower-case slug. */
  readonly id: string;
  createCheckout(input: CheckoutInput): Promise<CheckoutSession>;
  /** Verify and parse a webhook. Throws WebhookVerificationError for a bad signature, stale timestamp or malformed body. */
  verifyWebhook(req: WebhookRequest): WebhookEvent;
  refund(input: RefundInput): Promise<RefundResult>;
}
