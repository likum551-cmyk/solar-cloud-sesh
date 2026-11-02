import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { displayName, modeTag, serverKey } from '../lib/format.js';
import { pingStateOf } from '../lib/usePingQueue.js';
import { IconStar } from './icons.jsx';
import RowMenu from './RowMenu.jsx';
import { ListFoot, PingCell, useWholeRows } from './ui.jsx';

/** True while `ref`'s panel is too narrow for the address column (the job card still shows it). */
export function useCompact(ref) {
  const [compact, setCompact] = useState(false);
  useEffect(() => {
    const el = ref.current;
    if (!el) return undefined;
    const ro = new ResizeObserver(() => setCompact(el.clientWidth < 640));
    ro.observe(el);
    return () => ro.disconnect();
  }, [ref]);
  return compact;
}

// The list's geometry in CSS px (styles.css .schedule / .row): the header
// row, then rows at a 38 pitch, each carrying a 2px gap above its 36px body.
const GEOMETRY = { head: 30, pitch: 38 };

function Head({ label, colKey, sort, onSort, align = 'left' }) {
  const active = !!sort && sort.key === colKey;
  return (
    <th
      scope="col"
      className={align === 'right' ? 'text-right' : 'text-left'}
      aria-sort={onSort ? (active ? (sort.dir === 'asc' ? 'ascending' : 'descending') : 'none') : undefined}
    >
      {onSort ? (
        // the arrow hangs outside the label, so every label sits on its column's edge
        <button type="button" onClick={() => onSort(colKey)} className={`sort ${align === 'right' ? 'is-end' : ''} ${active ? 'is-active' : ''}`}>
          {label}
          <svg viewBox="0 0 10 10" className={`sort-arrow ${active ? '' : 'invisible'} ${active && sort.dir === 'desc' ? 'rotate-180' : ''}`} aria-hidden="true">
            <path d="M5 1 9 8H1z" fill="currentColor" />
          </svg>
        </button>
      ) : (
        <span className="sort">{label}</span>
      )}
    </th>
  );
}

/**
 * The menu list every tab shares: favourite star, name, address, players,
 * ping, in the same columns everywhere. Sorting and re-pinging are offered
 * only where the caller passes `onSort` / `onPing` (the live server list).
 * Arrows walk the rows, Space selects, Enter or a double-click connects; a
 * right-click (or the menu key) opens the row's action menu.
 */
export default function ServerGrid({
  label,
  rows,
  compact,
  sort,
  onSort,
  favoriteKeys,
  selectedKey,
  onSelect,
  onConnect,
  onToggleFavorite,
  onPing,
  pings,
  launching = false,
  onMenuChange,
  hideMode = false, // the list is filtered to one mode: the head already names it
}) {
  const fitRef = useRef(null);
  const wrapRef = useRef(null);
  const colRef = useRef(null);
  useLayoutEffect(() => {
    colRef.current = fitRef.current?.closest('.main') || null;
  }, []);
  // The list ends on a whole row, the foot right under it. The panel ends at
  // its content (styles .panel:has(> .panel-fit)), so the room is the main
  // column's height less what sits above the list (the panel's head), never
  // the list's own box, which would shrink with it.
  const fit = useWholeRows(
    [colRef, fitRef],
    () => {
      const fitEl = fitRef.current;
      const panel = fitEl.parentElement;
      const col = colRef.current;
      if (!col) return fitEl.clientHeight;
      return col.clientHeight - (panel.offsetHeight - fitEl.offsetHeight);
    },
    rows.length,
    GEOMETRY,
  );
  const picked = rows.findIndex((s) => serverKey(s) === selectedKey);
  // the right-click menu: which row, where (viewport px), and the row to refocus
  const [menu, setMenu] = useState(null);
  const menuServer = menu ? rows.find((s) => serverKey(s) === menu.key) : null;
  const menuOpen = !!menuServer;

  // the footer swaps to the menu's own prompts while it is open
  useEffect(() => {
    onMenuChange?.(menuOpen);
  }, [menuOpen, onMenuChange]);
  useEffect(() => () => onMenuChange?.(false), [onMenuChange]);

  // the menu hangs from the row's bottom edge at `x` (it never covers its own
  // row's text), or sits on its top edge when there is no room below
  function openMenu(s, i, x, rowEl) {
    onSelect(s);
    const r = rowEl.getBoundingClientRect();
    setMenu({ key: serverKey(s), index: i, x, y: r.bottom, above: r.top, rowEl });
  }
  // `refocus`: hand the keyboard back to the list (the row, or the row now in
  // its place when the action removed it, e.g. unfavoriting on Favorites)
  function closeMenu(refocus) {
    const m = menu;
    setMenu(null);
    if (!refocus || !m) return;
    window.requestAnimationFrame(() => {
      const body = fitRef.current?.querySelector('tbody');
      const target = m.rowEl?.isConnected ? m.rowEl : body?.children[Math.min(m.index, body.children.length - 1)];
      target?.focus({ preventScroll: true });
    });
  }

  // The mode words show only when every row's name fits beside its tags;
  // otherwise the whole table drops them (never one row here and there, and
  // never a truncated name for them). Measured with the words laid out but
  // hidden, so dropping them cannot flip the answer back.
  const tableRef = useRef(null);
  const [modeFits, setModeFits] = useState(true);
  useLayoutEffect(() => {
    const table = tableRef.current;
    if (!table) return undefined;
    const GAP = 10; // styles .row-name-in gap
    const measure = () => {
      let fits = true;
      for (const cell of table.querySelectorAll('.row-name-in')) {
        const kids = [...cell.children];
        const need = kids.reduce((w, k) => w + (k.classList.contains('truncate') ? k.scrollWidth : k.offsetWidth), 0) + GAP * (kids.length - 1);
        if (need > cell.clientWidth + 0.5) {
          fits = false;
          break;
        }
      }
      setModeFits(fits);
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(table);
    return () => ro.disconnect();
  }, [rows, compact, hideMode]);

  // a pick made elsewhere (the overview card's "Lowest ping") brings its row
  // into view; only on a new pick, so re-sorts never pull the list around
  const pickedRef = useRef(picked);
  pickedRef.current = picked;
  useEffect(() => {
    const tr = fitRef.current?.querySelector('tbody')?.children[pickedRef.current];
    tr?.scrollIntoView({ block: 'nearest' });
    // the overview's button went with the overview: the keyboard lands on the row
    if (tr && (!document.activeElement || document.activeElement === document.body)) tr.focus({ preventScroll: true });
  }, [selectedKey]);

  // its server left the list (a search, a refresh): close it for good, so it
  // cannot reappear by itself when the server comes back
  useEffect(() => {
    if (menu && !menuServer) closeMenu(document.activeElement === document.body);
  });

  function onRowKey(e, s, i) {
    if (e.target !== e.currentTarget) return;
    if (e.key === 'ContextMenu' || (e.shiftKey && e.key === 'F10')) {
      // the keyboard's own menu key: open it under the row's name
      e.preventDefault();
      openMenu(s, i, e.currentTarget.getBoundingClientRect().left + 48, e.currentTarget);
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const step = e.key === 'ArrowDown' ? 1 : -1;
      const next = rows[i + step];
      if (!next) return;
      onSelect(next);
      const row = e.currentTarget.parentElement.children[i + step];
      row?.focus();
      row?.scrollIntoView({ block: 'nearest' });
    } else if (e.key === 'Enter') {
      e.preventDefault();
      if (!launching) onConnect(s);
    } else if (e.key === ' ') {
      e.preventDefault();
      onSelect(s);
    }
  }

  return (
    <div ref={fitRef} className="panel-fit">
      <div ref={wrapRef} className={`panel-scroll ${fit ? 'is-paged' : ''}`} style={fit ? { height: fit } : undefined}>
        <div>
          {/* grid: makes aria-selected on the rows valid and announced */}
          <table ref={tableRef} className={`schedule ${modeFits ? '' : 'no-mode'}`} role="grid" aria-label={label}>
            <colgroup>
              <col style={{ width: 40 }} />
              <col />
              {/* the widest IPv4 host:port ("255.255.255.255:65535", 13px) plus padding: the name gets the rest */}
              {!compact && <col style={{ width: 164 }} />}
              <col style={{ width: 116 }} />
              <col style={{ width: 128 }} />
            </colgroup>
            <thead>
              <tr>
                <th scope="col">
                  <span className="sr-only">Favorite</span>
                </th>
                <Head label="Name" colKey="name" sort={sort} onSort={onSort} />
                {!compact && <Head label="Address" colKey="address" sort={sort} onSort={onSort} />}
                <Head label="Players" colKey="players" sort={sort} onSort={onSort} align="right" />
                <Head label="Ping" colKey="ping" sort={sort} onSort={onSort} align="right" />
              </tr>
            </thead>
            <tbody>
              {rows.map((s, i) => {
                const key = serverKey(s);
                const fav = favoriteKeys.has(key);
                const selected = i === picked;
                const pingState = pingStateOf(key, pings);
                const tag = modeTag(s);
                return (
                  <tr
                    key={key}
                    tabIndex={selected || (picked < 0 && i === 0) ? 0 : -1}
                    onClick={() => onSelect(s)}
                    onDoubleClick={() => {
                      if (!launching) onConnect(s);
                    }}
                    onKeyDown={(e) => onRowKey(e, s, i)}
                    onContextMenu={(e) => {
                      e.preventDefault();
                      const r = e.currentTarget.getBoundingClientRect();
                      const atPointer = e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom;
                      if (atPointer) openMenu(s, i, e.clientX, e.currentTarget);
                      // keyboard-started (Windows sends one for the menu key / Shift+F10, often at 0,0):
                      // under the row's name, unless the keydown already opened it
                      else if (menu?.key !== key) openMenu(s, i, r.left + 48, e.currentTarget);
                    }}
                    aria-selected={selected}
                    className="row"
                    title="Click to select. Double-click or Enter to connect."
                  >
                    <td>
                      <button
                        type="button"
                        title={fav ? 'Remove from favorites (F)' : 'Add to favorites (F)'}
                        aria-label={fav ? `Remove ${displayName(s)} from favorites` : `Add ${displayName(s)} to favorites`}
                        aria-pressed={fav}
                        onClick={(e) => {
                          e.stopPropagation();
                          onToggleFavorite(s);
                        }}
                        // hidden row actions leave the tab order; the selected row's join it
                        tabIndex={selected ? 0 : -1}
                        className={`star ${fav ? 'is-on' : ''}`}
                      >
                        <IconStar className="h-[15px] w-[15px]" filled={fav} />
                      </button>
                    </td>
                    <td className="row-name">
                      {/* the same tags on every row (the mode words go table-wide, `modeFits`) */}
                      <span className="row-name-in">
                        <span className="truncate">{displayName(s)}</span>
                        {tag.lang && <span className="row-tag is-lang">{tag.lang}</span>}
                        {tag.mode && !hideMode && <span className="row-tag is-mode">{tag.mode}</span>}
                      </span>
                    </td>
                    {!compact && (
                      <td className="row-addr">
                        {s.host}:{s.port}
                      </td>
                    )}
                    <td className="text-right font-bold">
                      {s.players >= 0 ? s.players : '—'}
                      {s.maxPlayers > 0 && <span className="row-max">/{s.maxPlayers}</span>}
                    </td>
                    <td className="text-right">
                      {onPing ? (
                        <button
                          type="button"
                          title="Ping this server again"
                          aria-label={`Ping ${displayName(s)}`}
                          disabled={pingState === 'pending'}
                          onClick={(e) => {
                            e.stopPropagation();
                            onPing(s);
                          }}
                          tabIndex={selected ? 0 : -1}
                          className="ping-btn"
                        >
                          <PingCell ms={s.ping} state={pingState} />
                        </button>
                      ) : (
                        <span className="ping-btn">
                          <PingCell ms={s.ping} state={pingState} />
                        </span>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      </div>
      <ListFoot scrollRef={wrapRef} index={picked + 1} total={rows.length} step={fit ? fit - GEOMETRY.head : 0} />
      {menuServer && (
        <RowMenu
          server={menuServer}
          x={menu.x}
          y={menu.y}
          above={menu.above}
          isFavorite={favoriteKeys.has(menu.key)}
          pinging={pingStateOf(menu.key, pings) === 'pending'}
          launching={launching}
          onConnect={onConnect}
          onToggleFavorite={onToggleFavorite}
          onPing={onPing}
          onClose={closeMenu}
        />
      )}
    </div>
  );
}
