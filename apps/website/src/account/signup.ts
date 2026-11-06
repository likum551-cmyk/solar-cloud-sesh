import '../base.css';
import '../account.css';
import { createAccountApi } from './api';
import { signUp } from './flows';
import { websiteDeviceName } from './format';
import { browserTokenStore } from './store';
import { clearProblems, need, setBusy, showAlert, showProblems, wireAccountLink, wireReveal, wireRules } from './ui';

const api = createAccountApi();
const store = browserTokenStore();

// already signed in on this browser: the account page is where they want to be
if (store.get() !== null) location.replace('/account/');

wireAccountLink(store);
wireReveal();

const form = need<HTMLFormElement>('.js-signup');
const username = need<HTMLInputElement>('#su-username', form);
const email = need<HTMLInputElement>('#su-email', form);
const password = need<HTMLInputElement>('#su-password', form);
const persist = need<HTMLInputElement>('#su-persist', form);
const submit = need<HTMLButtonElement>('.form__submit', form);
wireRules(need<HTMLElement>('#su-rules', form), password, () => ({ username: username.value, email: email.value }), [username, email]);

const done = need<HTMLElement>('.js-not-signed-in');

form.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  if (submit.disabled) return;
  clearProblems(form);
  setBusy(submit, true, 'Creating account…');
  try {
    const out = await signUp(api, store, {
      username: username.value,
      email: email.value,
      password: password.value,
      persist: persist.checked,
      deviceName: websiteDeviceName(navigator.userAgent),
    });
    switch (out.kind) {
      case 'done':
        location.assign('/account/');
        return; // stays busy while the next page loads
      case 'invalid':
        showProblems(form, out.problems);
        break;
      case 'error':
        showProblems(form, [out.problem]);
        break;
      case 'not-signed-in':
        form.hidden = true;
        done.hidden = false;
        need<HTMLElement>('h2', done).focus();
        break;
      case 'signed-out':
        showAlert(form, 'Something went wrong. Reload the page and try again.');
        break;
    }
  } catch (err) {
    console.error('sign up failed', err);
    showAlert(form, 'Something went wrong in this page. Reload it and try again.');
  }
  setBusy(submit, false, '');
});
