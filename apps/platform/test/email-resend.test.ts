import { describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config.js';
import { MemoryEmailSender, verifyEmailMessage, type EmailMessage } from '../src/email.js';
import { ResendEmailSender } from '../src/email-resend.js';
import type { Logger } from '../src/util/logger.js';

const KEY = 're_test_key_0123456789';
const TOKEN = 'tok_Ab3-xY9_secretSecret';

interface Line { level: string; msg: string; fields?: Record<string, unknown> }
function capture(): { logger: Logger; lines: Line[] } {
  const lines: Line[] = [];
  const at = (level: string) => (msg: string, fields?: Record<string, unknown>) => {
    lines.push({ level, msg, ...(fields ? { fields } : {}) });
  };
  return { logger: { debug: at('debug'), info: at('info'), warn: at('warn'), error: at('error') } as Logger, lines };
}

interface Call { url: string; init: RequestInit }
/** A fake Resend: answers each call with the next scripted response (a status, or an Error to throw). */
function fakeResend(script: Array<number | Error>, gate?: Promise<void>): { fetchImpl: typeof fetch; calls: Call[] } {
  const calls: Call[] = [];
  const fetchImpl = (async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), init: init ?? {} });
    if (gate) await gate;
    const step = script[calls.length - 1] ?? 500;
    if (step instanceof Error) throw step;
    const body = step < 300 ? { id: `msg_${calls.length}` } : { name: 'validation_error', message: `status ${step}` };
    return new Response(JSON.stringify(body), { status: step, headers: { 'Content-Type': 'application/json' } });
  }) as typeof fetch;
  return { fetchImpl, calls };
}

const message = (): EmailMessage => verifyEmailMessage('gage-mp://auth', 'player@example.com', 'player', TOKEN, 24);
const noSleep = async (): Promise<void> => {};

function sender(script: Array<number | Error>, extra: { record?: MemoryEmailSender; gate?: Promise<void> } = {}) {
  const { logger, lines } = capture();
  const { fetchImpl, calls } = fakeResend(script, extra.gate);
  const s = new ResendEmailSender({
    apiKey: KEY,
    apiBase: 'https://resend.test',
    from: 'Solar RP <no-reply@solar-rp.com>',
    logger,
    fetchImpl,
    sleep: noSleep,
    ...(extra.record ? { record: extra.record } : {}),
  });
  return { s, calls, lines };
}

describe('ResendEmailSender', () => {
  it('POSTs one message to <apiBase>/emails with the Bearer key and the Resend body shape', async () => {
    const { s, calls, lines } = sender([200]);
    await s.send(message());
    await s.drain();
    expect(calls).toHaveLength(1);
    const c = calls[0]!;
    expect(c.url).toBe('https://resend.test/emails');
    expect(c.init.method).toBe('POST');
    const h = new Headers(c.init.headers);
    expect(h.get('authorization')).toBe(`Bearer ${KEY}`);
    expect(h.get('content-type')).toBe('application/json');
    const body = JSON.parse(String(c.init.body)) as Record<string, unknown>;
    expect(body.from).toBe('Solar RP <no-reply@solar-rp.com>');
    expect(body.to).toEqual(['player@example.com']);
    expect(body.subject).toBe(message().subject);
    expect(String(body.text)).toContain(TOKEN);   // the player gets the real link
    const ok = lines.find((l) => l.msg === 'email sent (resend)');
    expect(ok?.fields).toMatchObject({ to: 'player@example.com', template: 'verify_email', id: 'msg_1', attempt: 1 });
  });

  it('send() returns before the provider answers (no timing side channel on forgot-password)', async () => {
    let release!: () => void;
    const gate = new Promise<void>((r) => { release = r; });
    const { s, calls, lines } = sender([200], { gate });
    await s.send(message());
    expect(calls).toHaveLength(1);
    expect(lines.some((l) => l.msg === 'email sent (resend)')).toBe(false);   // still in flight
    release();
    await s.drain();
    expect(lines.some((l) => l.msg === 'email sent (resend)')).toBe(true);
  });

  it('retries 429, 5xx and network errors, then succeeds', async () => {
    const { s, calls, lines } = sender([429, 503, new TypeError('fetch failed'), 200]);
    await s.send(message());
    await s.drain();
    expect(calls).toHaveLength(4);
    expect(lines.find((l) => l.msg === 'email sent (resend)')?.fields).toMatchObject({ attempt: 4 });
    expect(lines.some((l) => l.level === 'error')).toBe(false);
  });

  it('a 4xx other than 429 is final and logged as an error', async () => {
    const { s, calls, lines } = sender([403, 200]);
    await s.send(message());
    await s.drain();
    expect(calls).toHaveLength(1);
    const err = lines.find((l) => l.msg === 'email NOT delivered (resend)');
    expect(err?.level).toBe('error');
    expect(String(err?.fields?.error)).toContain('HTTP 403');
  });

  it('gives up after the last retry', async () => {
    const { s, calls, lines } = sender([500, 500, 500, 500, 200]);
    await s.send(message());
    await s.drain();
    expect(calls).toHaveLength(4);
    expect(lines.some((l) => l.msg === 'email NOT delivered (resend)')).toBe(true);
  });

  it('never logs the API key, the token, the subject or the body', async () => {
    const { s, lines } = sender([500, 403]);
    await s.send(message());
    await s.drain();
    const { s: s2, lines: lines2 } = sender([200]);
    await s2.send(message());
    await s2.drain();
    const all = JSON.stringify([...lines, ...lines2]);
    expect(lines.length + lines2.length).toBeGreaterThan(0);
    for (const secret of [KEY, TOKEN, message().subject]) expect(all).not.toContain(secret);
  });

  it('hands the message to the record sender first (the outbox copy)', async () => {
    const record = new MemoryEmailSender();
    const { s } = sender([200], { record });
    await s.send(message());
    await s.drain();
    expect(record.sent).toHaveLength(1);
    expect(record.sent[0]?.template).toBe('verify_email');
  });
});

describe('EMAIL_DRIVER=resend config', () => {
  const base = { ENABLE_DEV_ROUTES: '1', EMAIL_DRIVER: 'resend' };
  it('needs RESEND_API_KEY', () => {
    expect(() => loadConfig({ ...base, EMAIL_FROM: 'Solar RP <no-reply@solar-rp.com>' })).toThrow(/RESEND_API_KEY is required/);
  });
  it('refuses the placeholder .invalid sender', () => {
    expect(() => loadConfig({ ...base, RESEND_API_KEY: KEY })).toThrow(/EMAIL_FROM must be an address/);
  });
  it('loads with a key and a real sender', () => {
    const c = loadConfig({ ...base, RESEND_API_KEY: KEY, EMAIL_FROM: 'Solar RP <no-reply@solar-rp.com>' });
    expect(c.emailDriver).toBe('resend');
    expect(c.resend).toEqual({ apiKey: KEY, apiBase: 'https://api.resend.com' });
  });
  it('other drivers carry no Resend config', () => {
    expect(loadConfig({ ENABLE_DEV_ROUTES: '1', RESEND_API_KEY: KEY }).resend).toBeNull();
  });
});
