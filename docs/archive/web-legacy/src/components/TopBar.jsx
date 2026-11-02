import { useEffect, useState } from 'react';
import { getBridge } from '../bridge.js';
import { Lockup } from './Brand.jsx';
import { IconClose, IconMinimize } from './icons.jsx';

export const TABS = [
  { id: 'browse', label: 'Servers' },
  { id: 'favorites', label: 'Favorites' },
  { id: 'recents', label: 'Recents' },
  { id: 'settings', label: 'Settings' },
];

// The pause menu's header clock: weekday and 24-hour time, refreshed each minute.
function useClock() {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    let t = 0;
    const tick = () => {
      setNow(new Date());
      t = window.setTimeout(tick, 60000 - (Date.now() % 60000));
    };
    t = window.setTimeout(tick, 60000 - (Date.now() % 60000));
    return () => window.clearTimeout(t);
  }, []);
  return now;
}

/** The header's right-hand stack, as the game's: who and when. (The session's numbers live on the overview card.) */
function HudCluster({ playerName }) {
  const now = useClock();
  const day = now.toLocaleDateString('en-US', { weekday: 'long' });
  const time = now.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
  return (
    <div className="hud">
      <span className="hud-name">{playerName || 'No player name'}</span>
      <span className="hud-line">{`${day} ${time}`}</span>
    </div>
  );
}

/** Title, header stack and window buttons, then the tab strip. */
export default function TopBar({ isMock, tab, onTab, counts, playerName }) {
  const bridge = getBridge();
  return (
    <>
      <header className="masthead app-drag select-none">
        <h1 className="title">
          <span className="sr-only">GAGE-MP</span>
          <Lockup height={48} />
        </h1>
        {/* DEV-only: also keeps the badge copy out of the production bundle. */}
        {import.meta.env.DEV && isMock && (
          <span className="dev-badge" title="No WebView2 bridge detected: running against the built-in preview mock">
            preview
          </span>
        )}
        <HudCluster playerName={playerName} />
        <div className="win app-no-drag">
          <button type="button" title="Minimize" aria-label="Minimize window" onClick={() => bridge.send({ type: 'minimize' })} className="win-btn">
            <IconMinimize className="h-4 w-4" />
          </button>
          <button
            type="button"
            title="Close (keeps running in the tray)"
            aria-label="Close to tray (GAGE-MP keeps running)"
            onClick={() => bridge.send({ type: 'close' })}
            className="win-btn is-close"
          >
            <IconClose className="h-4 w-4" />
          </button>
        </div>
      </header>

      <nav className="tabs app-no-drag" aria-label="Views">
        {TABS.map((t) => {
          const n = t.id === 'favorites' ? counts.favorites : t.id === 'recents' ? counts.recents : null;
          return (
            <button key={t.id} type="button" className="tab" onClick={() => onTab(t.id)} aria-current={tab === t.id ? 'page' : undefined}>
              {t.label}
              {n > 0 && <span className="tab-count">{n}</span>}
            </button>
          );
        })}
      </nav>
    </>
  );
}
