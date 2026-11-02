import { useCallback, useEffect, useRef, useState } from 'react';
import { updateBusy, updateStatusText } from '../lib/bridge';
import { describeFailure } from '../lib/errors';
import { charProblem, MAX_IN_GAME_NAME, nameProblem, normalizeInGameName } from '../lib/names';
import type { SessionRow } from '../lib/schemas';
import { useApp } from '../state/AppState';
import { useLenis } from '../ui/useLenis';
import { Chip, Code, Field, Icon, Plate, Toggle } from '../ui/primitives';

// Settings: local game and client settings (native owns them; every change is one
// `saveSetting`), the account, signed-in devices, and sign-out kept apart at the end.
// Same grammar as the other screens: the GTA V still holds the left of the window with the
// title and a section index over it, and the sections scroll in the opaque right-hand panel.

/** Keeps a number and its unit on one line ("3 s"). Built in code so no invisible character sits in the source. */
const NBSP = String.fromCharCode(0xa0);

const SECTIONS = [
  { id: 's-game', label: 'Game' },
  { id: 's-client', label: 'Client' },
  { id: 's-account', label: 'Account' },
  { id: 's-devices', label: 'Devices' },
  { id: 's-exit', label: 'Sign out' },
] as const;

export function SettingsScreen() {
  const { settings, saveSetting, bridge, auth, signOut, demo } = useApp();
  const lenisRef = useLenis<HTMLDivElement>();
  const [panel, setPanel] = useState<HTMLDivElement | null>(null);
  const scroller = useCallback((el: HTMLDivElement | null) => { lenisRef(el); setPanel(el); }, [lenisRef]);
  const [current, setCurrent] = useState<string>(SECTIONS[0].id);
  const [launchOptions, setLaunchOptions] = useState(settings?.launchOptions ?? '');
  const [update, setUpdate] = useState<{ phase: 'idle' | 'checking' | 'done'; text?: string; busy?: boolean }>({ phase: 'idle' });
  useEffect(() => { setLaunchOptions(settings?.launchOptions ?? ''); }, [settings?.launchOptions]);
  useEffect(() => bridge.on((m) => {
    if (m.type === 'update') setUpdate({ phase: 'done', text: updateStatusText(m), busy: updateBusy(m) });
  }), [bridge]);

  // The index follows the frame whose heading is nearest the top of the panel. A frame picked
  // from the index stays current until the reader scrolls by hand (the last frames cannot reach
  // the top, so the position alone would name the wrong one).
  const picked = useRef<string | null>(null);
  useEffect(() => {
    if (!panel) return;
    const pick = () => {
      if (picked.current) { setCurrent(picked.current); return; }
      const top = panel.getBoundingClientRect().top;
      let best: string = SECTIONS[0].id;
      for (const s of SECTIONS) {
        const el = document.getElementById(s.id);
        if (el && el.getBoundingClientRect().top - top < 120) best = s.id;
      }
      // Scrolled to the end: the last frame is the one being read.
      if (panel.scrollTop + panel.clientHeight >= panel.scrollHeight - 4) best = SECTIONS.at(-1)?.id ?? best;
      setCurrent(best);
    };
    const release = () => { picked.current = null; };
    pick();
    panel.addEventListener('scroll', pick, { passive: true });
    panel.addEventListener('wheel', release, { passive: true });
    panel.addEventListener('pointerdown', release);
    panel.addEventListener('keydown', release);
    return () => {
      panel.removeEventListener('scroll', pick);
      panel.removeEventListener('wheel', release);
      panel.removeEventListener('pointerdown', release);
      panel.removeEventListener('keydown', release);
    };
  }, [panel]);

  const goTo = (id: string) => {
    const el = document.getElementById(id)?.closest('section');
    if (!panel || !el) return;
    picked.current = id;
    setCurrent(id);
    const top = el.getBoundingClientRect().top - panel.getBoundingClientRect().top + panel.scrollTop - 28;
    panel.scrollTo({ top, behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
  };

  if (auth.status !== 'signedIn') return null;
  return (
    <div className="settings">
      <aside className="settings__side">
        <h1 className="settings__title">Settings</h1>
        <nav className="settings__index" aria-label="Settings sections">
          {SECTIONS.map((s) => (
            <button key={s.id} type="button" className={`settings__jump ${current === s.id ? 'is-current' : ''}`} aria-current={current === s.id ? 'true' : undefined} onClick={() => goTo(s.id)}>
              <span>{s.label}</span>
            </button>
          ))}
        </nav>
      </aside>
      <div className="settings__panel" ref={scroller}>
        <div className="settings__inner">
          <section className="sframe" aria-labelledby="s-game">
            <header className="sframe__head"><h2 id="s-game">Game</h2></header>
            <div className="sframe__row">
              <div className="path">
                <span className="field__label">GTA V folder</span>
                <span className={`path__value ${settings?.gamePath ? '' : 'is-empty'}`}>{settings?.gamePath || 'Not found automatically. Choose the folder that holds GTA5.exe.'}</span>
              </div>
              <Plate tone="ghost" onClick={() => bridge.send({ type: 'browseGamePath' })}><Icon name="folder" /> Choose folder</Plate>
            </div>
            {demo && <InGameName />}
            <Field
              label="Launch options"
              value={launchOptions}
              placeholder="e.g. -windowed -noborder"
              onChange={(e) => setLaunchOptions(e.target.value)}
              onBlur={() => { if (launchOptions !== (settings?.launchOptions ?? '')) saveSetting('launchOptions', launchOptions.trim()); }}
              hint="Passed to GTA V on start. Saved when you leave the field."
              spellCheck={false}
            />
          </section>
  
          <section className="sframe" aria-labelledby="s-client">
            <header className="sframe__head"><h2 id="s-client">Client</h2></header>
            <label className="select">
              <span className="field__label">Language</span>
              <select value={settings?.language ?? 'en'} onChange={(e) => saveSetting('language', e.target.value)}>
                <option value="en">English</option>
              </select>
              <span className="field__hint">More languages arrive with the platform translations.</span>
            </label>
            <Toggle label="Start minimized" hint="Open in the tray when Windows starts Solar Launcher." checked={settings?.startMinimized ?? false} onChange={(v) => saveSetting('startMinimized', v)} />
            <Toggle label="Close the game when I quit Solar Launcher" hint={`Only a game Solar Launcher started. It is asked to close, then stopped after 3${NBSP}s.`} checked={settings?.closeGame ?? false} onChange={(v) => saveSetting('closeGame', v)} />
            <Toggle label="Install updates automatically" checked={settings?.autoUpdate ?? true} onChange={(v) => saveSetting('autoUpdate', v)} />
            <div className="sframe__row">
              <Code>Version {settings?.version || '—'}</Code>
              <Plate tone="quiet" size="sm" busy={update.phase === 'checking' || update.busy} onClick={() => { setUpdate({ phase: 'checking' }); bridge.send({ type: 'checkUpdate' }); }}>
                <Icon name="refresh" /> Check for updates
              </Plate>
            </div>
            {update.text && <p className="sframe__note" role="status">{update.text}</p>}
          </section>
  
          <AccountFrame />
          <DevicesFrame />
  
          <section className="sframe sframe--exit" aria-labelledby="s-exit">
            <header className="sframe__head"><h2 id="s-exit">Sign out</h2></header>
            <SignOut onSignOut={() => void signOut(null, true)} onQuit={() => bridge.send({ type: 'quit' })} />
          </section>
          <p className="settings__foot">Solar RP is not affiliated with Rockstar Games or Take-Two Interactive.</p>
          <p className="settings__foot">{`Background footage and stills: official Grand Theft Auto V trailers and screenshots, ©${NBSP}Rockstar Games. Type: Sofia Sans, OFL.`}</p>
        </div>
      </div>
    </div>
  );
}

/**
 * Demo mode only: every install signs in as the same demo account, and the server refuses a second
 * player with a name already in use, so each PC picks its own in-game name. With a real account the
 * username is the in-game name and this field does not show.
 */
function InGameName() {
  const { settings, saveSetting, auth } = useApp();
  const shared = auth.status === 'signedIn' ? auth.account.username : '';
  const stored = settings?.playerName ?? '';
  const own = stored.toLowerCase() === shared.toLowerCase() ? '' : stored;
  const [name, setName] = useState(own);
  // A stored name an older build let through shows its problem at once; Play would replace it.
  const [error, setError] = useState<string | null>(own ? nameProblem(own) : null);
  useEffect(() => { setName(own); setError(own ? nameProblem(own) : null); }, [own]);
  const commit = () => {
    const v = normalizeInGameName(name);
    setName(v);
    // Empty here means "not set", which the placeholder promises is fine.
    if (v === own) { setError(v ? nameProblem(v) : null); return; }
    // Emptying the field un-sets the name: the next Play picks a fresh random one.
    if (!v) { setError(null); saveSetting('playerName', ''); return; }
    const problem = nameProblem(v);
    if (problem) { setError(problem); return; }
    if (v.toLowerCase() === shared.toLowerCase()) { setError(`"${v}" is the shared demo account name. Pick your own.`); return; }
    setError(null);
    saveSetting('playerName', v);
  };
  return (
    <Field
      label="In-game name"
      value={name}
      placeholder={own ? undefined : 'Not set: your first Play picks one, like Player_7K2Q'}
      // Typing stops at the server's limit; a longer paste is cut where the user can see it.
      maxLength={MAX_IN_GAME_NAME}
      // A character the server refuses shows on the keystroke that adds it, not on a blur that may never be seen.
      onChange={(e) => { setName(e.target.value); setError(charProblem(e.target.value)); }}
      onKeyDown={(e) => { if (e.key === 'Enter') e.currentTarget.blur(); }}
      onBlur={commit}
      error={error}
      hint="What other players see. Saved when you leave the field."
      spellCheck={false}
      autoComplete="off"
    />
  );
}

function AccountFrame() {
  const { auth, api } = useApp();
  const [current, setCurrent] = useState('');
  const [next, setNext] = useState('');
  const [state, setState] = useState<{ busy: boolean; msg: string | null; err: string | null }>({ busy: false, msg: null, err: null });
  if (auth.status !== 'signedIn') return null;
  const a = auth.account;
  const ok = next.length >= 10 && (!a.hasPassword || current.length > 0);
  const submit = async () => {
    setState({ busy: true, msg: null, err: null });
    try {
      const r = await api.changePassword(next, a.hasPassword ? current : undefined);
      setCurrent(''); setNext('');
      setState({ busy: false, msg: `Password changed. ${r.revokedSessions} other device${r.revokedSessions === 1 ? ' was' : 's were'} signed out.`, err: null });
    } catch (e) {
      setState({ busy: false, msg: null, err: describeFailure(e) });
    }
  };
  return (
    <section className="sframe" aria-labelledby="s-account">
      <header className="sframe__head"><h2 id="s-account">Account</h2></header>
      <div className="path">
        <span className="field__label">Email</span>
        <span className="path__value">{a.email ?? 'No email on this account'} {a.email && <Chip tone={a.emailVerified ? 'done' : 'warn'}>{a.emailVerified ? 'Verified' : 'Not verified'}</Chip>}</span>
        <span className="field__hint">Changing the email is not available in the client yet. Ask staff on Discord.</span>
      </div>
      <form className="pw" onSubmit={(e) => { e.preventDefault(); if (ok) void submit(); }} noValidate>
        {a.hasPassword && <Field label="Current password" type="password" autoComplete="current-password" value={current} onChange={(e) => setCurrent(e.target.value)} />}
        <Field label={a.hasPassword ? 'New password' : 'Set a password'} type="password" autoComplete="new-password" value={next} onChange={(e) => setNext(e.target.value)} hint="At least 10 characters. Other devices are signed out." />
        <div className="pw__actions">
          <Plate tone="ghost" type="submit" busy={state.busy} disabled={!ok}>Change password</Plate>
          {state.msg && <span className="sframe__note" role="status">{state.msg}</span>}
          {state.err && <span className="sframe__error" role="alert">{state.err}</span>}
        </div>
      </form>
    </section>
  );
}

function DevicesFrame() {
  const { api } = useApp();
  const [rows, setRows] = useState<SessionRow[] | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const load = useCallback(async () => {
    try { setRows((await api.sessions()).sessions); setErr(null); } catch (e) { setErr(describeFailure(e)); }
  }, [api]);
  useEffect(() => { void load(); }, [load]);
  const others = rows?.filter((r) => !r.current) ?? [];
  return (
    <section className="sframe" aria-labelledby="s-devices">
      <header className="sframe__head"><h2 id="s-devices">Devices</h2></header>
      {rows === null && !err && <p className="sframe__note">Reading signed-in devices…</p>}
      {err && <p className="sframe__error" role="alert">{err}</p>}
      {rows && (
        <ul className="devices">
          {rows.map((r) => (
            <li key={r.id} className={`device ${r.current ? 'is-current' : ''}`}>
              <div className="device__main">
                <span className="device__name">{r.deviceName ?? r.userAgent ?? 'Unknown device'}</span>
                <span className="device__meta">
                  <Code>{r.ip ?? 'IP hidden'}</Code>
                  <Code>Last seen {r.lastSeenAt ? new Date(r.lastSeenAt).toLocaleString('en-GB', { dateStyle: 'medium', timeStyle: 'short' }) : 'never'}</Code>
                </span>
              </div>
              {r.current ? <Chip>This device</Chip> : (
                <Plate tone="quiet" size="sm" busy={busy === r.id} onClick={async () => {
                  setBusy(r.id);
                  try { await api.revokeSession(r.id); await load(); } catch (e) { setErr(describeFailure(e)); } finally { setBusy(null); }
                }}>Sign out</Plate>
              )}
            </li>
          ))}
        </ul>
      )}
      {others.length > 1 && (
        <Plate tone="quiet" size="sm" busy={busy === 'all'} onClick={async () => {
          setBusy('all');
          try { await api.revokeOtherSessions(); await load(); } catch (e) { setErr(describeFailure(e)); } finally { setBusy(null); }
        }}>Sign out all other devices</Plate>
      )}
    </section>
  );
}

/** Destructive actions, each behind an inline confirmation (no modal). */
function SignOut({ onSignOut, onQuit }: { onSignOut: () => void; onQuit: () => void }) {
  const [confirm, setConfirm] = useState<'logout' | null>(null);
  return (
    <div className="exit">
      {confirm === 'logout' ? (
        <div className="exit__confirm" role="group" aria-label="Confirm log out">
          <p>Log out of Solar RP on this PC? You will need your password or Discord to come back.</p>
          <div className="exit__row">
            <Plate tone="danger" onClick={onSignOut}>Log out</Plate>
            <Plate tone="quiet" onClick={() => setConfirm(null)}>Keep me signed in</Plate>
          </div>
        </div>
      ) : (
        <div className="exit__row">
          <Plate tone="danger" onClick={() => setConfirm('logout')}>Log out of this device</Plate>
          <Plate tone="quiet" onClick={onQuit}>Quit Solar Launcher</Plate>
        </div>
      )}
    </div>
  );
}
