import { useEffect, useState } from 'react';
import { describeFailure } from '../lib/errors';
import { tokenFromPaste } from '../lib/links';
import { ACCOUNT_URL } from '../lib/site';
import type { Balance, OwnedItem } from '../lib/schemas';
import { useApp } from '../state/AppState';
import { vipWindow, type VipWindow } from '../state/shop';
import { useNow } from '../ui/hooks';
import { Chip, Code, Field, Icon, Plate } from '../ui/primitives';

// Profile: who you are on the network. Bottom left over the still, the username at display
// scale with its roles under it; on the right, one opaque panel with the email state, the linked
// Discord account, wallet, owned count and VIP time as an exact-length bar (one tick per day).

const DAY = 86_400_000;

export function ProfileScreen() {
  const { auth, api, authDispatch, openExternal, demo, verifyEmail, notice, clearNotice, windowState } = useApp();
  const now = useNow(60_000);
  const [items, setItems] = useState<OwnedItem[] | null>(null);
  /** Wallet balances; 'error' when the platform did not answer (shown as such, never as 0). */
  const [balances, setBalances] = useState<Balance[] | 'error' | null>(null);
  const [msg, setMsg] = useState<string | null>(null);
  const [busy, setBusy] = useState<'resend' | 'verify' | null>(null);
  const [pasting, setPasting] = useState(false);
  const [pasted, setPasted] = useState('');

  useEffect(() => {
    let off = false;
    api.items().then((r) => { if (!off) setItems(r.items); }).catch(() => { if (!off) setItems([]); });
    api.wallet().then((r) => { if (!off) setBalances(r.balances); }).catch(() => { if (!off) setBalances('error'); });
    return () => { off = true; };
  }, [api]);

  // After "Link Discord" or "Unlink" sent the player to the website: re-read the account when the
  // window is focused again, so a change made there shows here.
  const [linkOpened, setLinkOpened] = useState(false);
  useEffect(() => {
    if (!linkOpened || !windowState.focused) return;
    let off = false;
    api.me().then(
      ({ account }) => { if (!off) authDispatch({ type: 'ACCOUNT', account }); },
      (e: unknown) => console.warn('[profile] account refresh failed:', e),
    );
    return () => { off = true; };
  }, [linkOpened, windowState.focused, api, authDispatch]);

  // A handled email link (AppState) reports here once.
  useEffect(() => {
    if (notice === null) return;
    setMsg(notice);
    clearNotice();
  }, [notice, clearNotice]);

  if (auth.status !== 'signedIn') return null;
  const a = auth.account;
  const vip = items ? vipWindow(items, now) : null;
  const pastedToken = tokenFromPaste(pasted, 'verifyEmail');
  const run = async (kind: 'resend' | 'verify', f: () => Promise<string | null>) => {
    setBusy(kind);
    setMsg(null);
    try { setMsg(await f()); } catch (e) { setMsg(describeFailure(e)); } finally { setBusy(null); }
  };

  return (
    <div className="profile">
      <section className="profile__plate">
        <h1 className="profile__name reveal">{a.username}</h1>
        <div className="profile__roles">
          {a.roles.map((r) => <Chip key={r}>{r}</Chip>)}
          <span className="profile__since">Since {new Date(a.createdAt).toLocaleDateString('en-GB', { month: 'short', year: 'numeric' })}</span>
          {demo && <Chip tone="demo">Demo account</Chip>}
        </div>

        {a.ban && (
          <div className="banner banner--hard" role="alert">
            <Icon name="alert" size={18} />
            <div>
              <strong>Banned</strong>
              <p>{a.ban.reason}{a.ban.expiresAt ? ` · until ${new Date(a.ban.expiresAt).toLocaleString('en-GB', { dateStyle: 'medium', timeStyle: 'short' })}` : ' · permanent'}</p>
            </div>
          </div>
        )}
      </section>

      <aside className="profile__panel panel" aria-label="Account">
        <dl className="facts">
          <div className="facts__row">
            <dt>Email</dt>
            <dd>
              <span>{a.email ?? 'No email on this account'}</span>
              {a.email && (a.emailVerified ? <Chip tone="done">Verified</Chip> : (
                <>
                  <Chip tone="warn">Not verified</Chip>
                  <button type="button" className="text-link" disabled={busy !== null} onClick={() => run('resend', async () => { await api.resendVerification(); return 'Verification email sent.'; })}>Resend link</button>
                  {!pasting && <button type="button" className="text-link" onClick={() => setPasting(true)}>Paste the link</button>}
                  {pasting && (
                    // The mail was opened on another device: its gage-mp:// link cannot reach this PC.
                    <form
                      className="facts__paste"
                      noValidate
                      onSubmit={(e) => {
                        e.preventDefault();
                        if (!pastedToken) return;
                        void run('verify', async () => {
                          const text = await verifyEmail(pastedToken);
                          setPasting(false);
                          setPasted('');
                          return text;
                        });
                      }}
                    >
                      <Field
                        label="Verification link from the email"
                        value={pasted}
                        onChange={(e) => setPasted(e.target.value)}
                        autoFocus
                        spellCheck={false}
                        autoComplete="off"
                        error={pasted.trim().length >= 16 && !pastedToken ? 'That is not a verification link.' : null}
                      />
                      <div className="facts__paste-actions">
                        <Plate tone="ghost" type="submit" busy={busy === 'verify'} disabled={!pastedToken || busy !== null}>Verify</Plate>
                        <button type="button" className="text-link" onClick={() => { setPasting(false); setPasted(''); }}>Cancel</button>
                      </div>
                    </form>
                  )}
                </>
              ))}
            </dd>
          </div>
          <div className="facts__row">
            <dt>Discord</dt>
            <dd>
              {a.discord ? (
                <>
                  <span>{a.discord.username ?? a.discord.id}</span>
                  <Chip tone="done">Linked</Chip>
                  {/* Unlinking asks for the password, so it runs on the website too. */}
                  <button type="button" className="text-link" disabled={busy !== null} onClick={() => {
                    openExternal(ACCOUNT_URL);
                    setLinkOpened(true);
                    setMsg('We opened your account page on solar-rp.com. Unlink Discord there; it shows here when you come back to this window.');
                  }}>Unlink <Icon name="external" /></button>
                </>
              ) : (
                <>
                  <span className="dim">Not linked</span>
                  {/* Linking runs on the website: it asks for the password, then makes the Discord round trip. */}
                  <button type="button" className="text-link" disabled={busy !== null} onClick={() => {
                    openExternal(ACCOUNT_URL);
                    setLinkOpened(true);
                    setMsg('We opened your account page on solar-rp.com. Link Discord there; it shows here when you come back to this window.');
                  }}>Link Discord <Icon name="external" /></button>
                </>
              )}
            </dd>
          </div>
          <div className="facts__row">
            <dt>Wallet</dt>
            <dd>
              {balances === null ? <span className="dim">Reading…</span> : balances === 'error' ? <span className="dim">Unavailable right now</span> : balances.length === 0 ? <span className="dim">Empty</span> : balances.map((b) => (
                <span key={b.currency} className="facts__count"><span className="facts__n">{b.balance.toLocaleString('en').replace(/,/g, ' ')}</span> <Code>{b.currency}</Code></span>
              ))}
            </dd>
          </div>
          <div className="facts__row">
            <dt>Owned</dt>
            <dd>
              {items === null ? <span className="dim">Reading…</span> : (
                <>
                  <span className="facts__count"><span className="facts__n">{items.length}</span> <Code>{items.length === 1 ? 'item' : 'items'}</Code></span>
                  {items.some((i) => !i.delivered) && <Chip tone="pending">{items.filter((i) => !i.delivered).length} queued</Chip>}
                </>
              )}
            </dd>
          </div>
        </dl>

        <VipBar vip={vip} />
        {msg && <p className="profile__msg" role="status">{msg}</p>}
      </aside>
    </div>
  );
}

/** VIP as an exact-length bar: one tick per day of the current VIP window, remaining days lit. */
export function VipBar({ vip }: { vip: VipWindow | null }) {
  if (vip === null) return <div className="vip"><span className="vip__label">VIP</span><span className="dim">Reading…</span></div>;
  if (vip.kind === 'none') return <div className="vip"><span className="vip__label">VIP</span><span className="dim">No VIP time. VIP passes are in the shop.</span></div>;
  if (vip.kind === 'permanent') return <div className="vip"><span className="vip__label">VIP</span><Chip tone="done">Permanent</Chip></div>;
  const totalDays = Math.max(1, Math.round(vip.totalMs / DAY));
  const leftDays = Math.floor(vip.remainingMs / DAY);
  const leftHours = Math.floor((vip.remainingMs % DAY) / 3_600_000);
  const lit = vip.remainingMs / DAY;
  const ticks = Math.min(totalDays, 120);
  const perTick = totalDays / ticks;
  return (
    <div className="vip" role="group" aria-label={`VIP: ${leftDays} days ${leftHours} hours left of ${totalDays}`}>
      <div className="vip__head">
        <span className="vip__label">VIP</span>
        <span className="vip__left"><span className="vip__n">{leftDays}</span> d <span className="vip__n">{leftHours}</span> h left</span>
        <Code>Ends {new Date(vip.endsAt).toLocaleDateString('en-GB', { day: '2-digit', month: 'short', year: 'numeric' }).toUpperCase()}</Code>
      </div>
      <div className="vip__bar" style={{ ['--ticks' as string]: ticks }} aria-hidden>
        {Array.from({ length: ticks }, (_, i) => {
          // Remaining time is lit from the left; each tick is `perTick` days of the window.
          const state = (i + 1) * perTick <= lit ? 'on' : i * perTick < lit ? 'part' : 'off';
          return <span key={i} className={`vip__tick vip__tick--${state}`} />;
        })}
      </div>
      <div className="vip__scale"><Code>Now</Code><Code>{totalDays}-day window</Code></div>
    </div>
  );
}
