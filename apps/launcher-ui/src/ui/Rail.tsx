import { useLayoutEffect, useState } from 'react';
import { useApp, type Screen } from '../state/AppState';
import { GageMark, Icon, Lockup } from './primitives';

// The rail is the top edge of the picture, not a strip of window chrome: the GAGE-MP lockup,
// the sections (PLAY SHOP QUESTS PROFILE SETTINGS) at title scale on a graded edge, one gold marker
// with the G's 45-degree cut riding a hairline track under the current section, then the demo
// label, the account and the window controls. The rail is the window's drag region
// (app-region: drag); every control carves itself out with no-drag. Nothing clickable sits in
// the top 8 px (WebView2's top resize border).

const SECTIONS: Array<{ id: Screen; label: string }> = [
  { id: 'play', label: 'Play' },
  { id: 'shop', label: 'Shop' },
  { id: 'quests', label: 'Quests' },
  { id: 'profile', label: 'Profile' },
  { id: 'settings', label: 'Settings' },
];

/** Where the marker sits: the current tab's label span inside the nav, in px. */
interface Mark { x: number; w: number }

/**
 * Measures the current tab's label (the tab minus its side padding) and keeps it measured as
 * the nav resizes (fonts arriving, the window resizing). Null until there is a current tab.
 */
function useMark(nav: HTMLElement | null, screen: Screen): Mark | null {
  const [mark, setMark] = useState<Mark | null>(null);
  useLayoutEffect(() => {
    if (!nav) { setMark(null); return; }
    const measure = () => {
      const tab = nav.querySelector<HTMLElement>('.tab.is-active');
      if (!tab) { setMark(null); return; }
      const pad = parseFloat(getComputedStyle(tab).paddingLeft) || 0;
      const next = { x: Math.round(tab.offsetLeft + pad), w: Math.round(tab.offsetWidth - 2 * pad) };
      setMark((m) => (m && m.x === next.x && m.w === next.w ? m : next));
    };
    measure();
    if (typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(measure);
    ro.observe(nav);
    return () => ro.disconnect();
  }, [nav, screen]);
  return mark;
}

/** `locked`: the launch log covers the screen; sections and the account take no clicks
 *  (the window controls always work). */
export function Rail({ locked = false }: { locked?: boolean }) {
  const { auth, screen, setScreen, bridge, demo, windowState } = useApp();
  const signedIn = auth.status === 'signedIn';
  const [nav, setNav] = useState<HTMLElement | null>(null);
  const mark = useMark(signedIn ? nav : null, screen);
  return (
    <header className={`rail${locked ? ' is-locked' : ''}${signedIn ? '' : ' is-guest'}`} data-drag>
      <div className="rail__brand">{signedIn ? <Lockup height="var(--lockup-h)" /> : <GageMark size={26} title="Solar RP" />}</div>
      {signedIn && (
        <nav className="rail__nav" aria-label="Sections" ref={setNav}>
          {SECTIONS.map((s) => {
            const active = s.id === screen;
            return (
              <button
                key={s.id}
                type="button"
                className={`tab ${active ? 'is-active' : ''}`}
                aria-current={active ? 'page' : undefined}
                disabled={locked}
                onClick={() => setScreen(s.id)}
              >
                {s.label}
              </button>
            );
          })}
          {mark && <span className="rail__mark" style={{ ['--mark-x' as string]: `${mark.x}px`, ['--mark-w' as string]: `${mark.w}px` }} aria-hidden />}
        </nav>
      )}
      <div className="rail__end">
        {demo && <span className="rail__demo">Demo data · not live</span>}
        {signedIn && (
          <button type="button" className="rail__account" disabled={locked} onClick={() => setScreen('profile')} aria-label={`Profile: ${auth.account.username}`} title={`Profile: ${auth.account.username}`}>
            <span className="rail__avatar" aria-hidden>{auth.account.username.slice(0, 1).toUpperCase()}</span>
            <span className="rail__user">{auth.account.username}</span>
          </button>
        )}
      </div>
      <div className="rail__window">
        <button type="button" className="win-btn" aria-label="Minimize" onClick={() => bridge.send({ type: 'minimize' })}><Icon name="min" /></button>
        <button type="button" className="win-btn" aria-label={windowState.maximized ? 'Restore' : 'Maximize'} onClick={() => bridge.send({ type: 'toggleMaximize' })}><Icon name="max" /></button>
        <button type="button" className="win-btn win-btn--close" aria-label="Close to tray" onClick={() => bridge.send({ type: 'close' })}><Icon name="close" /></button>
      </div>
    </header>
  );
}
