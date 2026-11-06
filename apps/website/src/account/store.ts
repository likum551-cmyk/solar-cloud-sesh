// Where the session token lives in the browser.
//
// The platform's login answers with an opaque bearer token (`gms_...`), valid for 30 days of inactivity
// (180 days at most), and the API docs make no storage recommendation. The choice here:
//   - "Stay signed in on this computer" unchecked (the default): sessionStorage. The token dies with the
//     tab, so a shared or café PC never keeps a live session for the next person.
//   - checked: localStorage, so a player's own PC keeps ONE session across visits instead of opening a
//     new one each time (each login is a new row in the account's Sessions list).
// No cookies: the platform takes bearer tokens only, and the site's CSP (script-src 'self', no inline
// scripts) is the guard against a script reading either store. Logging out clears both.

const KEY = 'solar.session';
const SHAPE = /^gms_[A-Za-z0-9_-]{20,200}$/;

export interface TokenStore {
  get(): string | null;
  set(token: string, persist: boolean): void;
  clear(): void;
}

type KV = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

function safeGet(s: KV | null): string | null {
  try {
    return s?.getItem(KEY) ?? null;
  } catch {
    return null; // storage disabled (privacy mode, blocked site data)
  }
}
function safeRemove(s: KV | null): void {
  try {
    s?.removeItem(KEY);
  } catch {
    /* nothing stored where nothing can be stored */
  }
}

export function createTokenStore(session: KV | null, local: KV | null): TokenStore {
  return {
    get() {
      for (const s of [session, local]) {
        const t = safeGet(s);
        if (t !== null && SHAPE.test(t)) return t;
      }
      return null;
    },
    set(token, persist) {
      if (!SHAPE.test(token)) throw new Error('refusing to store a token of the wrong shape');
      safeRemove(session);
      safeRemove(local);
      const target = persist ? local : session;
      try {
        target?.setItem(KEY, token);
      } catch {
        // Storage refused: the session works for this page only. Fall back to the other store once.
        const other = persist ? session : local;
        try {
          other?.setItem(KEY, token);
        } catch {
          throw new Error('this browser does not allow the site to remember the sign-in');
        }
      }
    },
    clear() {
      safeRemove(session);
      safeRemove(local);
    },
  };
}

function storage(name: 'sessionStorage' | 'localStorage'): KV | null {
  try {
    return window[name];
  } catch {
    return null;
  }
}

/** The page's own stores. */
export function browserTokenStore(): TokenStore {
  return createTokenStore(storage('sessionStorage'), storage('localStorage'));
}
