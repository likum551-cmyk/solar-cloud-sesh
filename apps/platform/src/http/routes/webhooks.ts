import { randomUUID } from 'node:crypto';
import { eq } from 'drizzle-orm';
import { z } from 'zod';
import type { Deps } from '../../deps.js';
import { orders } from '../../db/schema.js';
import { FAKE_SIGNATURE_HEADER, FakeProvider } from '../../shop/fake-provider.js';
import { handleWebhook } from '../../shop/service.js';
import { ApiError } from '../../util/errors.js';
import { uuidParam, type Router } from '../router.js';

const WEBHOOK_MAX_BYTES = 262_144;
const completeBody = z.object({ outcome: z.enum(['succeeded', 'failed']).default('succeeded') });

/** Provider webhooks, plus the dev-only fake checkout that "pays" an order through the real webhook path. */
export function registerWebhookRoutes(r: Router, deps: Deps): void {
  r.add(
    {
      method: 'post', path: '/v1/webhooks/:provider', summary: 'Payment provider webhook', auth: 'webhook', tags: ['shop', 'webhooks'],
      description: 'Authenticated by the provider\'s signature over the raw body (provider-specific header). Verified events always get 200 { received, duplicate, result }; (provider, event id) is processed once. A bad signature is 400 invalid_webhook.',
      response: '200 { received: true, duplicate: boolean, result: applied | ignored | unmatched | rejected | duplicate }',
      errors: ['invalid_webhook', 'unknown_provider'], maxBodyBytes: WEBHOOK_MAX_BYTES,
    },
    async (c) => {
      const raw = await c.req.text();
      const headers: Record<string, string> = {};
      c.req.raw.headers.forEach((value, key) => {
        headers[key.toLowerCase()] = value;
      });
      return c.json(await handleWebhook(deps, c.req.param('provider') ?? '', raw, headers));
    },
  );

  const fake = deps.payments.find((p): p is FakeProvider => p instanceof FakeProvider);
  // Opt-in only (ENABLE_DEV_ROUTES=1, which loadConfig refuses in production): an unset or mistyped NODE_ENV must never mount them.
  const devEnabled = fake !== undefined && deps.config.paymentProvider === 'fake' && deps.config.devRoutes && deps.config.nodeEnv !== 'production';

  r.add(
    {
      method: 'get', path: '/v1/dev/fake-checkout/:orderId', summary: 'Dev only: the fake provider\'s "payment page"', auth: 'none', tags: ['dev'],
      description: 'Mounted only with PAYMENT_PROVIDER=fake and ENABLE_DEV_ROUTES=1 (refused in production). Says how to complete the payment.',
      response: '200 { orderId, status, totalMinor, currency, complete: { method, url, body } }', errors: ['order_not_found'], enabled: devEnabled,
    },
    async (c) => {
      const id = uuidParam(c, 'orderId');
      const [order] = await deps.db.select().from(orders).where(eq(orders.id, id)).limit(1);
      if (!order) throw new ApiError(404, 'order_not_found', 'No such order.');
      return c.json({
        orderId: order.id, status: order.status, totalMinor: order.totalMinor, currency: order.currency,
        complete: { method: 'POST', url: `${deps.config.publicApiUrl}/v1/dev/fake-checkout/${order.id}/complete`, body: { outcome: 'succeeded | failed' } },
      });
    },
  );

  r.add(
    {
      method: 'post', path: '/v1/dev/fake-checkout/:orderId/complete', summary: 'Dev only: pay or fail an order through a signed webhook', auth: 'none', tags: ['dev'], body: completeBody.optional().default({ outcome: 'succeeded' }),
      description: 'Builds a correctly signed fake-provider event and runs it through the same handler as POST /v1/webhooks/fake.',
      response: '200 webhook outcome', errors: ['order_not_found'], enabled: devEnabled,
    },
    async (c, { body }) => {
      if (!fake) throw new ApiError(404, 'not_found', 'Not found.');
      const id = uuidParam(c, 'orderId');
      const [order] = await deps.db.select().from(orders).where(eq(orders.id, id)).limit(1);
      if (!order) throw new ApiError(404, 'order_not_found', 'No such order.');
      const event = JSON.stringify({
        id: `evt_${randomUUID()}`,
        type: body.outcome === 'succeeded' ? 'payment.succeeded' : 'payment.failed',
        orderId: order.id,
        providerRef: order.providerRef,
        amountMinor: order.totalMinor,
        currency: order.currency,
      });
      return c.json(await handleWebhook(deps, fake.id, event, { [FAKE_SIGNATURE_HEADER]: fake.sign(event) }));
    },
  );
}
