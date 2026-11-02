import { useEffect, useState } from 'react';
import { useApp } from '../state/AppState';
import { STAGES, stageLabel } from '../state/launch';
import { EmblemSlot } from '../ui/emblemSlot';
import { Chip, Code, Icon, Plate } from '../ui/primitives';

// After PLAY the screen gives way and the launch sits over the same shot of the chosen server,
// pushed in and still playing (it pauses once GTA V is running). Left: the server's name, the
// phase and the clock since PLAY, a five-segment progress rail with the current step at display
// size, then launched or failed with what to do next. The rail stays on top, so the window can
// still be minimised or closed to the tray.

/**
 * Time since PLAY was pressed (`startedAt`, epoch ms), ticking while the launch runs and
 * stopped at the moment it ends, so a finished or failed launch shows how long it took.
 */
function useElapsed(running: boolean, startedAt: number): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    setNow(Date.now());
    if (!running) return;
    const t = setInterval(() => setNow(Date.now()), 250);
    return () => clearInterval(t);
  }, [running]);
  return Math.max(0, now - startedAt);
}

const JOIN_LABEL = 'Checking you in with the Solar RP network';
/** The rail's visible segment names: join, then the four native stages. */
const RAIL_NAMES = ['Join', 'Files', 'Store', 'Game', 'Load'] as const;

export function LaunchOverlay({ onBack, startedAt }: { onBack: () => void; startedAt: number }) {
  const { launch, setScreen, dismissLaunch } = useApp();
  const running = launch.phase === 'joining' || launch.phase === 'launching';
  const elapsed = useElapsed(running, startedAt);
  if (launch.phase === 'idle') return null;
  const secs = Math.floor(elapsed / 1000);
  const clock = `${String(Math.floor(secs / 60)).padStart(2, '0')}:${String(secs % 60).padStart(2, '0')}`;
  const stageIndex = launch.phase === 'launching' && launch.stage ? STAGES.indexOf(launch.stage) : launch.phase === 'launched' ? STAGES.length - 1 : -1;
  const steps = [{ key: 'join', label: JOIN_LABEL }, ...STAGES.slice(0, 4).map((s) => ({ key: s, label: stageLabel(s) }))];
  // step 0 is the join; steps 1..4 are the native stages
  const stepState = (i: number): '' | 'is-now' | 'is-done' => {
    if (launch.phase === 'launched') return 'is-done';
    if (i === 0) return launch.phase === 'joining' ? 'is-now' : 'is-done';
    const s = i - 1;
    return s < stageIndex ? 'is-done' : s === stageIndex ? 'is-now' : '';
  };
  const current = launch.phase === 'joining' ? JOIN_LABEL : launch.phase === 'launching' ? stageLabel(launch.stage) : null;
  // The host may add detail to a stage ("Steam is updating GTA V"); repeat nothing the step
  // already says, so "Waiting for GTA V to start" under "Waiting for GTA V" is dropped.
  const said = (current ?? '').toLowerCase();
  const detail = launch.phase === 'launching' && launch.statusText && !(said && launch.statusText.toLowerCase().startsWith(said)) ? launch.statusText : null;
  const phase = launch.phase === 'failed' ? 'Stopped' : launch.phase === 'launched' ? 'In game' : 'Launching';

  return (
    <section className={`launch launch--${launch.phase}`} role="dialog" aria-modal="true" aria-label="Launch" aria-live="polite">
      <div className="launch__inner">
        {/* Same row as the Play hero (emblem, then the name at the same size), so the title holds
            its place when the screen gives way. */}
        <div className="launch__title">
          <EmblemSlot className="hero__emblem" />
          <h1 className="launch__name">{launch.serverName}</h1>
        </div>
        <div className="launch__meta">
          <Chip tone={launch.phase === 'failed' ? 'void' : launch.phase === 'launched' ? 'done' : 'plain'}>{phase}</Chip>
          <Code tone="ink">{clock}</Code>
        </div>

        {launch.phase !== 'failed' && (
          <ol className="launch__rail" aria-label="Launch steps">
            {steps.map((st, i) => {
              const state = stepState(i);
              return (
                <li key={st.key} className={state}>
                  <span className="launch__seg" aria-hidden />
                  <span className="launch__segname" aria-hidden>{RAIL_NAMES[i]}</span>
                  <span className="sr-only">{st.label}{state === 'is-done' ? ', done' : state === 'is-now' ? ', in progress' : ''}</span>
                </li>
              );
            })}
          </ol>
        )}

        {running && (
          <>
            <p className="launch__step">{current}</p>
            {detail && <p className="launch__status">{detail}</p>}
            <p className="launch__fine">Minimise or close to the tray whenever you like; the launch carries on.</p>
          </>
        )}

        {launch.phase === 'launched' && (
          <>
            <p className="launch__lead">Solar RP is in GTA V and joining <strong>{launch.serverName}</strong>.</p>
            <p className="launch__fine">Not in after a minute? The in-game chat says what happened. This window can stay in the tray.</p>
            <div className="launch__actions">
              <Plate tone="ghost" onClick={() => { dismissLaunch(); onBack(); }}><Icon name="back" /> Back to the client</Plate>
            </div>
          </>
        )}

        {launch.phase === 'failed' && (
          <>
            <p className="launch__lead"><strong>{launch.title}.</strong> {launch.body}</p>
            <div className="launch__actions">
              {launch.action === 'gameFolder' && (
                <Plate tone="ghost" onClick={() => { dismissLaunch(); onBack(); setScreen('settings'); }}><Icon name="folder" /> Set GTA V folder</Plate>
              )}
              {launch.action === 'verifyEmail' && (
                <Plate tone="ghost" onClick={() => { dismissLaunch(); onBack(); setScreen('profile'); }}>Go to profile</Plate>
              )}
              <Plate tone="quiet" onClick={() => { dismissLaunch(); onBack(); }}><Icon name="back" /> Back</Plate>
            </div>
          </>
        )}
      </div>
    </section>
  );
}
