import { useEffect, useRef, useState, type FormEvent } from 'react';
import { describeFailure, formatWait, isFailure } from '../lib/errors';
import { tokenFromPaste } from '../lib/links';
import { SIGNUP_URL } from '../lib/site';
import { useApp } from '../state/AppState';
import { isLocked } from '../state/auth';
import { EmblemSlot } from '../ui/emblemSlot';
import { useNow } from '../ui/hooks';
import { Chip, Field, Icon, Lockup, Plate } from '../ui/primitives';

// Signed out: log in, forgot / reset password and Discord. Accounts are created on the website
// only (lib/site.ts SIGNUP_URL, opened in the system browser), so there is one way to make one.
// The dusk reel fills the window; bottom left, the chrome emblem, the lockup and one line on what
// this is; on the right, one opaque panel holds the form.

export function AuthScreen() {
  const app = useApp();
  const { auth, authDispatch, api } = app;
  const now = useNow(1000);
  if (auth.status !== 'signedOut') return <AuthFrame title="Signing in" busy />;
  const locked = isLocked(auth, now);
  const fail = (e: unknown) => authDispatch({ type: 'FAIL', error: describeFailure(e), retryAfterSeconds: isFailure(e, 'rate_limited') ? e.retryAfterSeconds : null, now: Date.now() });
  const show = (view: typeof auth.view) => authDispatch({ type: 'SHOW', view });

  switch (auth.view) {
    case 'login':
      return (
        <LoginView
          demo={app.demo}
          busy={auth.busy}
          error={auth.error}
          reason={auth.reason}
          lockedFor={locked && auth.lockedUntil ? (auth.lockedUntil - now) / 1000 : 0}
          onSubmit={async (identifier, password) => {
            authDispatch({ type: 'SUBMIT', now: Date.now() });
            try {
              const r = await api.login({ identifier, password, deviceName: 'Solar Launcher' });
              await app.signIn(r.token, r.account);
            } catch (e) { fail(e); }
          }}
          onDiscord={() => startDiscord()}
          onForgot={() => show('forgot')}
          onSignup={() => app.openExternal(SIGNUP_URL)}
          onReset={() => show('reset')}
          onRetry={app.retryBoot}
        />
      );
    case 'forgot':
      return (
        <ForgotView
          busy={auth.busy}
          error={auth.error}
          onSubmit={async (email) => {
            authDispatch({ type: 'SUBMIT', now: Date.now() });
            try {
              await api.forgotPassword(email);
              authDispatch({ type: 'FORGOT_SENT', email });
            } catch (e) { fail(e); }
          }}
          onBack={() => show('login')}
          onHaveToken={() => show('reset')}
        />
      );
    case 'forgotSent':
      return (
        <AuthFrame title="Reset link sent">
          <p className="auth__lead">If <strong>{auth.email}</strong> has an account, a reset email is on its way. Open its link on this PC, or paste the link here.</p>
          <Plate tone="primary" size="lg" className="auth__submit" onClick={() => show('reset')}>I have the link <Icon name="arrow" /></Plate>
          <button type="button" className="auth__link" onClick={() => show('login')}>Back to log in</button>
        </AuthFrame>
      );
    case 'reset':
      return (
        <ResetView
          key={app.resetToken ?? ''}
          initialToken={app.resetToken ?? ''}
          busy={auth.busy}
          error={auth.error}
          onSubmit={async (token, password) => {
            authDispatch({ type: 'SUBMIT', now: Date.now() });
            try {
              await api.resetPassword(token, password);
              authDispatch({ type: 'RESET_DONE' });
            } catch (e) { fail(e); }
          }}
          onBack={() => show('login')}
        />
      );
    case 'resetDone':
      return (
        <AuthFrame title="Password changed">
          <p className="auth__lead">Your new password is set and every other device was signed out. Log in with it now.</p>
          <Plate tone="primary" size="lg" className="auth__submit" onClick={() => show('login')}>Log in <Icon name="arrow" /></Plate>
        </AuthFrame>
      );
    case 'discord':
      return <DiscordWait onCancel={() => show('login')} onError={fail} />;
    case 'discordUsername':
      return (
        <DiscordUsername
          suggested={auth.discord?.suggestedUsername ?? ''}
          busy={auth.busy}
          error={auth.error}
          onSubmit={async (username) => {
            if (!auth.discord) return;
            authDispatch({ type: 'SUBMIT', now: Date.now() });
            try {
              const r = await api.discordComplete(auth.discord.flowId, auth.discord.pollSecret, username);
              await app.signIn(r.token, r.account);
            } catch (e) { fail(e); }
          }}
          onCancel={() => show('login')}
        />
      );
  }

  async function startDiscord() {
    authDispatch({ type: 'SUBMIT', now: 0 });
    try {
      const flow = await api.discordStart();
      authDispatch({ type: 'DISCORD_STARTED', flowId: flow.flowId, pollSecret: flow.pollSecret });
      app.openExternal(flow.authorizeUrl);
    } catch (e) { fail(e); }
  }
}

/** The stable intro (emblem, lockup, one line) and the panel holding the current form. */
function AuthFrame({ title, children, busy }: { title: string; children?: React.ReactNode; busy?: boolean }) {
  return (
    <section className="auth" aria-busy={busy || undefined}>
      <div className="auth__intro">
        <EmblemSlot className="auth__emblem" />
        <div className="auth__brand">
          <Lockup height="clamp(48px, 4.85vw, 69px)" />
          <p className="auth__tagline">Multiplayer for GTA V</p>
        </div>
      </div>
      <div className="auth__panel">
        <h1 className="auth__title">{title}</h1>
        <div className="auth__body">{children}</div>
      </div>
    </section>
  );
}

function FormError({ error }: { error: string | null }) {
  return error ? <p className="auth__error" role="alert">{error}</p> : null;
}

// Demo mode only (no platform API yet): the mock platform accepts any password but "wrong".
const DEMO_LOGIN = { identifier: 'demo@solar-rp.com', password: 'demo-password' };

function LoginView(p: {
  demo: boolean; busy: boolean; error: string | null; reason: string | null; lockedFor: number;
  onSubmit: (identifier: string, password: string) => void; onDiscord: () => void; onForgot: () => void; onSignup: () => void; onReset: () => void; onRetry: () => void;
}) {
  const [identifier, setIdentifier] = useState(p.demo ? DEMO_LOGIN.identifier : '');
  const [password, setPassword] = useState(p.demo ? DEMO_LOGIN.password : '');
  // After "Create an account" the browser has the sign-up page; this frame says where to come back.
  const [signupOpened, setSignupOpened] = useState(false);
  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (!identifier.trim() || !password) return;
    p.onSubmit(identifier.trim(), password);
  };
  const locked = p.lockedFor > 0;
  return (
    <AuthFrame title="Log in">
      {p.reason && <p className="auth__notice">{p.reason}</p>}
      {/* Mounted from the start, so the sentence that lands in it is announced. On screen the
          link row itself says what happened (no reflow of the centred panel under the cursor). */}
      <div className="sr-only" role="status">
        {signupOpened ? 'Opening the sign-up page on solar-rp.com in your browser. Create your account there, then log in here.' : ''}
      </div>
      <form className="auth__form" onSubmit={submit} noValidate>
        <Field label="Email or username" autoComplete="username" value={identifier} onChange={(e) => setIdentifier(e.target.value)} autoFocus spellCheck={false} />
        <Field label="Password" type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} />
        {locked ? (
          <p className="auth__error auth__error--lock" role="alert">
            Locked after too many attempts. Try again in {formatWait(p.lockedFor).toLowerCase()}.
          </p>
        ) : (
          <FormError error={p.error} />
        )}
        {/* LOG IN is the form's one gold submit; demo entry sits under it as a quiet option. */}
        <Plate tone="primary" size="lg" type="submit" className="auth__submit" busy={p.busy} disabled={locked || !identifier.trim() || !password}>
          {p.busy ? 'Logging in' : 'Log in'} <Icon name="arrow" />
        </Plate>
      </form>
      <div className="auth__or"><span>or</span></div>
      {p.demo && (
        <Plate tone="ghost" size="md" className="auth__demo" busy={p.busy} onClick={() => p.onSubmit(DEMO_LOGIN.identifier, DEMO_LOGIN.password)}>
          Enter with the demo account <Chip tone="demo">Demo</Chip>
        </Plate>
      )}
      <Plate tone="ghost" size="md" className="auth__discord" onClick={p.onDiscord} disabled={p.busy}>Log in with Discord <Icon name="external" /></Plate>
      <div className="auth__links">
        <button type="button" className="auth__link" onClick={p.onForgot}>Forgot password</button>
        <button type="button" className="auth__link" onClick={() => { setSignupOpened(true); p.onSignup(); }}>
          {signupOpened ? 'Sent to your browser · Open again' : 'Create an account'} <Icon name="external" size={14} />
        </button>
      </div>
      {p.error && /reach|answer in time|network address/.test(p.error) && (
        <button type="button" className="auth__link auth__retry" onClick={p.onRetry}><Icon name="refresh" /> Try the network again</button>
      )}
    </AuthFrame>
  );
}

function ForgotView(p: { busy: boolean; error: string | null; onSubmit: (email: string) => void; onBack: () => void; onHaveToken: () => void }) {
  const [email, setEmail] = useState('');
  const ok = /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email.trim());
  return (
    <AuthFrame title="Reset password">
      <p className="auth__lead">Enter the email on your account. We send a reset code there.</p>
      <form className="auth__form" onSubmit={(e) => { e.preventDefault(); if (ok) p.onSubmit(email.trim()); }} noValidate>
        <Field label="Email" type="email" autoComplete="email" value={email} onChange={(e) => setEmail(e.target.value)} autoFocus spellCheck={false} />
        <FormError error={p.error} />
        <Plate tone="primary" size="lg" type="submit" className="auth__submit" busy={p.busy} disabled={!ok}>Send reset code <Icon name="arrow" /></Plate>
      </form>
      <div className="auth__links">
        <button type="button" className="auth__link" onClick={p.onBack}><Icon name="back" /> Back to log in</button>
        <button type="button" className="auth__link" onClick={p.onHaveToken}>I already have a link</button>
      </div>
    </AuthFrame>
  );
}

function ResetView(p: { initialToken: string; busy: boolean; error: string | null; onSubmit: (token: string, password: string) => void; onBack: () => void }) {
  const [token, setToken] = useState(p.initialToken);
  const [password, setPassword] = useState('');
  // The field takes the whole emailed link or just its token.
  const parsed = tokenFromPaste(token, 'resetPassword');
  const wrongLink = token.trim().length > 0 && parsed === null;
  const ok = parsed !== null && password.length >= 10;
  return (
    <AuthFrame title="New password">
      <form className="auth__form" onSubmit={(e) => { e.preventDefault(); if (ok && parsed) p.onSubmit(parsed, password); }} noValidate>
        <Field
          label="Reset link from the email"
          value={token}
          onChange={(e) => setToken(e.target.value)}
          autoFocus={!p.initialToken}
          spellCheck={false}
          autoComplete="off"
          error={wrongLink && token.trim().length >= 16 ? 'That is not a password reset link.' : null}
          hint={p.initialToken ? 'Filled in from the link you opened.' : 'Paste the whole link.'}
        />
        <Field label="New password" type="password" autoComplete="new-password" value={password} onChange={(e) => setPassword(e.target.value)} hint="At least 10 characters." />
        <FormError error={p.error} />
        <Plate tone="primary" size="lg" type="submit" className="auth__submit" busy={p.busy} disabled={!ok}>Set password <Icon name="arrow" /></Plate>
      </form>
      <div className="auth__links"><button type="button" className="auth__link" onClick={p.onBack}><Icon name="back" /> Back to log in</button></div>
    </AuthFrame>
  );
}

/** Polls the Discord flow while the browser login runs; a `discordCallback` bridge event polls at once. */
function DiscordWait({ onCancel, onError }: { onCancel: () => void; onError: (e: unknown) => void }) {
  const app = useApp();
  const { auth, api, authDispatch, bridge } = app;
  const flow = auth.status === 'signedOut' ? auth.discord : null;
  const busy = useRef(false);
  useEffect(() => {
    if (!flow) return;
    let stopped = false;
    const poll = async () => {
      if (busy.current || stopped) return;
      busy.current = true;
      try {
        const r = await api.discordPoll(flow.flowId, flow.pollSecret);
        if (stopped) return;
        if (r.status === 'complete') { stopped = true; await app.signIn(r.token, r.account); }
        else if (r.status === 'needs_username') { stopped = true; authDispatch({ type: 'DISCORD_NEEDS_USERNAME', suggestedUsername: r.suggestedUsername }); }
        else if (r.status === 'failed') { stopped = true; authDispatch({ type: 'SHOW', view: 'login' }); authDispatch({ type: 'FAIL', error: `Discord login did not finish (${r.code}).`, now: Date.now() }); }
      } catch (e) {
        if (!isFailure(e, 'offline', 'timeout')) { stopped = true; authDispatch({ type: 'SHOW', view: 'login' }); onError(e); }
      } finally {
        busy.current = false;
      }
    };
    const t = setInterval(() => void poll(), 2000);
    const off = bridge.on((m) => { if (m.type === 'discordCallback') void poll(); });
    return () => { stopped = true; clearInterval(t); off(); };
  }, [flow, api, app, authDispatch, bridge, onError]);
  return (
    <AuthFrame title="Finish in your browser">
      <p className="auth__lead">We opened Discord in your browser. Approve Solar RP there and this frame logs you in by itself.</p>
      <div className="auth__waiting" aria-live="polite"><span className="auth__pulse" aria-hidden /> Waiting for Discord</div>
      <div className="auth__links">
        <button type="button" className="auth__link" onClick={onCancel}><Icon name="back" /> Cancel</button>
      </div>
    </AuthFrame>
  );
}

function DiscordUsername(p: { suggested: string; busy: boolean; error: string | null; onSubmit: (u: string) => void; onCancel: () => void }) {
  const [username, setUsername] = useState(p.suggested);
  const ok = /^[A-Za-z0-9_]{3,20}$/.test(username);
  return (
    <AuthFrame title="Pick a username">
      <p className="auth__lead">Your Discord account is new here. Choose the name other players see.</p>
      <form className="auth__form" onSubmit={(e) => { e.preventDefault(); if (ok) p.onSubmit(username); }} noValidate>
        <Field label="Username" value={username} onChange={(e) => setUsername(e.target.value)} autoFocus spellCheck={false} error={username && !ok ? '3 to 20 letters, digits or underscore.' : null} />
        <FormError error={p.error} />
        <Plate tone="primary" size="lg" type="submit" className="auth__submit" busy={p.busy} disabled={!ok}>Finish <Icon name="arrow" /></Plate>
      </form>
      <div className="auth__links"><button type="button" className="auth__link" onClick={p.onCancel}><Icon name="back" /> Cancel</button></div>
    </AuthFrame>
  );
}
