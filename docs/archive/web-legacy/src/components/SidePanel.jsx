import { useEffect, useRef, useState } from 'react';
import { DEFAULT_PORT, displayName, langName, modeLabel } from '../lib/format.js';
import { IconCheck, IconStar } from './icons.jsx';
import { BACKDROP_ART, coverFor } from './gta.jsx';
import { Key, PingCell, Spinner } from './ui.jsx';

/** A failed `launchResult`'s message; an empty `error` says "unknown error" explicitly. */
export function failureText(result) {
  const error = typeof result.error === 'string' ? result.error.trim() : '';
  const code = Number.isFinite(result.code) && result.code !== 0 ? ` (code ${result.code})` : '';
  return error || `Unknown error${code}`;
}

/**
 * The job card column, as in GTA Online's lobby: the destination's key art,
 * its title strip in the lead's colour, the facts as label/value rows, then
 * the one action. Direct connect sits at the foot of the column.
 */
export default function SidePanel({
  selected,
  launching,
  launchResult,
  promote,
  overview = null,
  listState = 'ok',
  dcFocus = 0,
  isFavorite,
  pingState,
  onToggleFavorite,
  onLaunch,
  onDirectConnect,
  onOpenSettings,
  onPick,
  onPickFocus,
}) {
  const [dc, setDc] = useState('');
  const [dcError, setDcError] = useState('');
  const dcRef = useRef(null);
  const launched = !launching && !!launchResult?.ok;
  const failed = !!launchResult && !launchResult.ok;
  const cover = selected ? coverFor(selected) : null;
  const mode = modeLabel(selected?.mode);
  const lang = langName(selected?.lang);

  useEffect(() => {
    if (!promote) return;
    // take focus only when nothing else holds it
    const active = document.activeElement;
    if (!active || active === document.body) dcRef.current?.focus();
  }, [promote]);

  // "Connect directly" from an empty or offline list lands here
  useEffect(() => {
    if (dcFocus) dcRef.current?.focus();
  }, [dcFocus]);

  function submitDirect(e) {
    e.preventDefault();
    if (launching || launched) return;
    if (onDirectConnect(dc)) {
      setDc('');
      setDcError('');
    } else {
      setDcError('Enter host:port, ip:port, or a ws:// address.');
    }
  }

  const word = launching ? 'Launching' : launched ? 'Launched' : failed && selected ? 'Retry' : 'Connect';
  // Nothing picked: the strip names the session, never a negative. The list
  // panel already explains an offline or empty list, so the card says only
  // what holds (and the hint is left to the list where it would repeat it).
  // (while loading, the list panel says so; the card does not repeat it)
  const idleTitle = overview ? (overview.mode ? `${overview.mode} servers` : 'Online now') : listState === 'loading' ? 'Online now' : listState === 'error' ? 'List offline' : listState === 'empty' ? 'No servers listed' : 'Online now';
  const idleHint = overview ? 'Pick a server, or connect directly by address.' : '';

  return (
    // overflow-x stays hidden: overflow-y:auto alone turns x to auto
    <aside className="side app-no-drag" aria-label="Launch">
      <section className="card">
        {cover ? (
          <img key={cover.src} className="card-art" src={cover.src} alt="" decoding="async" style={{ objectPosition: cover.focus }} />
        ) : (
          // nothing picked: the city itself, unblurred, as the game's freemode card
          <img className="card-art is-empty" src={BACKDROP_ART} alt="" decoding="async" />
        )}
        <div className="card-title">
          <h2 className="min-w-0 truncate" title={selected ? displayName(selected) : undefined}>
            {selected && <span className="sr-only">Destination: </span>}
            {selected ? displayName(selected) : idleTitle}
          </h2>
          {selected && (
            <button
              type="button"
              onClick={() => onToggleFavorite(selected)}
              aria-pressed={isFavorite}
              aria-label={isFavorite ? 'Remove from favorites' : 'Add to favorites'}
              title={isFavorite ? 'Remove from favorites (F)' : 'Add to favorites (F)'}
              className={`star ${isFavorite ? 'is-on' : ''}`}
            >
              <IconStar className="h-[17px] w-[17px]" filled={isFavorite} />
            </button>
          )}
        </div>
        {selected ? (
          <dl className="facts">
            {mode && (
              <div>
                <dt>Mode</dt>
                <dd className="truncate">{mode}</dd>
              </div>
            )}
            {lang && (
              <div>
                <dt>Language</dt>
                <dd className="truncate">{lang}</dd>
              </div>
            )}
            <div>
              <dt>Players</dt>
              <dd>
                {selected.players >= 0 ? selected.players : '—'}
                {selected.maxPlayers > 0 && <span className="fact-sub">{`of ${selected.maxPlayers}`}</span>}
              </dd>
            </div>
            <div>
              <dt>Ping</dt>
              <dd>
                <PingCell ms={selected.ping} state={pingState} />
              </dd>
            </div>
            <div>
              <dt>Address</dt>
              <dd className="is-plain truncate">
                {selected.host}:{selected.port}
              </dd>
            </div>
          </dl>
        ) : (
          <>
            {/* nothing picked: the session's numbers, from the list already fetched */}
            {overview && (
              <>
                <dl className="facts">
                  <div>
                    <dt>Servers</dt>
                    <dd>{overview.servers}</dd>
                  </div>
                  <div>
                    <dt>Players online</dt>
                    <dd>{overview.players}</dd>
                  </div>
                </dl>
                {/* the standouts are a click away: picking one selects it in the list */}
                <div className="facts">
                  {[
                    overview.busiest && { label: 'Most players', s: overview.busiest, sub: `${overview.busiest.players} online` },
                    overview.best && { label: 'Lowest ping', s: overview.best, sub: `${overview.best.ping} ms` },
                  ]
                    .filter(Boolean)
                    .map((f) => (
                      <button
                        key={f.label}
                        type="button"
                        className="fact-pick"
                        onClick={() => onPick(f.s)}
                        onFocus={() => onPickFocus?.(true)}
                        onBlur={() => onPickFocus?.(false)}
                        title={`Select ${displayName(f.s)}`}
                      >
                        <span className="fact-pick-label">{f.label}</span>
                        <span className="truncate font-bold">{displayName(f.s)}</span>
                        <span className="fact-sub">{f.sub}</span>
                      </button>
                    ))}
                </div>
              </>
            )}
            {/* the list is still coming: the card's rows wait with it */}
            {!overview && listState === 'loading' && (
              <dl className="facts" aria-hidden="true">
                <div>
                  <dt>Servers</dt>
                  <dd>–</dd>
                </div>
                <div>
                  <dt>Players online</dt>
                  <dd>–</dd>
                </div>
              </dl>
            )}
            {idleHint && <p className="card-hint">{idleHint}</p>}
          </>
        )}

        {/* Only a picked server has a Connect (no ghost button over nothing).
            Locked from launch until the player is back (App re-arms it), so it
            never invites a second launch mid-boot. */}
        {selected && (
          <button
            type="button"
            onClick={() => !launched && onLaunch(selected)}
            disabled={launching || launched}
            className={`connect ${launching ? 'is-launching' : ''} ${launched ? 'is-launched' : ''}`}
            aria-label={
              launched
                ? `Launched ${displayName(selected)}`
                : failed
                  ? `Retry ${displayName(selected)}`
                  : `Connect to ${displayName(selected)}`
            }
          >
            <span>{word}</span>
            {/* a disabled action shows no key: nothing would happen on it */}
            {launching ? <Spinner className="h-[18px] w-[18px]" /> : launched ? <IconCheck className="h-5 w-5" /> : <Key>Enter</Key>}
          </button>
        )}

        {/* The error itself is said once, on the foot's status line; the
            card only offers the fix. code 1 = native found no GTA5.exe
            (BRIDGE.md): the fix is one field away. */}
        {failed && launchResult.code === 1 && (
          <div className="card-note">
            <button type="button" className="menu-btn" onClick={onOpenSettings}>
              <span>Set game folder</span>
            </button>
          </div>
        )}
      </section>

      <section className={`dc ${promote ? 'is-promoted' : ''}`} aria-label="Direct connect">
        <div className="dc-head">
          <h2 className="panel-title">Direct connect</h2>
          <span className="ml-auto text-[11px] text-white/60">{`Default port ${DEFAULT_PORT}`}</span>
        </div>
        <form onSubmit={submitDirect} className="dc-form" noValidate>
          <input
            ref={dcRef}
            value={dc}
            onChange={(e) => {
              setDc(e.target.value);
              if (dcError) setDcError('');
            }}
            placeholder="host:port or ws://host:22005"
            spellCheck="false"
            aria-label="Server address"
            aria-invalid={!!dcError}
            aria-describedby={dcError ? 'dc-error' : undefined}
            className={`field ${dcError ? 'is-invalid' : ''}`}
          />
          <button type="submit" disabled={launching || launched || !dc.trim()} className="go">
            Go
          </button>
        </form>
        {dcError && (
          <p id="dc-error" className="dc-error">
            {dcError}
          </p>
        )}
      </section>
    </aside>
  );
}
