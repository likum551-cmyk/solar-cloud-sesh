import { useEffect, useRef, useState } from 'react';
import { IconFolder } from './icons.jsx';
import { Key, ListFoot, Spinner, Toggle, useWholeRows } from './ui.jsx';

// If native never answers `checkUpdate`, stop the spinner and say so.
const UPDATE_CHECK_TIMEOUT_MS = 15000;

// The description box says what the highlighted row does, as the game's
// settings do: its name, then the explanation.
const DESC = {
  gamePath: ['Game folder', 'The folder with GTA5.exe or GTA5_Enhanced.exe, or the exe itself. Browse opens a folder picker.'],
  playerName: ['Player name', 'The name other players see in game. Up to 32 characters.'],
  autoUpdate: ['Auto-update', 'Check for launcher updates every time GAGE-MP starts.'],
  startMinimized: ['Start minimized', 'Start hidden in the system tray instead of opening this window.'],
  closeGame: ['Close game on exit', 'Quit the game session this launcher started when you exit GAGE-MP from the tray.'],
  clearFavorites: ['Clear favorites', 'Remove every server from Favorites. Press twice: the first press arms it.'],
  clearRecents: ['Clear recents', 'Forget every server you connected to. Press twice: the first press arms it.'],
  version: ['Installed version', 'The GAGE-MP version installed on this PC.'],
  update: ['Check for updates', 'Ask the update server whether a newer GAGE-MP is out.'],
};

// Text setting committed on blur / Enter, never on every keystroke.
function useCommitted(initial, commit) {
  const [value, setValue] = useState(initial || '');
  useEffect(() => setValue(initial || ''), [initial]);
  const onCommit = () => {
    const v = value.trim();
    if (v !== (initial || '')) commit(v);
  };
  return [value, setValue, onCommit];
}

// Destructive, so two steps: the first press arms it, the second clears.
function ClearRow({ label, count, onClear, onFocusRow, current }) {
  const [armed, setArmed] = useState(false);
  const timer = useRef(0);
  useEffect(() => () => clearTimeout(timer.current), []);
  return (
    <button
      type="button"
      className={`opt ${armed ? 'is-armed' : ''} ${current ? 'is-current' : ''}`}
      disabled={count === 0}
      onFocus={onFocusRow}
      onMouseEnter={onFocusRow}
      onBlur={() => setArmed(false)}
      onClick={() => {
        if (armed) {
          clearTimeout(timer.current);
          setArmed(false);
          onClear();
        } else {
          setArmed(true);
          timer.current = setTimeout(() => setArmed(false), 3500);
        }
      }}
    >
      <span className="opt-label">{label}</span>
      {/* the value names the action, so the row reads as one */}
      <span className="opt-value">{count === 0 ? 'Empty' : armed ? 'Press again to clear' : `Clear ${count} ${count === 1 ? 'server' : 'servers'}`}</span>
    </button>
  );
}

// option rows: 36px each with a 2px gap between (styles.css .opts / .opt)
const GEOMETRY = { pitch: 38, gap: 2 };

/**
 * The Settings tab, as the game's: one wide list of option rows, each value
 * in a track at the right, and under it the description box for the
 * highlighted row (and any error). Up/Down walk the rows, Left/Right flip a
 * toggle. A list taller than the window pages by whole rows.
 */
export default function SettingsView({ settings, updateInfo, send, callGamePath = 0, favoritesCount = 0, recentsCount = 0 }) {
  const [checking, setChecking] = useState(false);
  const [checkError, setCheckError] = useState('');
  const [desc, setDesc] = useState('gamePath');
  const checkTimer = useRef(null);
  const mainRef = useRef(null);
  const descRef = useRef(null);
  const wrapRef = useRef(null);
  const bodyRef = useRef(null);

  // Sent here by a launch failure: the game folder takes focus, its text
  // selected so typing replaces it.
  const gamePathRef = useRef(null);
  useEffect(() => {
    if (!callGamePath) return;
    gamePathRef.current?.focus();
    gamePathRef.current?.select();
  }, [callGamePath]);

  const save = (key) => (value) => send({ type: 'saveSetting', key, value });
  const [gamePath, setGamePath, commitGamePath] = useCommitted(settings?.gamePath, save('gamePath'));
  const [playerName, setPlayerName, commitPlayerName] = useCommitted(settings?.playerName, save('playerName'));

  // App.jsx stores each native `update` message as a NEW updateInfo object, so
  // a changed identity means a reply arrived: end the check and its watchdog.
  useEffect(() => {
    if (!updateInfo) return;
    window.clearTimeout(checkTimer.current);
    setChecking(false);
    setCheckError('');
  }, [updateInfo]);
  useEffect(() => () => window.clearTimeout(checkTimer.current), []);

  function checkUpdate() {
    if (checking) return;
    window.clearTimeout(checkTimer.current);
    setCheckError('');
    setChecking(true);
    checkTimer.current = window.setTimeout(() => {
      setChecking(false);
      setCheckError('No response from the launcher. The update check timed out.');
    }, UPDATE_CHECK_TIMEOUT_MS);
    send({ type: 'checkUpdate' });
  }

  // Up/Down move between rows (a field's row or an option button), as in the game.
  function onListKey(e) {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    const stops = [...bodyRef.current.querySelectorAll('.opt input, button.opt:not(:disabled)')];
    const at = stops.findIndex((el) => el === e.target || el.contains(e.target));
    if (at < 0) return;
    const next = stops[at + (e.key === 'ArrowDown' ? 1 : -1)];
    if (!next) return;
    e.preventDefault();
    next.focus();
    next.scrollIntoView({ block: 'nearest' });
  }

  const notice = checkError ? { error: checkError } : updateInfo;
  const enter = (e) => {
    if (e.key === 'Enter') e.currentTarget.blur();
  };
  const focusRow = (k) => () => setDesc(k);
  // the row the description box speaks for is the one drawn reversed
  const cur = (k) => (desc === k ? 'is-current' : '');
  const updateState = checking
    ? 'Checking'
    : !notice
      ? ''
      : notice.error
        ? 'Check failed'
        : notice.available
          ? `Update available: ${notice.latest}`
          : `Up to date${notice.latest ? ` (${notice.latest})` : ''}`;
  const [descTitle, descText] = DESC[desc];
  const rowCount = 9;
  // room for the list: the column less the description box and its gap
  const listH = useWholeRows(
    [mainRef, descRef],
    () => mainRef.current.clientHeight - descRef.current.offsetHeight - 12,
    rowCount,
    GEOMETRY,
  );

  return (
    <main ref={mainRef} className="main is-settings">
      <section className="panel app-no-drag" aria-label="Settings">
        <div ref={wrapRef} className={`panel-scroll ${listH ? 'is-paged' : ''}`} style={listH ? { height: listH } : undefined}>
          {/* Up/Down here move between the rows' own controls */}
          <div ref={bodyRef} className="opts" onKeyDown={onListKey}>
            <div className={`opt is-field ${cur('gamePath')}`} onMouseEnter={focusRow('gamePath')}>
              <label htmlFor="game-path" className="opt-label">
                Game folder
              </label>
              <span className="opt-value">
                <input
                  id="game-path"
                  ref={gamePathRef}
                  value={gamePath}
                  onChange={(e) => setGamePath(e.target.value)}
                  onBlur={commitGamePath}
                  onFocus={focusRow('gamePath')}
                  onKeyDown={enter}
                  placeholder="Folder that contains GTA5.exe"
                  spellCheck="false"
                  title={gamePath || undefined}
                  // at rest a long path shows its end (the game's own folder), clipped at the left
                  className="opt-input is-path"
                />
                <button type="button" onClick={() => send({ type: 'browseGamePath' })} onFocus={focusRow('gamePath')} className="opt-btn">
                  <IconFolder className="h-3.5 w-3.5" />
                  Browse
                </button>
              </span>
            </div>
            <div className={`opt is-field ${cur('playerName')}`} onMouseEnter={focusRow('playerName')}>
              <label htmlFor="player-name" className="opt-label">
                Player name
              </label>
              <span className="opt-value">
                <input
                  id="player-name"
                  value={playerName}
                  onChange={(e) => setPlayerName(e.target.value)}
                  onBlur={commitPlayerName}
                  onFocus={focusRow('playerName')}
                  onKeyDown={enter}
                  placeholder="PlayerOne"
                  spellCheck="false"
                  maxLength={32}
                  className="opt-input"
                />
              </span>
            </div>

            <Toggle label="Auto-update" checked={!!settings?.autoUpdate} onChange={save('autoUpdate')} onFocusRow={focusRow('autoUpdate')} current={desc === 'autoUpdate'} />
            <Toggle label="Start minimized" checked={!!settings?.startMinimized} onChange={save('startMinimized')} onFocusRow={focusRow('startMinimized')} current={desc === 'startMinimized'} />
            <Toggle label="Close game on exit" checked={!!settings?.closeGame} onChange={save('closeGame')} onFocusRow={focusRow('closeGame')} current={desc === 'closeGame'} />

            <ClearRow label="Favorites" count={favoritesCount} onClear={() => send({ type: 'clearFavorites' })} onFocusRow={focusRow('clearFavorites')} current={desc === 'clearFavorites'} />
            <ClearRow label="Recents" count={recentsCount} onClear={() => send({ type: 'clearRecents' })} onFocusRow={focusRow('clearRecents')} current={desc === 'clearRecents'} />

            <div className={`opt ${cur('version')}`} onMouseEnter={focusRow('version')}>
              <span className="opt-label">Installed version</span>
              <span className="opt-value">{settings?.version || '—'}</span>
            </div>
            <button type="button" className={`opt ${cur('update')}`} onClick={checkUpdate} disabled={checking} onFocus={focusRow('update')} onMouseEnter={focusRow('update')}>
              <span className="opt-label">Check for updates</span>
              <span className={`opt-value ${notice?.error ? 'is-stop' : notice?.available ? 'is-gold' : ''}`} role="status">
                {checking && <Spinner className="h-3.5 w-3.5" />}
                {/* idle: the row says what it does; highlighted, which key does it */}
                {updateState || (
                  <>
                    <span className="opt-idle">Not checked</span>
                    <span className="opt-key">
                      <Key>Enter</Key>
                    </span>
                  </>
                )}
              </span>
            </button>
          </div>
        </div>
        <ListFoot scrollRef={wrapRef} step={listH ? listH + GEOMETRY.gap : 0} />
      </section>

      {/* the game's description box, docked under the list: what the
          highlighted row does, then whatever needs fixing, in red */}
      <section ref={descRef} className="desc app-no-drag" aria-label="Description" aria-live="polite">
        <h2 className="desc-title">{descTitle}</h2>
        <p className="desc-text">{descText}</p>
        {notice?.error && <p className="desc-error">{notice.error}</p>}
      </section>
    </main>
  );
}
