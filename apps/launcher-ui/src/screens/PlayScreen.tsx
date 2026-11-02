import { useEffect, type ReactNode } from 'react';
import type { Server } from '../lib/schemas';
import { clipThumbUrl, serverArt } from '../media/catalog';
import { useApp } from '../state/AppState';
import { isBusy, stageLabel } from '../state/launch';
import { EmblemSlot } from '../ui/emblemSlot';
import { Chip, Icon, Plate, StatusDot } from '../ui/primitives';

// Play: the chosen server's own footage fills the window (App/Backdrop). Bottom left, the
// emblem beside the server's name at display scale, its live count, and the PLAY plate. On the
// right, our servers as a column of screenshot cards; choosing one crossfades the window into
// its world. Enter plays.

/** Why a server cannot be played right now, or null when it can. */
export function blockedReason(s: Server): string | null {
  if (s.comingSoon) return 'This server is not live yet.';
  if (s.maintenance.enabled) return s.maintenance.message || 'This server is in maintenance.';
  if (s.status.state === 'offline') return 'This server is offline right now.';
  if (s.status.state === 'unknown') return null; // not probed yet: joining is still allowed
  if (s.status.players !== null && s.status.maxPlayers && s.status.players >= s.status.maxPlayers) return 'This server is full.';
  return null;
}

function liveState(s: Server): 'online' | 'offline' | 'unknown' | 'maintenance' {
  return s.maintenance.enabled ? 'maintenance' : s.status.state;
}

export function PlayScreen({ onPlay }: { onPlay: () => void }) {
  const { servers, selectedServerId, selectServer, launch, settings, refreshServers, demo, setScreen } = useApp();
  const list = servers.servers;
  const selected = list.find((s) => s.id === selectedServerId) ?? null;
  const blocked = selected ? blockedReason(selected) : 'Pick a server.';
  const noGame = settings !== null && !settings.gamePath;
  // With no GTA V folder a launch can only fail, so setting the folder is the one action.
  const busy = isBusy(launch);
  const canPlay = !!selected && !blocked && !noGame && !busy;
  const allOut = list.length > 1 && list.every((s) => s.status.state === 'offline' && !s.maintenance.enabled);
  const busyLabel = launch.phase === 'joining' ? 'Joining' : launch.phase === 'launching' ? stageLabel(launch.stage) : '';

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const t = e.target as HTMLElement | null;
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT')) return;
      if (e.key === 'Enter' && canPlay && (!t || t === document.body || t.closest('.servers'))) {
        e.preventDefault();
        onPlay();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [canPlay, onPlay]);

  return (
    <div className="play">
      <section className="play__hero" aria-live="polite">
        {servers.status === 'loading' && list.length === 0 && <HeroMessage title="Finding servers" body="Asking the Solar RP network which of our servers are up." />}
        {servers.status === 'error' && list.length === 0 && (
          <HeroMessage title={servers.offline ? 'Offline' : 'No answer'} body={servers.error ?? ''} action={<Plate tone="ghost" onClick={refreshServers}><Icon name="refresh" /> Try again</Plate>} />
        )}
        {servers.status === 'ok' && list.length === 0 && (
          <HeroMessage title="No servers up" body="None of our servers are listed right now. They come back here the moment one starts." action={<Plate tone="ghost" onClick={refreshServers}><Icon name="refresh" /> Check again</Plate>} />
        )}
        {selected && (
          <div className="hero">
            <div className="hero__title">
              <EmblemSlot className="hero__emblem" />
              <h1 className="hero__name reveal" key={selected.id}>{selected.name}</h1>
            </div>
            {selected.status.state === 'online' && selected.status.players !== null && !selected.maintenance.enabled ? (
              <div className="count">
                <span className="count__n">{selected.status.players}</span>
                <span className="count__side">
                  <span className="count__label"><StatusDot state={liveState(selected)} /> Online now</span>
                  <span className="count__max">of {selected.status.maxPlayers ?? '—'} slots</span>
                </span>
                {selected.status.maxPlayers ? <span className="count__bar" style={{ ['--fill' as string]: Math.min(1, selected.status.players / selected.status.maxPlayers) }} aria-hidden /> : null}
              </div>
            ) : null}
            <div className="hero__meta">
              {!(selected.status.state === 'online' && selected.status.players !== null && !selected.maintenance.enabled) && (
                <span className="hero__live">
                  <StatusDot state={liveState(selected)} />
                  <span>{selected.maintenance.enabled ? 'Maintenance' : selected.comingSoon ? 'Coming soon' : selected.status.state === 'unknown' ? 'Checking server' : 'Offline'}</span>
                </span>
              )}
              <span className="hero__fact">{selected.region}</span>
              {selected.tags.map((t) => <span key={t} className="hero__fact">{t}</span>)}
              {selected.status.pingMs !== null && selected.status.state === 'online' && <span className="hero__fact">{Math.round(selected.status.pingMs)} ms</span>}
              {demo && selected.status.state === 'online' && !selected.maintenance.enabled && !selected.status.live && <Chip tone="demo">Demo counts</Chip>}
            </div>
            {selected.maintenance.enabled && (
              <div className="banner" role="status">
                <Icon name="alert" size={18} />
                <div>
                  <strong>Maintenance</strong>
                  <p>{selected.maintenance.message || 'This server is being worked on.'}</p>
                </div>
              </div>
            )}
            <div className="hero__action">
              {noGame && !blocked ? (
                <Plate tone="primary" size="xl" className="play-plate play-plate--setup" onClick={() => setScreen('settings')} aria-describedby="play-why">
                  <Icon name="folder" size={22} /> Set GTA V folder
                </Plate>
              ) : (
                <Plate tone="primary" size="xl" className="play-plate" onClick={onPlay} disabled={!canPlay && !busy} busy={busy} aria-describedby="play-why">
                  {busy ? busyLabel : 'Play'}
                </Plate>
              )}
              {/* Says only why PLAY is closed; a playable server needs no caption. The maintenance
                  banner above already carries its own message. */}
              <p id="play-why" className="hero__why">
                {busy ? '' : selected.maintenance.enabled ? 'PLAY opens when the maintenance ends.' : allOut ? 'All of our servers are offline right now. They come back here the moment one starts.' : blocked ?? (noGame ? 'GTA V was not found on this PC. PLAY opens once its folder is set.' : '')}
              </p>
            </div>
            {servers.status === 'error' && list.length > 0 && <p className="hero__stale">{servers.error} Showing the last list.</p>}
          </div>
        )}
      </section>
      {list.length > 0 && <ServerList servers={list} selectedId={selectedServerId} onSelect={selectServer} demo={demo} />}
    </div>
  );
}

function HeroMessage({ title, body, action }: { title: string; body: string; action?: ReactNode }) {
  return (
    <div className="hero hero--message">
      <div className="hero__title">
        <EmblemSlot className="hero__emblem" />
        <h1 className="hero__name hero__name--quiet">{title}</h1>
      </div>
      <p className="hero__body">{body}</p>
      {action}
    </div>
  );
}

/**
 * Our servers as screenshot cards, one per server: art by name/tag (media/catalog.ts), name,
 * region and mode, the live count and a capacity bar. The chosen card is framed in gold; an
 * unplayable one goes grey and says why. A listbox: arrow keys move, Enter plays.
 */
export function ServerList({ servers, selectedId, onSelect, demo }: { servers: Server[]; selectedId: string | null; onSelect: (id: string) => void; demo: boolean }) {
  const onKey = (e: React.KeyboardEvent, i: number) => {
    const fwd = e.key === 'ArrowDown' || e.key === 'ArrowRight';
    const back = e.key === 'ArrowUp' || e.key === 'ArrowLeft';
    if (!fwd && !back) return;
    e.preventDefault();
    const next = servers[(i + (fwd ? 1 : servers.length - 1)) % servers.length];
    if (next) {
      onSelect(next.id);
      (document.querySelector(`[data-frame="${next.id}"]`) as HTMLElement | null)?.focus();
    }
  };
  return (
    <section className="servers" aria-label="Our servers">
      {/* The cards carry their own count; the heading stays for screen readers only. */}
      <header className="servers__head sr-only">
        <h2 className="servers__title">Servers</h2>
        <span className="servers__count">{servers.length}</span>
      </header>
      <div className="servers__list" role="listbox" aria-label="Servers" aria-orientation="vertical">
        {servers.map((s, i) => {
          const chosen = s.id === selectedId;
          const state = liveState(s);
          const out = s.comingSoon || state === 'offline' || state === 'maintenance';
          const art = serverArt(s, i);
          // Capacity only for a server that is up: a server in maintenance shows no count (hero or
          // card), so its bar stays empty too.
          const fill = state === 'online' && s.status.players !== null && s.status.maxPlayers ? Math.min(1, s.status.players / s.status.maxPlayers) : 0;
          return (
            <button
              key={s.id}
              data-frame={s.id}
              type="button"
              role="option"
              aria-selected={chosen}
              tabIndex={chosen ? 0 : -1}
              className={`scard ${chosen ? 'is-chosen' : ''} ${out ? 'is-out' : ''}`}
              onClick={() => onSelect(s.id)}
              onKeyDown={(e) => onKey(e, i)}
            >
              <img className="scard__img" src={clipThumbUrl(art.clip)} alt="" decoding="async" />
              <span className="scard__body">
                <span className="scard__name">{s.name}</span>
                <span className="scard__meta">{[s.region, ...s.tags].join(' · ')}</span>
              </span>
              <span className="scard__state">
                {s.comingSoon ? 'Coming soon' : (
                  <>
                    <StatusDot state={state} />
                    {state === 'maintenance' ? 'Maintenance' : s.status.state === 'online' && s.status.players !== null ? (
                      <span className="scard__count">{s.status.players}<span>/{s.status.maxPlayers ?? '—'}</span></span>
                    ) : s.status.state === 'offline' ? 'Offline' : 'Checking'}
                  </>
                )}
              </span>
              <span className="scard__bar" style={{ ['--fill' as string]: fill }} aria-hidden />
            </button>
          );
        })}
      </div>
    </section>
  );
}
