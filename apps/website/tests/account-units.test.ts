import { describe, expect, it } from 'vitest';
import { describeUserAgent, formatDate, relativeTime, sessionLabel, websiteDeviceName } from '../src/account/format';
import { checkPasswordRules, firstPasswordProblem, looksLikeEmail, passwordLength } from '../src/account/password';
import { createTokenStore } from '../src/account/store';
import { PARTIALS } from '../partials';
import { memoryStorage, TOKEN, TOKEN_2 } from './account-helpers';

describe('password rules (mirror of the platform checkPassword, minus the common list)', () => {
  it('counts code points after NFKC, as the server does', () => {
    const grin = String.fromCodePoint(0x1f600); // two UTF-16 units, one code point
    expect(passwordLength(grin.repeat(10))).toBe(10);
    expect(firstPasswordProblem(grin.repeat(10))).toBeNull();
    const ff = String.fromCodePoint(0xfb00); // the "ff" ligature: NFKC makes it two letters
    expect(passwordLength(ff.repeat(5))).toBe(10);
    expect(firstPasswordProblem('a'.repeat(9))).toBe('too_short');
    expect(firstPasswordProblem('a'.repeat(10))).toBeNull();
    expect(firstPasswordProblem('a'.repeat(128))).toBeNull();
    expect(firstPasswordProblem('a'.repeat(129))).toBe('too_long');
  });

  it('refuses the username (3+ characters) and the email name (4+), case-insensitively', () => {
    const ctx = { username: 'Speedy_Gonzo', email: ' Speedy@Example.com ' };
    expect(firstPasswordProblem('xx-SPEEDY_gonzo-xx', ctx)).toBe('contains_username');
    expect(firstPasswordProblem('xx-SPEEDY-xx-long', ctx)).toBe('contains_email');
    expect(firstPasswordProblem('abc-0123456789', { username: 'ab' })).toBeNull();
    expect(firstPasswordProblem('bob-0123456789', { email: 'bob@example.com' })).toBeNull(); // 3-letter local part is not checked
    expect(firstPasswordProblem('bobby-0123456789', { email: 'bobby@example.com' })).toBe('contains_email');
  });

  it('reports each rule for the live list, length first in the server order', () => {
    expect(checkPasswordRules('short', { username: 'short' })).toEqual({ long: false, notTooLong: true, noUsername: false, noEmailName: true });
    expect(firstPasswordProblem('short', { username: 'short' })).toBe('too_short');
  });

  it('checks the email shape loosely', () => {
    expect(looksLikeEmail('speedy@example.com')).toBe(true);
    expect(looksLikeEmail(' speedy@example.com ')).toBe(true);
    expect(looksLikeEmail('speedy@example')).toBe(false);
    expect(looksLikeEmail('speedy example.com')).toBe(false);
  });
});

describe('token store', () => {
  it('keeps the token in sessionStorage by default and in localStorage when asked, never both', () => {
    const s = memoryStorage();
    const l = memoryStorage();
    const store = createTokenStore(s, l);
    store.set(TOKEN, false);
    expect([s.map.get('solar.session'), l.map.size]).toEqual([TOKEN, 0]);
    store.set(TOKEN_2, true);
    expect([s.map.size, l.map.get('solar.session')]).toEqual([0, TOKEN_2]);
    expect(store.get()).toBe(TOKEN_2);
    store.clear();
    expect(store.get()).toBeNull();
    expect(s.map.size + l.map.size).toBe(0);
  });

  it('ignores a stored value of the wrong shape and refuses to store one', () => {
    const s = memoryStorage();
    s.map.set('solar.session', 'not-a-token');
    const store = createTokenStore(s, memoryStorage());
    expect(store.get()).toBeNull();
    expect(() => store.set('Bearer x', false)).toThrow();
  });

  it('falls back to the other store when one refuses, and works with no storage at all', () => {
    const s = memoryStorage({ throwOnSet: true });
    const l = memoryStorage();
    const store = createTokenStore(s, l);
    store.set(TOKEN, false);
    expect(l.map.get('solar.session')).toBe(TOKEN);
    const none = createTokenStore(null, null);
    expect(none.get()).toBeNull();
    expect(() => none.clear()).not.toThrow();
  });
});

describe('formatting', () => {
  it('names browsers and the launcher', () => {
    expect(describeUserAgent('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36 Edg/130.0')).toBe('Edge on Windows');
    expect(describeUserAgent('Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1')).toBe('Safari on iOS');
    expect(describeUserAgent('Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0')).toBe('Firefox on Linux');
    expect(describeUserAgent('GAGE-MP-Launcher/0.2')).toBe('Solar Launcher');
    expect(describeUserAgent('curl/8.0')).toBeNull();
    expect(sessionLabel({ deviceName: null, userAgent: 'curl/8.0' })).toBe('Unknown device');
    expect(sessionLabel({ deviceName: 'Gaming PC', userAgent: null })).toBe('Gaming PC');
  });

  it('sends a device name the platform keeps whole (64 characters at most)', () => {
    expect(websiteDeviceName('Mozilla/5.0 (Windows NT 10.0) Chrome/130.0 Safari/537.36')).toBe('Website, Chrome on Windows');
    expect(websiteDeviceName('')).toBe('Website');
    expect(websiteDeviceName('x'.repeat(500)).length).toBeLessThanOrEqual(64);
  });

  it('writes times relative to now, then as a date', () => {
    const now = new Date('2026-11-06T12:00:00.000Z');
    expect(relativeTime('2026-11-06T11:59:30.000Z', now)).toBe('just now');
    expect(relativeTime('2026-11-06T11:15:00.000Z', now)).toBe('45 min ago');
    expect(relativeTime('2026-11-06T07:00:00.000Z', now)).toBe('5 h ago');
    expect(relativeTime('2026-11-05T11:00:00.000Z', now)).toBe('yesterday');
    expect(relativeTime('2026-11-02T12:00:00.000Z', now)).toBe('4 days ago');
    expect(relativeTime('2026-10-01T12:00:00.000Z', now)).toBe(formatDate('2026-10-01T12:00:00.000Z'));
    expect(formatDate('2026-10-01T12:00:00.000Z')).toMatch(/^(30 Sep|1 Oct|2 Oct) 2026$/); // the viewer's zone
    expect(formatDate('garbage')).toBe('');
  });
});

// The pages are static HTML wired by the scripts in src/account: these checks keep the two in step
// without a browser (the captures check the rest).
const pages = import.meta.glob(['../index.html', '../signup/index.html', '../login/index.html', '../account/index.html'], { query: '?raw', import: 'default', eager: true }) as Record<string, string>;

describe('page markup', () => {
  it('finds all four pages', () => {
    expect(Object.keys(pages).sort()).toEqual(['../account/index.html', '../index.html', '../login/index.html', '../signup/index.html']);
  });

  it('includes only partials that exist', () => {
    let seen = 0;
    for (const [file, html] of Object.entries(pages)) {
      for (const m of html.matchAll(/<!--\s*@include\s+([a-z]+)\s*-->/g)) {
        seen++;
        expect(Object.hasOwn(PARTIALS, m[1]!), `${file} includes ${m[1]}`).toBe(true);
      }
    }
    expect(seen).toBe(10); // head+bar+foot on three account pages, foot on the landing page
  });

  it.each([
    ['../signup/index.html', ['js-signup', 'id="su-username"', 'id="su-email"', 'id="su-password"', 'id="su-persist"', 'id="su-rules"', 'form__submit', 'js-not-signed-in', 'form__alert-text', 'aria-controls="su-password"', '/src/account/signup.ts']],
    ['../login/index.html', ['js-login', 'id="li-identifier"', 'id="li-password"', 'id="li-persist"', 'form__submit', 'form__alert-text', 'class="forgot"', '/src/account/login.ts']],
    ['../account/index.html', ['id="main"', 'js-username', 'js-since', 'js-p-username', 'js-p-email', 'js-email-state', 'js-email-note', 'js-ban', 'js-ban-text', 'js-sessions', 'js-sessions-count', 'js-revoke-others', 'js-sessions-status', 'js-password', 'id="pw-current"', 'id="pw-new"', 'id="pw-rules"', 'js-pw-done', 'js-pw-done-text', 'js-logout', 'js-logout-status', 'js-load-error', 'js-load-error-text', 'js-retry', 'id="h-password"', 'data-field="currentPassword"', 'js-discord', 'js-discord-linked', 'js-discord-name', 'js-discord-unlinked', 'js-discord-open', 'js-discord-form', 'id="dl-password"', 'aria-controls="dl-password"', 'js-discord-go', 'js-discord-cancel', 'js-discord-status', 'js-discord-status-text', '/src/account/account.ts']],
  ])('%s carries every element its script needs', (file, needles) => {
    const html = pages[file];
    expect(html, file).toBeDefined();
    for (const n of needles) expect(html, `${file}: ${n}`).toContain(n);
  });

  it('never promises email it cannot send, and offers no Discord login', () => {
    const all = Object.values(pages).join('\n') + Object.values(PARTIALS).join('\n');
    expect(all).not.toMatch(/check your (email|inbox)|we sent|verification (mail|email) (has been|was) sent/i);
    expect(all).not.toMatch(/(log|sign) in with discord/i);
    // Discord linking exists now: the page no longer says it does not
    expect(pages['../account/index.html']).not.toMatch(/linking isn.t available/i);
    expect(pages['../account/index.html']).toContain('Link Discord');
    expect(pages['../login/index.html']).toContain('Password reset is not available yet');
  });
});
