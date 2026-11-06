// A scripted fetch for the account tests: each call takes the next answer from a queue (or throws, to
// simulate no network) and is recorded with its method, URL, headers and parsed JSON body.

export interface Call {
  method: string;
  url: string;
  headers: Record<string, string>;
  body: unknown;
}

export type Answer =
  | { status: number; json?: unknown; text?: string; headers?: Record<string, string> }
  | 'network-down';

export function scriptedFetch(answers: Answer[]) {
  const calls: Call[] = [];
  const queue = [...answers];
  const fetchImpl = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const headers: Record<string, string> = {};
    new Headers(init?.headers).forEach((v, k) => { headers[k] = v; });
    const raw = typeof init?.body === 'string' ? init.body : undefined;
    calls.push({ method: init?.method ?? 'GET', url: String(input), headers, body: raw === undefined ? undefined : JSON.parse(raw) });
    const a = queue.shift();
    if (a === undefined) throw new Error(`unexpected request ${init?.method ?? 'GET'} ${String(input)}`);
    if (a === 'network-down') throw new TypeError('Failed to fetch');
    const body = a.text ?? (a.json === undefined ? null : JSON.stringify(a.json));
    return new Response(a.status === 204 ? null : body, { status: a.status, headers: a.headers });
  }) as typeof fetch;
  return { fetchImpl, calls, remaining: () => queue.length };
}

export function memoryStorage(opts: { throwOnSet?: boolean } = {}) {
  const m = new Map<string, string>();
  return {
    map: m,
    getItem: (k: string) => m.get(k) ?? null,
    setItem: (k: string, v: string) => {
      if (opts.throwOnSet) throw new Error('QuotaExceededError');
      m.set(k, v);
    },
    removeItem: (k: string) => { m.delete(k); },
  };
}

export const TOKEN = 'gms_Qm9uZUpvdXJub3NoZ2FtZXBsYXRmb3JtMDEyMzQ1Ng';
export const TOKEN_2 = 'gms_c2Vjb25kX3Rva2VuX2Zvcl90aGVfdGVzdHNfMDEyMw';

export const ACCOUNT = {
  id: '7c9e6679-7425-40de-944b-e07fc1f90ae7',
  username: 'Speedy_Gonzo',
  email: 'speedy@example.com',
  emailVerified: false,
  roles: ['player'],
  hasPassword: true,
  discord: null,
  ban: null,
  createdAt: '2026-10-01T09:30:00.000Z',
};

export const SESSION = {
  id: '3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33',
  deviceName: 'Website, Chrome on Windows',
  userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36',
  ip: '203.0.113.50',
  createdAt: '2026-10-29T12:00:00.000Z',
  lastSeenAt: '2026-10-29T12:00:00.000Z',
  expiresAt: '2026-11-28T12:00:00.000Z',
  current: true,
};

/** The platform's documented error body. */
export function apiError(status: number, code: string, details?: unknown, headers?: Record<string, string>): Answer {
  return { status, json: { error: { code, message: `dev message for ${code}`, requestId: 'req-0123456789', ...(details === undefined ? {} : { details }) } }, headers };
}
