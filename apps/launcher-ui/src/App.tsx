import gsap from 'gsap';
import { lazy, Suspense, useCallback, useEffect, useRef, useState } from 'react';
import { playLaunch } from './audio/launch';
import { EMBLEM_3D } from './brand/brand';
import { Backdrop, type BackdropScene } from './media/Backdrop';
import { QUESTS_REEL, serverArt, type StillName } from './media/catalog';
import { AuthScreen } from './screens/AuthScreen';
import { LaunchOverlay } from './screens/LaunchOverlay';
import { PlayScreen } from './screens/PlayScreen';
import { ProfileScreen } from './screens/ProfileScreen';
import { QuestsScreen } from './screens/QuestsScreen';
import { SettingsScreen } from './screens/SettingsScreen';
import { UpdatePopup } from './screens/UpdatePopup';
import { ShopScreen } from './screens/ShopScreen';
import { AppStateProvider, useApp } from './state/AppState';
import { isBusy } from './state/launch';
import { renderActive, useIdle, useReducedMotion } from './ui/hooks';
import { GageMark } from './ui/primitives';
import { Rail } from './ui/Rail';

// The Solar RP client shell: GTA V footage full-bleed behind everything (Backdrop), a scrim per
// screen so type sits on black, the rail on top, the screen, the chrome emblem on its own small
// canvas, and the PLAY moment: the emblem charges, the screen gives way, and the launch sits over
// the same shot, pushed in and still playing.

const EmblemLayer = lazy(() => import('./three/EmblemLayer'));

export type Shot = 'auth' | 'play' | 'quests' | 'profile' | 'settings' | 'shop';

export default function App() {
  return (
    <AppStateProvider>
      <Shell />
    </AppStateProvider>
  );
}

function Shell() {
  const app = useApp();
  const { auth, screen, launch, windowState, gameRunning } = app;
  const reducedMotion = useReducedMotion();
  const idle = useIdle(10_000);
  const charge = useRef(0);
  const backdropEl = useRef<HTMLDivElement>(null);
  const screenEl = useRef<HTMLElement>(null);
  const [covered, setCovered] = useState(false);
  const [launchKey, setLaunchKey] = useState(0);
  const [booted, setBooted] = useState(false);
  const [shopStill, setShopStill] = useState<StillName>('vehicle');
  /** When PLAY was pressed (epoch ms): the launch log's clock counts from here. */
  const [launchStartedAt, setLaunchStartedAt] = useState(0);

  const shot: Shot = auth.status !== 'signedIn' ? 'auth' : screen;
  const servers = app.servers.servers;
  const selIndex = servers.findIndex((s) => s.id === app.selectedServerId);
  const selected = selIndex >= 0 ? servers[selIndex]! : null;
  const art = selected ? serverArt(selected, selIndex) : null;
  const scene: BackdropScene =
    shot === 'auth' ? { clip: 'skyline' }
      : shot === 'play' ? { clip: art?.clip ?? 'skyline' }
        // Its own dusk reel (catalog.ts QUESTS_REEL), framed right of centre so the towers stand
        // over the title and clear of the wide quest panel.
        : shot === 'quests' ? { clip: QUESTS_REEL[0], reel: QUESTS_REEL, frame: '85% 50%' }
        : shot === 'shop' ? { still: shopStill, drift: true }
          : shot === 'profile' ? { still: 'downtown', drift: true }
            : { still: 'settings' };
  const active = renderActive({
    visible: windowState.visible,
    focused: windowState.focused,
    idle,
    launchBusy: isBusy(launch) && covered,
    gameRunning,
    launchedOverlay: covered && launch.phase === 'launched',
  });
  // The footage keeps playing under the launch (it is the destination; hardware video decode is
  // cheap) and pauses with the rest once the game runs, the window hides or sits idle.
  const footageActive = renderActive({
    visible: windowState.visible,
    focused: windowState.focused,
    idle,
    launchBusy: false,
    gameRunning,
    launchedOverlay: covered && launch.phase === 'launched',
  });

  // Boot: black until the first picture has decoded (or 5 s), then the cover lifts.
  const ready = useCallback(() => setBooted(true), []);
  useEffect(() => {
    const t = setTimeout(ready, 5000);
    return () => clearTimeout(t);
  }, [ready]);

  // A new server chosen: the emblem's enamel gives a short pulse as the footage crossfades.
  const lastSelected = useRef(app.selectedServerId);
  useEffect(() => {
    const prev = lastSelected.current;
    lastSelected.current = app.selectedServerId;
    if (prev === null || prev === app.selectedServerId || !booted || reducedMotion || shot !== 'play') return;
    const tl = gsap.timeline()
      .to(charge, { current: 0.42, duration: 0.06, ease: 'power2.out' })
      .to(charge, { current: 0, duration: 0.3, ease: 'power2.in' });
    return () => { tl.kill(); charge.current = 0; };
  }, [app.selectedServerId, booted, reducedMotion, shot]);

  // PLAY, the memorable moment: the emblem charges gold-white, the footage pushes in 5% and keeps
  // playing, the screen gives way, and the launch comes up over that same shot.
  const onPlay = useCallback(() => {
    if (isBusy(launch)) return;
    setLaunchStartedAt(Date.now());
    app.play();
    setLaunchKey((k) => k + 1);
    const bd = backdropEl.current;
    const sc = screenEl.current;
    if (reducedMotion || !bd || !sc) { setCovered(true); return; }
    playLaunch(460, 0.45);
    gsap.timeline()
      .to(charge, { current: 1, duration: 0.46, ease: 'power2.in' }, 0)
      // Explicit start value: GSAP interpolates from what it is given, not from `transform: none`.
      .fromTo(bd, { scale: 1 }, { scale: 1.05, duration: 1.4, ease: 'power2.out' }, 0)
      .to(sc, { opacity: 0, duration: 0.3, ease: 'power1.in' }, 0.32)
      .call(() => { setCovered(true); charge.current = 0; }, undefined, 0.64)
      // The screen is restored under the launch, after React has painted it; the push holds.
      .set(sc, { clearProps: 'opacity' }, 1.0);
  }, [app, launch, reducedMotion]);

  // Back from the launch log to the screen.
  const uncover = useCallback(() => setCovered(false), []);
  // Uncovered: the push eases back out.
  useEffect(() => {
    const bd = backdropEl.current;
    if (covered || !bd) return;
    const tw = gsap.to(bd, { scale: 1, duration: 0.6, ease: 'power2.inOut', clearProps: 'transform' });
    return () => { tw.kill(); };
  }, [covered]);
  useEffect(() => {
    if (launch.phase === 'idle' && covered) setCovered(false);
  }, [launch.phase, covered]);

  return (
    <div className={`app app--${shot} ${booted ? 'is-booted' : ''} ${covered && launch.phase !== 'idle' ? 'is-covered' : ''}`}>
      <Backdrop ref={backdropEl} scene={scene} active={footageActive} reducedMotion={reducedMotion} onReady={ready} />
      <div className="scrim" aria-hidden />
      <Rail locked={(covered && launch.phase !== 'idle') || app.updateView.open} />
      <main className="screen" key={shot} ref={screenEl}>
        {auth.status !== 'signedIn' ? <AuthScreen /> : (
          <>
            {screen === 'play' && <PlayScreen onPlay={onPlay} />}
            {screen === 'shop' && <ShopScreen onStill={setShopStill} />}
            {screen === 'quests' && <QuestsScreen />}
            {screen === 'profile' && <ProfileScreen />}
            {screen === 'settings' && <SettingsScreen />}
          </>
        )}
      </main>
      <Suspense fallback={null}>
        {/* The emblem stays through the launch (the overlay carries it beside the server name)
            and stops with the footage once the launch hands off to the game. */}
        {EMBLEM_3D && <EmblemLayer charge={charge} active={covered ? footageActive : active} reducedMotion={reducedMotion} />}
      </Suspense>
      {covered && launch.phase !== 'idle' && <LaunchOverlay key={launchKey} onBack={uncover} startedAt={launchStartedAt} />}
      {/* The forced-update popup: over the screen and the launch, under the rail's window buttons. */}
      <UpdatePopup />
      {!booted && (
        <div className="boot" aria-label="Starting Solar Launcher">
          <GageMark size={30} />
        </div>
      )}
    </div>
  );
}
