import '../base.css';
import '../account.css';
import { createAccountApi } from './api';
import { logIn } from './flows';
import { websiteDeviceName } from './format';
import { browserTokenStore } from './store';
import { clearProblems, need, setBusy, showAlert, showProblems, wireAccountLink, wireReveal } from './ui';

const api = createAccountApi();
const store = browserTokenStore();

if (store.get() !== null) location.replace('/account/');

wireAccountLink(store);
wireReveal();

const form = need<HTMLFormElement>('.js-login');
const identifier = need<HTMLInputElement>('#li-identifier', form);
const password = need<HTMLInputElement>('#li-password', form);
const persist = need<HTMLInputElement>('#li-persist', form);
const submit = need<HTMLButtonElement>('.form__submit', form);

form.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  if (submit.disabled) return;
  clearProblems(form);
  setBusy(submit, true, 'Logging in…');
  try {
    const out = await logIn(api, store, {
      identifier: identifier.value,
      password: password.value,
      persist: persist.checked,
      deviceName: websiteDeviceName(navigator.userAgent),
    });
    if (out.kind === 'done') {
      location.assign('/account/');
      return;
    }
    if (out.kind === 'invalid') showProblems(form, out.problems);
    else if (out.kind === 'error') {
      // the alert is announced (role=alert); the cursor goes back to an empty password for the retry
      showProblems(form, [out.problem]);
      password.value = '';
      password.focus();
    } else showAlert(form, 'Something went wrong. Reload the page and try again.');
  } catch (err) {
    console.error('log in failed', err);
    showAlert(form, 'Something went wrong in this page. Reload it and try again.');
  }
  setBusy(submit, false, '');
});
