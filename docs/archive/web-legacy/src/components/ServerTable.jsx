import { useEffect, useMemo, useRef } from 'react';
import { displayName, matchesQuery, modeKey, modeLabel } from '../lib/format.js';
import { IconRefresh, IconSearch } from './icons.jsx';
import { PanelMessage } from './PanelMessage.jsx';
import ServerGrid, { useCompact } from './ServerGrid.jsx';
import { Chevron } from './ui.jsx';

const plural = (n) => `${n} ${n === 1 ? 'server' : 'servers'}`;

/**
 * The list head's game-mode selector, a pause-menu option: `‹ Roleplay 8 ›`.
 * The arrows (or Left/Right while it has focus) step through the modes the
 * list carries, busiest first; "All modes" is the stop between the ends.
 */
function ModePicker({ modes, value, onChange, total }) {
  const at = Math.max(0, modes.findIndex((m) => m.key === value));
  const options = [{ key: '', count: total }, ...modes];
  const i = value ? at + 1 : 0;
  const step = (d) => onChange(options[(i + d + options.length) % options.length].key);
  const cur = options[i];
  return (
    <div
      className="mode-pick"
      role="group"
      aria-label="Game mode"
      onKeyDown={(e) => {
        if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
          e.preventDefault();
          step(e.key === 'ArrowLeft' ? -1 : 1);
        }
      }}
    >
      <button type="button" className="mode-step" onClick={() => step(-1)} aria-label="Previous game mode" title="Previous game mode (Left)">
        <Chevron />
      </button>
      <span className="mode-cur" aria-live="polite">
        <span className="truncate">{cur.key ? modeLabel(cur.key) : 'All modes'}</span>
        <span className="mode-count">{cur.count}</span>
      </span>
      <button type="button" className="mode-step" onClick={() => step(1)} aria-label="Next game mode" title="Next game mode (Right)">
        <Chevron flip />
      </button>
    </div>
  );
}

/** The live server list: search, sort, re-ping, and its loading / empty / offline states. */
export default function ServerTable({
  servers,
  status,
  error,
  query,
  onQuery,
  sort,
  onSort,
  favoriteKeys,
  selectedKey,
  onSelect,
  onConnect,
  onToggleFavorite,
  onPing,
  pings,
  onRetry,
  onDirect,
  launching = false,
  onMenuChange,
  mode: pickedMode = '', // the game-mode filter (App owns it: the overview card follows it)
  onMode: setMode,
}) {
  const loading = status === 'loading';
  const failed = status === 'error';
  const sectionRef = useRef(null);
  const compact = useCompact(sectionRef);

  // the modes this list carries, busiest first; a mode the list no longer
  // has (a refresh) falls back to all
  const modes = useMemo(() => {
    const by = new Map();
    for (const s of servers) {
      const k = modeKey(s);
      if (k) by.set(k, (by.get(k) || 0) + 1);
    }
    return [...by].map(([key, count]) => ({ key, count })).sort((a, b) => b.count - a.count || a.key.localeCompare(b.key));
  }, [servers]);
  const mode = modes.some((m) => m.key === pickedMode) ? pickedMode : '';

  const visible = useMemo(() => {
    const list = servers.filter((s) => (!mode || modeKey(s) === mode) && matchesQuery(s, query));
    const dir = sort.dir === 'asc' ? 1 : -1;
    const arr = [...list];
    if (sort.key === 'players' || sort.key === 'ping') {
      // unknown values (-1) always sink to the bottom, in either direction
      const known = (v) => v != null && v >= 0;
      arr.sort((a, b) => {
        const av = a[sort.key];
        const bv = b[sort.key];
        if (!known(av) || !known(bv)) return known(av) ? -1 : known(bv) ? 1 : 0;
        return (av - bv) * dir;
      });
    } else if (sort.key === 'name' || sort.key === 'address') {
      // by the name as shown: list names arrive padded (" GALAXY DM")
      const val = (s) => (sort.key === 'name' ? displayName(s) : `${s.host}:${s.port}`);
      arr.sort((a, b) => val(a).localeCompare(val(b), 'en', { sensitivity: 'base', numeric: true }) * dir);
    }
    return arr;
  }, [servers, query, sort, mode]);

  // The host turns off browser accelerators (no find bar, no reload), so the
  // list claims the familiar keys: Ctrl+F or / to search, F5 or Ctrl+R to refresh.
  const searchRef = useRef(null);
  // search and the head's refresh only mean something over a list; without
  // one the panel's own message carries Retry / Refresh
  const live = status === 'ok' && servers.length > 0;
  const searchable = live || !!query;
  const keys = useRef({ onRetry, loading, searchable });
  keys.current = { onRetry, loading, searchable };
  useEffect(() => {
    const onKey = (e) => {
      const typing = e.target instanceof Element && e.target.closest('input, textarea');
      const k = e.key.toLowerCase();
      if ((e.ctrlKey && k === 'f') || (k === '/' && !typing && !e.ctrlKey && !e.altKey)) {
        e.preventDefault();
        if (!keys.current.searchable) return;
        searchRef.current?.focus();
        searchRef.current?.select();
      } else if (e.key === 'F5' || (e.ctrlKey && k === 'r')) {
        e.preventDefault();
        if (!keys.current.loading) keys.current.onRetry();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  // the tab names the list and the overview card counts it; the head speaks
  // only to say how much of it a search left
  const count = live && query ? `${visible.length} of ${plural(servers.length)}` : '';

  return (
    <section ref={sectionRef} className="panel app-no-drag" aria-label="Server list">
      <h2 className="sr-only">Servers</h2>
      {/* no list (fetching, offline, empty): no search bar over nothing; the message fills the panel */}
      {searchable && (
        <div className="panel-head">
          {live && modes.length > 1 && <ModePicker modes={modes} value={mode} onChange={setMode} total={servers.length} />}
          <span className="panel-count" aria-live="polite">
            {count}
          </span>
          <div className="ml-auto flex items-center gap-1.5">
            <label className="search">
              <span className="sr-only">Search servers</span>
              <IconSearch className="search-icon h-3.5 w-3.5" />
              <input
                ref={searchRef}
                value={query}
                onChange={(e) => onQuery(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Escape') onQuery('');
                }}
                title="Search servers (Ctrl+F)"
                placeholder="Name, address or mode"
                spellCheck="false"
                className="field"
              />
            </label>
            {live && (
              <button type="button" onClick={onRetry} title="Refresh the server list (F5)" aria-label="Refresh the server list" className="icon-btn">
                <IconRefresh className="h-4 w-4" />
              </button>
            )}
          </div>
        </div>
      )}

      {loading ? (
        <PanelMessage busy title="Loading servers" />
      ) : failed ? (
        <PanelMessage
          tone="stop"
          title="Server list unavailable"
          detail={error || 'Could not load the server list.'}
          hint="Check your connection and retry, or connect to a server by address."
          actions={[
            { label: 'Retry', keyHint: 'F5', onClick: onRetry },
            { label: 'Connect directly', onClick: onDirect },
          ]}
        />
      ) : visible.length === 0 ? (
        query ? (
          <PanelMessage
            title="No match"
            hint={
              mode
                ? `No ${modeLabel(mode)} server matches “${query}”.`
                : `Nothing matches “${query}”. Try part of a name, an address or a game mode.`
            }
            actions={[
              { label: 'Clear search', keyHint: 'Esc', onClick: () => onQuery('') },
              ...(mode ? [{ label: 'All modes', onClick: () => setMode('') }] : []),
            ]}
          />
        ) : (
          <PanelMessage
            title="No servers listed"
            hint="The list came back empty."
            actions={[
              { label: 'Refresh', keyHint: 'F5', onClick: onRetry },
              { label: 'Connect directly', onClick: onDirect },
            ]}
          />
        )
      ) : (
        <ServerGrid
          label="Servers"
          rows={visible}
          compact={compact}
          sort={sort}
          onSort={onSort}
          favoriteKeys={favoriteKeys}
          selectedKey={selectedKey}
          onSelect={onSelect}
          onConnect={onConnect}
          onToggleFavorite={onToggleFavorite}
          onPing={onPing}
          pings={pings}
          launching={launching}
          onMenuChange={onMenuChange}
          hideMode={!!mode}
        />
      )}
    </section>
  );
}
