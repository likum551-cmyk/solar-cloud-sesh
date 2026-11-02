import { nativeWebView, type OutMsg } from './bridge';

// Every uncaught error, unhandled rejection and console.error in the UI goes to
// native as {"type":"log"}, which writes it to gage.log next to the exe, so a
// tester's log holds UI failures too. Capped per session so a render loop that
// throws every frame cannot flood the log.

const MAX_MESSAGES = 200;
const MAX_LENGTH = 2000;

function describe(value: unknown): string {
  if (value instanceof Error) return `${value.name}: ${value.message}${value.stack ? `\n${value.stack}` : ''}`;
  if (typeof value === 'string') return value;
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

/** Installs the forwarding once; a no-op outside the exe (no native bridge). */
export function installErrorLog(): void {
  const wv = nativeWebView();
  if (!wv) return;
  let sent = 0;
  const send = (level: 'error' | 'warn', message: string) => {
    if (sent >= MAX_MESSAGES) return;
    sent += 1;
    const text = sent === MAX_MESSAGES ? `${message} (further UI errors are not logged this session)` : message;
    const msg: OutMsg = { type: 'log', level, message: text.slice(0, MAX_LENGTH) };
    try {
      wv.postMessage(msg);
    } catch {
      // The bridge itself is gone (page unloading); nothing left to report to.
    }
  };

  window.addEventListener('error', (e) => {
    const where = e.filename ? ` (${e.filename}:${e.lineno}:${e.colno})` : '';
    send('error', `uncaught: ${e.error ? describe(e.error) : e.message}${where}`);
  });
  window.addEventListener('unhandledrejection', (e) => send('error', `unhandled rejection: ${describe(e.reason)}`));

  const original = console.error.bind(console);
  console.error = (...args: unknown[]) => {
    send('error', `console.error: ${args.map(describe).join(' ')}`);
    original(...args);
  };
}
