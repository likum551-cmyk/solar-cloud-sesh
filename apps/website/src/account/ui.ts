// DOM glue shared by the account pages: field errors, the busy state of a submit, the password reveal
// and the live rules list. All text from the API goes in through textContent, never as markup.

import type { Problem } from './errors';
import { checkPasswordRules } from './password';
import type { TokenStore } from './store';

export const $ = <T extends Element>(sel: string, el: ParentNode = document): T | null => el.querySelector<T>(sel);
export const $$ = <T extends Element>(sel: string, el: ParentNode = document): T[] => Array.from(el.querySelectorAll<T>(sel));

/** The element `sel` must exist: a missing one is a broken page, said loudly. */
export function need<T extends Element>(sel: string, el: ParentNode = document): T {
  const found = el.querySelector<T>(sel);
  if (!found) throw new Error(`account page: missing ${sel}`);
  return found;
}

function fieldOf(form: HTMLFormElement, name: string) {
  const wrap = $<HTMLElement>(`.field[data-field="${name}"]`, form);
  if (!wrap) return null;
  return { wrap, input: need<HTMLInputElement>('input', wrap), error: need<HTMLElement>('.field__error', wrap) };
}

export function clearProblems(form: HTMLFormElement): void {
  for (const wrap of $$<HTMLElement>('.field', form)) {
    wrap.classList.remove('is-invalid');
    const input = $<HTMLInputElement>('input', wrap);
    input?.removeAttribute('aria-invalid');
    const error = $<HTMLElement>('.field__error', wrap);
    if (error) {
      error.textContent = '';
      error.hidden = true;
    }
  }
  const alert = $<HTMLElement>('.form__alert', form);
  if (alert) {
    alert.hidden = true;
    need<HTMLElement>('.form__alert-text', alert).textContent = '';
  }
}

/** Shows each problem under its field (or in the form's alert) and focuses the first one. */
export function showProblems(form: HTMLFormElement, problems: Problem[]): void {
  clearProblems(form);
  let first: HTMLElement | null = null;
  for (const p of problems) {
    const f = p.field ? fieldOf(form, p.field) : null;
    if (f) {
      f.wrap.classList.add('is-invalid');
      f.input.setAttribute('aria-invalid', 'true');
      f.error.textContent = p.text;
      f.error.hidden = false;
      first ??= f.input;
    } else {
      showAlert(form, p.text);
      first ??= $<HTMLElement>('.form__alert', form);
    }
  }
  first?.focus();
}

export function showAlert(form: HTMLFormElement, text: string): void {
  const alert = need<HTMLElement>('.form__alert', form);
  need<HTMLElement>('.form__alert-text', alert).textContent = text;
  alert.hidden = false;
}

/** Disables the submit while a request runs, and says what is happening on it. */
export function setBusy(button: HTMLButtonElement, busy: boolean, busyLabel: string): void {
  const label = $<HTMLElement>('.js-label', button) ?? button;
  if (busy) {
    button.dataset.idle = label.textContent ?? '';
    label.textContent = busyLabel;
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
  } else {
    if (button.dataset.idle !== undefined) label.textContent = button.dataset.idle;
    button.disabled = false;
    button.removeAttribute('aria-busy');
  }
}

/** "Show" / "Hide" buttons next to password inputs. */
export function wireReveal(root: ParentNode = document): void {
  for (const btn of $$<HTMLButtonElement>('.field__reveal', root)) {
    const id = btn.getAttribute('aria-controls');
    const input = id ? document.getElementById(id) : null;
    if (!(input instanceof HTMLInputElement)) throw new Error(`reveal button without its input (${id ?? 'no aria-controls'})`);
    btn.addEventListener('click', () => {
      const show = input.type === 'password';
      input.type = show ? 'text' : 'password';
      btn.textContent = show ? 'Hide' : 'Show';
      btn.setAttribute('aria-pressed', String(show));
      input.focus();
    });
  }
}

/** Keeps a `.rules` list in step with a password field. Rules stay neutral until something is typed. */
export function wireRules(list: HTMLElement, password: HTMLInputElement, ctx: () => { username?: string; email?: string }, also: HTMLInputElement[] = []): () => void {
  const update = () => {
    const pw = password.value;
    const r = checkPasswordRules(pw, ctx());
    const state: Record<string, boolean> = { length: r.long && r.notTooLong, username: r.noUsername, email: r.noEmailName };
    for (const li of $$<HTMLElement>('[data-rule]', list)) {
      const ok = state[li.dataset.rule ?? ''];
      li.dataset.state = pw.length === 0 ? 'idle' : ok ? 'ok' : 'fail';
    }
  };
  for (const el of [password, ...also]) el.addEventListener('input', update);
  update();
  return update;
}

/** The header's account link: "Log in" for visitors, "Account" once this browser holds a session;
 *  the footer swaps "Create account / Log in" for "Your account" the same way. */
export function wireAccountLink(store: TokenStore): void {
  const signedIn = store.get() !== null;
  for (const a of $$<HTMLAnchorElement>('.js-account-link')) {
    if (a.getAttribute('aria-current') === 'page') continue;
    a.href = signedIn ? '/account/' : '/login/';
    a.textContent = signedIn ? 'Account' : 'Log in';
  }
  for (const a of $$<HTMLElement>('.js-foot-guest')) a.hidden = signedIn;
  for (const a of $$<HTMLElement>('.js-foot-member')) a.hidden = !signedIn;
}
