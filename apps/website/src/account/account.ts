import '../base.css';
import '../account.css';
import { createAccountApi, type Account, type SessionInfo } from './api';
import { readReturn, withoutFragment } from './discord';
import { mountDiscord } from './discord-ui';
import { changePassword, loadAccount, logOut, revokeOtherSessions, revokeSession } from './flows';
import { formatDate, formatDateTime, relativeTime, sessionLabel } from './format';
import { mountQuests } from './quests-ui';
import { browserTokenStore } from './store';
import { $, $$, clearProblems, need, setBusy, showProblems, wireAccountLink, wireReveal, wireRules } from './ui';

// Back from Discord: the flow and claim ride in the fragment (never sent to a server). Read them once and
// take them out of the address bar at once, so they are not bookmarked, shared or kept in history.
const discordBack = readReturn(location.hash);
if (location.hash !== '') history.replaceState(history.state, '', withoutFragment(location.href));

const api = createAccountApi();
const store = browserTokenStore();
const main = need<HTMLElement>('#main');

const goLogin = () => location.replace('/login/');
if (store.get() === null) goLogin();

wireAccountLink(store);
// this page IS the account link's target: name it so and mark it current
for (const a of document.querySelectorAll<HTMLAnchorElement>('.js-account-link, .js-foot-member')) {
  a.href = '/account/';
  a.textContent = 'Account';
  a.setAttribute('aria-current', 'page');
}

let account: Account | null = null;

/* ---------------------------------------------------------------- profile */

function renderAccount(a: Account) {
  account = a;
  need<HTMLElement>('.js-username').textContent = a.username;
  // the name is sized to fit one line at any width (account.css reads --len)
  need<HTMLElement>('.acct__name').style.setProperty('--len', String(Math.max(6, a.username.length)));
  const since = formatDate(a.createdAt);
  need<HTMLElement>('.js-since').textContent = since ? `Member since ${since}` : '';
  need<HTMLElement>('.js-p-username').textContent = a.username;
  need<HTMLElement>('.js-p-email').textContent = a.email ?? 'No email address';
  const chip = need<HTMLElement>('.js-email-state');
  chip.hidden = a.email === null;
  chip.textContent = a.emailVerified ? 'Verified' : 'Not verified';
  chip.classList.toggle('chip--ok', a.emailVerified);
  need<HTMLElement>('.js-email-note').hidden = a.email === null || a.emailVerified;

  const ban = need<HTMLElement>('.js-ban');
  ban.hidden = a.ban === null;
  if (a.ban) {
    const until = a.ban.expiresAt ? `until ${formatDateTime(a.ban.expiresAt)}` : 'with no end date';
    need<HTMLElement>('.js-ban-text', ban).textContent = `This account is banned ${until}. Reason: ${a.ban.reason}`;
  }

  // the hidden username fields let a password manager file (and fill) the password under the right account
  for (const pmUser of $$<HTMLInputElement>('form input[name="username"].visually-hidden')) pmUser.value = a.username;
  discord.render(a.discord);
  // an account without a password (Discord-only) sets its first one; the current-password field goes
  need<HTMLElement>('.field[data-field="currentPassword"]').hidden = !a.hasPassword;
  need<HTMLElement>('#h-password').textContent = a.hasPassword ? 'Change password' : 'Set a password';
}

/* ---------------------------------------------------------------- sessions */

const list = need<HTMLUListElement>('.js-sessions');
const othersBtn = need<HTMLButtonElement>('.js-revoke-others');
const sessionsStatus = need<HTMLElement>('.js-sessions-status');

function sessionRow(s: SessionInfo, now: Date): HTMLLIElement {
  const li = document.createElement('li');
  li.className = 'session';
  if (s.current) li.classList.add('session--current');

  const text = document.createElement('div');
  text.className = 'session__text';
  const name = document.createElement('p');
  name.className = 'session__name';
  name.textContent = sessionLabel(s);
  if (s.current) {
    const tag = document.createElement('span');
    tag.className = 'chip chip--ok';
    tag.textContent = 'This browser';
    name.append(' ', tag);
  }
  const meta = document.createElement('p');
  meta.className = 'session__meta';
  const seen = document.createElement('time');
  seen.dateTime = s.lastSeenAt;
  seen.title = formatDateTime(s.lastSeenAt);
  seen.textContent = s.current ? 'Active now' : `Last active ${relativeTime(s.lastSeenAt, now)}`;
  meta.append(seen);
  const bits = [s.ip ? `IP ${s.ip}` : '', `signed in ${formatDate(s.createdAt)}`].filter(Boolean);
  meta.append(` · ${bits.join(' · ')}`);
  text.append(name, meta);
  li.append(text);

  if (!s.current) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'plate plate--ghost plate--xs';
    btn.innerHTML = '<span class="js-label">Sign out</span>';
    btn.setAttribute('aria-label', `Sign out ${sessionLabel(s)}`);
    btn.addEventListener('click', async () => {
      setBusy(btn, true, 'Signing out…');
      const out = await revokeSession(api, store, s.id);
      if (out.kind === 'signed-out') return goLogin();
      if (out.kind === 'error') {
        sessionsStatus.textContent = out.problem.text;
        setBusy(btn, false, '');
        return;
      }
      sessionsStatus.textContent = `${sessionLabel(s)} is signed out.`;
      await refresh();
    });
    li.append(btn);
  }
  return li;
}

function renderSessions(sessions: SessionInfo[]) {
  const now = new Date();
  const sorted = [...sessions].sort((a, b) => Number(b.current) - Number(a.current) || b.lastSeenAt.localeCompare(a.lastSeenAt));
  list.replaceChildren(...sorted.map((s) => sessionRow(s, now)));
  list.removeAttribute('aria-busy');
  const others = sessions.filter((s) => !s.current).length;
  othersBtn.hidden = others === 0;
  need<HTMLElement>('.js-sessions-count').textContent = others === 0 ? 'Only this browser is signed in.' : `This browser and ${others} other${others === 1 ? '' : 's'}.`;
}

othersBtn.addEventListener('click', async () => {
  setBusy(othersBtn, true, 'Signing out…');
  const out = await revokeOtherSessions(api, store);
  setBusy(othersBtn, false, '');
  if (out.kind === 'signed-out') return goLogin();
  if (out.kind === 'error') {
    sessionsStatus.textContent = out.problem.text;
    return;
  }
  sessionsStatus.textContent = out.revoked === 1 ? '1 other session signed out.' : `${out.revoked} other sessions signed out.`;
  await refresh();
});

/* ---------------------------------------------------------------- password */

wireReveal();
const pwForm = need<HTMLFormElement>('.js-password');
const current = need<HTMLInputElement>('#pw-current', pwForm);
const next = need<HTMLInputElement>('#pw-new', pwForm);
const pwSubmit = need<HTMLButtonElement>('.form__submit', pwForm);
const pwDone = need<HTMLElement>('.js-pw-done');
wireRules(need<HTMLElement>('#pw-rules', pwForm), next, () => ({ username: account?.username, email: account?.email ?? undefined }));

pwForm.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  if (!account || pwSubmit.disabled) return;
  clearProblems(pwForm);
  pwDone.hidden = true;
  setBusy(pwSubmit, true, 'Saving…');
  const out = await changePassword(api, store, { currentPassword: current.value, newPassword: next.value, account });
  setBusy(pwSubmit, false, '');
  if (out.kind === 'signed-out') return goLogin();
  if (out.kind === 'invalid') return showProblems(pwForm, out.problems);
  if (out.kind === 'error') return showProblems(pwForm, [out.problem]);
  pwForm.reset();
  next.dispatchEvent(new Event('input'));
  const n = out.revokedSessions;
  need<HTMLElement>('.js-pw-done-text').textContent =
    n === 0 ? 'Password changed.' : `Password changed. ${n === 1 ? '1 other session was' : `${n} other sessions were`} signed out.`;
  pwDone.hidden = false;
  account = { ...account, hasPassword: true };
  await refresh();
});

/* ---------------------------------------------------------------- log out */

const logoutBtn = need<HTMLButtonElement>('.js-logout');
const logoutStatus = need<HTMLElement>('.js-logout-status');
logoutBtn.addEventListener('click', async () => {
  setBusy(logoutBtn, true, 'Logging out…');
  const out = await logOut(api, store);
  if (out.kind === 'signed-out') return location.assign('/');
  setBusy(logoutBtn, false, '');
  logoutStatus.textContent = out.kind === 'error' ? `You’re still signed in: ${out.problem.text}` : '';
});

/* ---------------------------------------------------------------- load */

const loadError = need<HTMLElement>('.js-load-error');
async function refresh(): Promise<void> {
  const out = await loadAccount(api, store);
  if (out.kind === 'signed-out') return goLogin();
  if (out.kind === 'error') {
    if (account === null) {
      main.dataset.state = 'error';
      need<HTMLElement>('.js-load-error-text', loadError).textContent = out.problem.text;
      loadError.hidden = false;
    } else sessionsStatus.textContent = out.problem.text;
    return;
  }
  loadError.hidden = true;
  renderAccount(out.account);
  renderSessions(out.sessions);
  main.dataset.state = 'ready';
  main.removeAttribute('aria-busy');
}

/* ---------------------------------------------------------------- quests and Discord */

const discord = mountDiscord(api, store, goLogin, async () => {
  await Promise.all([refresh(), quests.refresh()]);
});
const quests = mountQuests(api, store, goLogin, () => discord.open());

$<HTMLButtonElement>('.js-retry')?.addEventListener('click', () => {
  main.dataset.state = 'loading';
  loadError.hidden = true;
  void refresh();
  void quests.refresh();
});

if (store.get() !== null) {
  void refresh();
  void quests.refresh();
  // a link this tab started comes back here: finish it, then redraw the account and the quests it unlocks
  if (discordBack) void discord.finish(discordBack);
}
