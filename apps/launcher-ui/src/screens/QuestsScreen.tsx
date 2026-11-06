import { useCallback, useEffect, useRef, useState } from 'react';
import { describeFailure, isFailure } from '../lib/errors';
import type { Quest, QuestList } from '../lib/schemas';
import { ACCOUNT_URL } from '../lib/site';
import { useApp } from '../state/AppState';
import { coinsEarned, coinsLeft, deadlineFor, questAction, questFailureCopy, questOrder, refusalNeedsReload, waitFromFailure, type QuestAction } from '../state/quests';
import { fmtDate } from './ShopScreen';
import { useNow } from '../ui/hooks';
import { useLenis } from '../ui/useLenis';
import { Chip, Icon, Plate } from '../ui/primitives';

// Quests: coins for backing Solar RP. Bottom left over the dusk skyline reel, the title, one line
// on what pays, and the wallet as Play's count block (the figure, and a bar of earned out of what
// this screen pays, explained by the line beside it); on the right, one opaque panel with the
// Discord state and the quests as a ledger. Start opens the quest's page in the system browser
// and counts down to Claim; linking Discord happens on the website (lib/site.ts ACCOUNT_URL),
// from the Discord quest's own row. The platform decides every status (apps/platform docs/api.md
// "Quests"); state/quests.ts only turns it into the row's next step and the row order (what
// can be done now first, claimed last). Gold is rationed: unlinked, the Discord quest's Link
// Discord is the gold (nothing can be claimed); linked, only the first claimable row's Claim is.

/** `order`: the rows' slugs, taken when the list was read (state/quests.ts questOrder). */
type Loaded = { list: QuestList; receivedAt: number; order: string[] };

const fmtCoins = (n: number) => n.toLocaleString('en').replace(/,/g, ' ');
/** A wait as m:ss, the way a game clock reads. */
const fmtClock = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

export function QuestsScreen() {
  const { api, auth, authDispatch, demo, openExternal, windowState } = useApp();
  const [data, setData] = useState<Loaded | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  /** Local claim times by slug (epoch ms on this PC), from the last answer that carried a wait. */
  const [deadlines, setDeadlines] = useState<Record<string, number>>({});
  const [balance, setBalance] = useState<number | null | 'error'>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [rowError, setRowError] = useState<{ slug: string; text: string } | null>(null);
  const [credit, setCredit] = useState<{ amount: number; key: number } | null>(null);
  const [linkOpened, setLinkOpened] = useState(false);
  const scroller = useLenis<HTMLOListElement>();
  const rows = useRef(new Map<string, HTMLLIElement>());

  const absorb = useCallback((quests: readonly Quest[], receivedAt: number) => {
    setDeadlines((d) => {
      const next = { ...d };
      for (const q of quests) {
        const at = deadlineFor(q, receivedAt);
        if (at === null) delete next[q.slug];
        else next[q.slug] = at;
      }
      return next;
    });
  }, []);

  const load = useCallback(async () => {
    setLoading(true);
    const [q, w] = await Promise.allSettled([api.quests(), api.wallet()]);
    setLoading(false);
    const now = Date.now();
    if (q.status === 'fulfilled') {
      const list = { ...q.value, quests: [...q.value.quests].sort((a, b) => a.sort - b.sort) };
      const waits: Record<string, number> = {};
      for (const x of list.quests) {
        const at = deadlineFor(x, now);
        if (at !== null) waits[x.slug] = at;
      }
      setData({ list, receivedAt: now, order: questOrder(list.quests, waits, now, list.visitMinSeconds) });
      absorb(list.quests, now);
      setError(null);
      for (const x of list.quests) {
        if (!['available', 'started', 'claimable', 'claimed', 'locked'].includes(x.status)) console.warn(`[quests] unknown status "${x.status}" for ${x.slug}`);
      }
    } else {
      setError(describeFailure(q.reason));
    }
    if (w.status === 'fulfilled') setBalance(w.value.balances.find((b) => b.currency === 'coins')?.balance ?? 0);
    else setBalance((b) => (typeof b === 'number' ? b : 'error'));
  }, [api, absorb]);
  useEffect(() => { void load(); }, [load]);

  // Back from the browser (a Discord link made on the website, a page followed): read again.
  const wasFocused = useRef(windowState.focused);
  useEffect(() => {
    const regained = windowState.focused && !wasFocused.current;
    wasFocused.current = windowState.focused;
    if (!regained) return;
    void load();
    if (linkOpened) {
      api.me().then(
        ({ account }) => authDispatch({ type: 'ACCOUNT', account }),
        (e: unknown) => console.warn('[quests] account refresh failed:', e),
      );
    }
  }, [windowState.focused, load, linkOpened, api, authDispatch]);

  const quests = data?.list.quests ?? [];
  const waiting = quests.some((q) => q.status === 'started');
  const now = useNow(waiting ? 250 : 60_000);
  const linked = data?.list.discordLinked ?? false;
  const minWait = data?.list.visitMinSeconds ?? 30;

  // The credited flash beside the wallet fades after a few seconds.
  useEffect(() => {
    if (!credit) return;
    const t = window.setTimeout(() => setCredit(null), 4200);
    return () => window.clearTimeout(t);
  }, [credit]);

  const replace = (q: Quest) => setData((d) => (d ? { ...d, list: { ...d.list, quests: d.list.quests.map((x) => (x.slug === q.slug ? q : x)) } } : d));

  // A Start or Claim swaps the row's control (Claim becomes a chip), which drops keyboard focus to
  // the page; hand it to the row itself so the next Tab continues from there.
  const keepFocus = (slug: string) => {
    requestAnimationFrame(() => {
      const el = document.activeElement;
      if (el === null || el === document.body || !el.isConnected) rows.current.get(slug)?.focus();
    });
  };

  const fail = async (slug: string, e: unknown) => {
    if (isFailure(e, 'unauthorized')) { setRowError({ slug, text: describeFailure(e) }); return; }
    const wait = waitFromFailure(e);
    if (wait !== null) setDeadlines((d) => ({ ...d, [slug]: Date.now() + wait * 1000 }));
    setRowError({ slug, text: questFailureCopy(e) ?? describeFailure(e) });
    if (refusalNeedsReload(e)) await load();
  };

  const start = async (q: Quest) => {
    setBusy(q.slug);
    setRowError(null);
    try {
      const { quest } = await api.startQuest(q.slug);
      absorb([quest], Date.now());
      replace(quest);
      if (quest.url && /^https:\/\//i.test(quest.url)) openExternal(quest.url);
      else setRowError({ slug: q.slug, text: 'This quest has no page to open yet. It is started; claim it when the wait is over.' });
    } catch (e) {
      await fail(q.slug, e);
    } finally {
      setBusy(null);
      keepFocus(q.slug);
    }
  };

  const claim = async (q: Quest) => {
    setBusy(q.slug);
    setRowError(null);
    try {
      const r = await api.claimQuest(q.slug);
      replace(r.quest);
      if (r.credited.currency === 'coins') setBalance(r.balance);
      setCredit({ amount: r.credited.amount, key: Date.now() });
    } catch (e) {
      await fail(q.slug, e);
    } finally {
      setBusy(null);
      keepFocus(q.slug);
    }
  };

  const linkDiscord = () => {
    openExternal(ACCOUNT_URL);
    setLinkOpened(true);
  };

  const left = coinsLeft(quests);
  const earned = coinsEarned(quests);
  const discordName = auth.status === 'signedIn' ? auth.account.discord?.username ?? null : null;
  // Rows in the order taken at the last read; a quest the order does not know (never expected) goes last.
  const rank = new Map((data?.order ?? []).map((slug, i) => [slug, i]));
  const ordered = [...quests].sort((a, b) => (rank.get(a.slug) ?? rank.size) - (rank.get(b.slug) ?? rank.size));
  const actions = ordered.map((q) => questAction(q, deadlines[q.slug] ?? null, now, minWait));
  // The Discord quest's row holds Link Discord while no Discord is linked (and is the gold then).
  const linkRow = actions.some((a) => a.kind === 'link');
  const firstClaim = linkRow ? null : ordered.find((_, i) => actions[i]!.kind === 'claim')?.slug ?? null;
  // The line beside the wallet explains the bar under it, the way "of 300 slots" does on Play.
  const purseLine = balance === 'error' ? 'Wallet unavailable right now'
    : data === null ? (error ? null : 'Reading…')
      : quests.length === 0 ? null
        : `${fmtCoins(earned)} of ${fmtCoins(earned + left)} earned here`;

  return (
    <div className="quests">
      <section className="quests__plate">
        <h1 className="quests__title reveal">Quests</h1>
        <p className="quests__lead">Coins for backing Solar RP. Each quest pays once: claim it here and the coins land in your wallet.</p>
        <div className="count purse" aria-live="polite">
          <span className="count__n">{typeof balance === 'number' ? fmtCoins(balance) : '—'}</span>
          <span className="count__side">
            <span className="count__label">Coins in your wallet</span>
            {/* A failed or empty list says so once, in the panel; the purse then shows only the wallet. */}
            {purseLine && <span className="count__max">{purseLine}</span>}
          </span>
          {credit && <span key={credit.key} className="purse__credit">+{fmtCoins(credit.amount)}</span>}
          {data && earned + left > 0 && <span className="count__bar" style={{ ['--fill' as string]: earned / (earned + left) }} aria-hidden />}
        </div>
        {demo && <Chip tone="demo">Demo quests · not live</Chip>}
      </section>

      <aside className="quests__panel panel" aria-label="Quests">
        {!(data === null && error) && <div className="facts__row quests__discord">
          <span className="quests__discord-label">Discord</span>
          {data === null ? <span className="dim">Reading…</span> : (
            <span className="quests__discord-state">
              {linked ? <Chip tone="done">Linked</Chip> : <Chip>Not linked</Chip>}
              {linked && discordName && <span className="quests__discord-name">{discordName}</span>}
              {/* The Discord quest's row normally offers the link; with no such row (claimed on another
                  account, retired) the head keeps the way to link, as the gold: nothing can be claimed unlinked. */}
              {!linked && !linkRow && <Plate tone="primary" size="sm" onClick={linkDiscord}>Link Discord <Icon name="external" size={14} /></Plate>}
            </span>
          )}
        </div>}
        {/* Mounted for the screen's life, so the text that lands in it is announced. */}
        <div role="status" className="quests__status">
          {linkOpened && !linked && <p className="profile__msg quests__note">We opened your account page on solar-rp.com. Link Discord there; this list updates when you come back to this window.</p>}
        </div>

        {data === null ? (
          <div className="record--empty quests__empty" role={error ? 'alert' : undefined}>
            <h2 className="record__title">{error ? 'Quests unavailable' : 'Reading your quests'}</h2>
            <p className="record__body">{error && !loading ? error : 'One moment.'}</p>
            {error && <Plate tone="ghost" busy={loading} disabled={loading} onClick={() => void load()}><Icon name="refresh" /> Try again</Plate>}
          </div>
        ) : quests.length === 0 ? (
          <div className="record--empty quests__empty">
            <h2 className="record__title">No quests right now</h2>
            <p className="record__body">New quests show up here as soon as they open.</p>
          </div>
        ) : (
          <ol className="quests__list" ref={scroller}>
            {ordered.map((q, i) => (
              <QuestRow
                key={q.slug}
                rowRef={(el) => { if (el) rows.current.set(q.slug, el); else rows.current.delete(q.slug); }}
                quest={q}
                action={actions[i]!}
                primary={q.slug === firstClaim}
                busy={busy === q.slug}
                locked={busy !== null && busy !== q.slug}
                error={rowError?.slug === q.slug ? rowError.text : null}
                onStart={() => void start(q)}
                onClaim={() => void claim(q)}
                onLink={linkDiscord}
                onReopen={() => { if (q.url && /^https:\/\//i.test(q.url)) openExternal(q.url); }}
              />
            ))}
          </ol>
        )}
        {quests.length > 0 && <p className="quests__fine">Follow quests run on trust: open the page, follow it, then claim. Each quest pays once per Solar RP account and Discord account.</p>}
      </aside>
    </div>
  );
}

/**
 * One quest as one line in the list's ledger columns (title, reward, next step), in every state,
 * so nothing moves when a row changes. A started quest counts down inside its Claim plate, which
 * fills toward the moment it opens, with a quiet icon beside it to open the page again. A claimed
 * quest folds to its title, date and chip. The Discord quest, unlinked, offers Link Discord as
 * the gold. `primary` = the one claimable row whose Claim is gold. Exported for tests.
 */
export function QuestRow(p: {
  quest: Quest; action: QuestAction; primary: boolean; busy: boolean; locked: boolean; error: string | null;
  onStart: () => void; onClaim: () => void; onReopen: () => void; onLink?: () => void; rowRef?: (el: HTMLLIElement | null) => void;
}) {
  const { quest: q, action: a } = p;
  const done = a.kind === 'claimed';
  const fill = a.kind === 'wait' ? Math.min(1, Math.max(0, 1 - a.secondsLeft / Math.max(1, a.total))) : 0;
  return (
    <li ref={p.rowRef} tabIndex={-1} className={`quest quest--${a.kind}`} aria-label={q.title}>
      <div className="quest__body">
        <h3 className="quest__name" title={done ? q.title : undefined}>{q.title}</h3>
        {a.kind === 'locked' && <p className="quest__hint">{a.why}</p>}
        {done && a.at && <p className="quest__hint">Claimed {fmtDate(a.at)}</p>}
        {p.error && <p className="quest__error" role="alert"><Icon name="alert" size={14} /> {p.error}</p>}
      </div>
      {!done && <span className="quest__amt" aria-label={`${q.reward.amount} ${q.reward.currency}`}>+{fmtCoins(q.reward.amount)}</span>}
      <div className="quest__act">
        {a.kind === 'start' && <Plate tone="ghost" size="sm" busy={p.busy} disabled={p.locked} onClick={p.onStart}>Start <Icon name="external" size={14} /></Plate>}
        {a.kind === 'wait' && q.url && (
          <Plate tone="quiet" size="sm" className="quest__reopen" aria-label={`Open the ${q.title} page again`} title="Open the page again" onClick={p.onReopen}>
            <Icon name="external" size={14} />
          </Plate>
        )}
        {a.kind === 'wait' && (
          <Plate tone="ghost" size="sm" className="quest__wait" disabled style={{ ['--fill' as string]: fill }} aria-label={`Claim opens in ${a.secondsLeft} seconds`}>
            <span>Claim · <span className="quest__clock">{fmtClock(a.secondsLeft)}</span></span>
          </Plate>
        )}
        {a.kind === 'claim' && <Plate tone={p.primary ? 'primary' : 'ghost'} size="sm" busy={p.busy} disabled={p.locked} onClick={p.onClaim}>Claim</Plate>}
        {a.kind === 'link' && <Plate tone="primary" size="sm" onClick={p.onLink}>Link Discord <Icon name="external" size={14} /></Plate>}
        {a.kind === 'linkDiscord' && <Chip>Needs Discord</Chip>}
        {done && <Chip tone="done">Claimed</Chip>}
        {a.kind === 'soon' && <Chip>Coming soon</Chip>}
        {a.kind === 'locked' && <Chip>Unavailable</Chip>}
        {a.kind === 'unknown' && <Chip>Unavailable</Chip>}
      </div>
    </li>
  );
}
