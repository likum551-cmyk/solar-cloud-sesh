import { useEffect, useLayoutEffect, useRef } from 'react';
import { displayName } from '../lib/format.js';
import { Pricedown, splashFor } from './gta.jsx';
import { failureText } from './SidePanel.jsx';
import { Key, Spinner } from './ui.jsx';

const fmt = (n) => n.toLocaleString('en-US');

// One unit system on this screen: binary megabytes, labelled MB as Windows does.
const MiB = 1048576;

function fmtBytes(b) {
  if (!b) return '0 MB';
  const mb = b / MiB;
  if (mb >= 1000) return `${(mb / 1024).toFixed(mb >= 10240 ? 0 : 1)} GB`;
  if (mb >= 100) return `${Math.round(mb)} MB`;
  return `${mb.toFixed(1)} MB`;
}

// short enough to keep the line whole in a narrow foot
function fmtLeft(sec) {
  if (sec < 60) return `${Math.max(10, Math.ceil(sec / 10) * 10)} s left`;
  if (sec < 90 * 60) return `${Math.ceil(sec / 60)} min left`;
  return `${Math.round(sec / 3600)} h left`;
}

// three significant digits, so the line's width holds as the speed changes
function fmtRate(r) {
  return r >= 100 ? r.toFixed(0) : r >= 10 ? r.toFixed(1) : r.toFixed(2);
}

// the heading already says "Starting GTA V for ...": native's steps here say
// what is happening without repeating it, and without a closing full stop
function stepText(s) {
  return s
    .replace(/^Starting GTA V through (.+)$/, 'Opening $1')
    .replace(/^Starting GTA V…$/, 'Starting the game…')
    .replace(/\.$/, '');
}

// a file this big gets its own line while it downloads
const BIG_FILE = 64 * MiB;

/** Share of the files on disk, counting how far the largest in-flight file has come. */
export function assetShare(p) {
  if (!(p.total > 0)) return null;
  const partial = p.bigSize > 0 ? Math.min(1, p.bigDone / p.bigSize) : 0;
  return Math.min(1, (p.files + partial) / p.total);
}

/** The counts' pieces: each keeps its words together; a narrow foot breaks between them. */
function Seg({ children }) {
  return <span className="loading-seg">{children}</span>;
}

/**
 * A line of pieces: a piece that wrapped to a new line hides the dot that
 * joins it to the one before (.is-wrap), as the dot would start the line.
 */
function SegLine({ className = '', children }) {
  const ref = useRef(null);
  useLayoutEffect(() => {
    const line = ref.current;
    if (!line) return undefined;
    const mark = () => {
      let top = null;
      for (const seg of line.children) {
        seg.classList.toggle('is-wrap', top !== null && seg.offsetTop > top + 1);
        top = seg.offsetTop;
      }
    };
    mark();
    const ro = new ResizeObserver(mark);
    ro.observe(line);
    return () => ro.disconnect();
  });
  return (
    <p ref={ref} className={`loading-line ${className}`}>
      {children}
    </p>
  );
}

/**
 * The server's own files, as the splash counts its pings: the counts under
 * the step's percent, the bottom edge filling with the share of files on disk
 * (running while native is still counting how many there are). Once they are
 * all in, bar and counts hold a beat and clear (.is-complete).
 */
function AssetCounts({ p }) {
  const known = p.total >= 0;
  return (
    <SegLine className={`loading-counts ${p.done ? 'is-complete' : ''}`}>
      <Seg>
        <span className="splash-num">{fmt(p.files)}</span>
        {p.done ? (
          ' files'
        ) : known ? (
          <>
            {' of '}
            {/* a recount holds the last total: more may come */}
            <span className="splash-num">{fmt(p.total)}{p.recount ? '+' : ''}</span> files
          </>
        ) : (
          ' files so far'
        )}
      </Seg>
      {/* the amount always moves, even while the file share barely does */}
      {(p.done ? p.bytes > 0 : p.bytes >= 0) && (
        <Seg>
          <span className="splash-num">{fmtBytes(p.bytes)}</span>
        </Seg>
      )}
      {!p.done && p.rate != null && (
        <Seg>
          <span className="splash-num">{fmtRate(p.rate)}</span> MB/s
        </Seg>
      )}
    </SegLine>
  );
}

/**
 * What the wait is on, from the sizes native reads off each reply: once every
 * remaining file is downloading, how much is left of them and about how long
 * at the current speed; before that, a large file's own progress.
 */
function AssetTail({ p }) {
  if (p.done || !(p.active > 0)) return null;
  const queued = p.total >= 0 ? p.total - p.files - p.active : Infinity;
  if (queued <= 0 && p.activeLeft > 0) {
    const left = p.rate > 0.05 ? fmtLeft(p.activeLeft / (p.rate * MiB)) : null;
    return (
      <SegLine className="loading-tail">
        <Seg>{p.active === 1 ? 'Last file' : `Last ${p.active} files`}</Seg>
        <Seg>
          <span className="splash-num">{fmtBytes(p.activeLeft)}</span> to go
        </Seg>
        {left && <Seg>{left}</Seg>}
      </SegLine>
    );
  }
  if (p.bigSize >= BIG_FILE) {
    return (
      <SegLine className="loading-tail">
        <Seg>Large file</Seg>
        <Seg>
          <span className="splash-num">{fmtBytes(p.bigDone)}</span> of{' '}
          <span className="splash-num">{fmtBytes(p.bigSize)}</span>
        </Seg>
      </SegLine>
    );
  }
  return null;
}

function AssetBar({ p }) {
  const known = p.total > 0;
  const share = assetShare(p);
  // what the screen shows: the percent (big file's part included) and the counts
  const said = known
    ? `${p.done ? 100 : Math.min(99, Math.floor(100 * share))}%, ${fmt(p.files)} of ${fmt(p.total)}${p.recount ? '+' : ''} files`
    : `${fmt(p.files)} files so far, still counting`;
  return (
    <div
      className={`splash-bar ${known ? '' : 'is-busy'} ${p.done ? 'is-complete' : ''}`}
      aria-hidden={p.done || undefined}
      role="progressbar"
      aria-label="Server files"
      aria-valuemin={0}
      aria-valuemax={known ? p.total : undefined}
      aria-valuenow={known ? p.files : undefined}
      aria-valuetext={said}
    >
      <span style={known ? { transform: `scaleX(${share})` } : undefined} />
    </div>
  );
}

/**
 * Joining a server is a GTA loading screen: the server's lead full length,
 * who you are joining at the foot, the busy spinner with native's progress.
 * Once the game is up, the game's own big message says so.
 */
export function LaunchScreen({ server, phase, statusText, assets, onBack }) {
  const art = splashFor(server);
  const done = phase === 'done';
  const fetching = assets && !assets.done;
  // native downloads while it counts, so both states say so; the bar's
  // running segment is what says the total is not known yet. A file found
  // past the counted end keeps the last total, the bar and the percent
  // where they were (the counts mark the total with a +).
  const busyText = fetching ? 'Downloading server files' : stepText(statusText || 'Starting the game');
  const pct = fetching && assets.total > 0 ? Math.min(99, Math.floor(100 * assetShare(assets))) : null;
  const backRef = useRef(null);
  useEffect(() => {
    if (!done) return undefined;
    backRef.current?.focus();
    const onKey = (e) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onBack();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [done, onBack]);

  return (
    <div className={`loading-screen ${done ? 'is-done' : ''}`} data-who={art.who} aria-label={done ? 'Game started' : 'Launching'}>
      <img className="loading-art" src={art.src} alt="" decoding="async" />
      <div className="loading-shade" aria-hidden="true" />
      <div className="loading-drag app-drag" aria-hidden="true" />
      {done && (
        <div className="shard" aria-hidden="true">
          <Pricedown k="launched" size={74} className="shard-title" />
        </div>
      )}
      <div className="loading-foot">
        <div className="min-w-0">
          {/* gage_mp.dll does not join servers yet: once the game is up,
              say so instead of claiming the player is on the server */}
          {/* real names run 60-88 characters: two lines, then an ellipsis */}
          <p className="loading-name" title={done ? undefined : displayName(server)}>
            <span className="loading-verb">{done ? 'Game started' : 'Starting GTA V for'}</span>
            {!done && ` ${displayName(server)}`}
          </p>
          <p className="loading-sub">
            {done ? `Joining ${displayName(server)} is not in this build yet.` : `${server.host}:${server.port}`}
          </p>
        </div>
        {done ? (
          <button ref={backRef} type="button" className="hint is-loud" onClick={onBack}>
            <Key>Esc</Key>
            <span>Back to the menu</span>
          </button>
        ) : (
          <div className="loading-status">
            {/* rows that come and go sit on top, so the counts never move */}
            {assets && <AssetTail p={assets} />}
            {assets && <AssetCounts p={assets} />}
            <p className="loading-busy">
              <span className="loading-step">{busyText}</span>
              {pct !== null ? (
                <span className="loading-pct">
                  {pct}
                  <span className="loading-pct-sign">%</span>
                </span>
              ) : (
                // no width, but the percent's line: the rows above sit where they will stay
                <span className="loading-pct is-empty" aria-hidden="true">
                  0
                </span>
              )}
              <Spinner className="h-6 w-6 shrink-0" />
            </p>
          </div>
        )}
      </div>
      {!done && assets && <AssetBar p={assets} />}
    </div>
  );
}

/**
 * A failed launch is the game's alert screen: the menu dims to black, the
 * word in its lettering, what went wrong, and what to press.
 */
export function AlertScreen({ server, result, onRetry, onFix, onBack }) {
  const retryRef = useRef(null);
  const canFix = result.code === 1;
  useEffect(() => {
    retryRef.current?.focus();
    // preventDefault: the key must not land in the field the fix focuses
    const onKey = (e) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onBack();
      } else if (canFix && (e.key === 's' || e.key === 'S') && !e.ctrlKey && !e.altKey) {
        e.preventDefault();
        onFix();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [canFix, onBack, onFix]);

  return (
    <div className="alert" role="alertdialog" aria-modal="true" aria-labelledby="alert-title" aria-describedby="alert-body">
      <div className="alert-body">
        <h2 id="alert-title">
          <span className="sr-only">Alert: launch failed</span>
          <Pricedown k="alert" size={96} />
        </h2>
        <p id="alert-body" className="alert-text">
          {/* native's code-1 message already names the fix; S below acts on it */}
          {`${server ? displayName(server) : 'The game'} did not start. ${failureText(result)}`}
        </p>
        <div className="alert-rule" aria-hidden="true" />
        <div className="alert-actions">
          <button ref={retryRef} type="button" className="hint is-loud" onClick={onRetry} disabled={!server}>
            <Key>Enter</Key>
            <span>Retry</span>
          </button>
          {canFix && (
            <button type="button" className="hint is-loud" onClick={onFix}>
              <Key>S</Key>
              <span>Set game folder</span>
            </button>
          )}
          <button type="button" className="hint is-loud" onClick={onBack}>
            <Key>Esc</Key>
            <span>Back</span>
          </button>
        </div>
      </div>
    </div>
  );
}
