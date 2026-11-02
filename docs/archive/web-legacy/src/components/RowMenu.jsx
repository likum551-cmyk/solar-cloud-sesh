import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { getBridge } from '../bridge.js';
import { displayName } from '../lib/format.js';
import { Key } from './ui.jsx';

const EDGE = 6; // px kept clear of the window edge
const COPIED_MS = 650; // the copy row says so, then the menu closes
const COPY_TIMEOUT_MS = 2000; // no `copied` reply by then: say it failed

// #root's CSS zoom (main.jsx fitZoom): the menu is portalled to <body> and
// wears the same zoom, so its CSS px are pointer px divided by it.
function rootZoom() {
  const z = parseFloat(document.getElementById('root')?.style.zoom || '');
  return z > 0 ? z : 1;
}

const isWeb = (url) => /^https?:\/\//i.test(url || '');

/**
 * A server row's right-click menu, after GTA Online's interaction menu: a
 * freemode-blue strip naming the server, then the actions as rows that
 * reverse to white. Opens at (x, y) in viewport px. Up/Down walk it, Enter
 * picks, F toggles the favorite, Esc or a click outside closes it.
 */
export default function RowMenu({ server, x, y, above = y, isFavorite, pinging, launching, onConnect, onToggleFavorite, onPing, onClose }) {
  const ref = useRef(null);
  const [pos, setPos] = useState(null);
  const [copied, setCopied] = useState('');
  const closeTimer = useRef(0);
  const copyCleanup = useRef(() => {});
  const copyBusy = useRef(false);
  useEffect(() => () => copyCleanup.current(), []);
  const zoom = rootZoom();

  const address = `${server.host}:${server.port}`;
  const items = [
    { id: 'connect', label: launching ? 'Launching' : 'Connect', key: 'Enter', disabled: launching, run: () => onConnect(server) },
    { id: 'favorite', label: isFavorite ? 'Remove from favorites' : 'Add to favorites', key: 'F', run: () => onToggleFavorite(server) },
    onPing && { id: 'ping', label: pinging ? 'Pinging' : 'Ping again', disabled: pinging, run: () => onPing(server) },
    { id: 'copy', label: copied === 'ok' ? 'Address copied' : copied === 'fail' ? 'Copy failed' : 'Copy address', keep: true, run: copy },
    isWeb(server.url) && { id: 'web', label: 'Open website', run: () => window.open(server.url, '_blank', 'noopener') },
  ].filter(Boolean);

  // native puts it on the Windows clipboard (BRIDGE.md copyText) and answers
  // `copied`; no page clipboard permission involved
  function copy() {
    if (copyBusy.current) return; // one copy per menu
    copyBusy.current = true;
    const bridge = getBridge();
    let settled = false;
    const done = (ok) => {
      if (settled) return;
      settled = true;
      off();
      window.clearTimeout(noReply);
      setCopied(ok ? 'ok' : 'fail');
      closeTimer.current = window.setTimeout(() => closeRef.current(true), COPIED_MS);
    };
    const off = bridge.onMessage((d) => {
      if (d?.type === 'copied') done(!!d.ok);
    });
    const noReply = window.setTimeout(() => {
      console.error('row menu: the launcher did not answer copyText');
      done(false);
    }, COPY_TIMEOUT_MS);
    copyCleanup.current = () => {
      off();
      window.clearTimeout(noReply);
    };
    bridge.send({ type: 'copyText', text: address });
  }

  function pick(item) {
    if (item.disabled) return;
    item.run();
    // the list keeps the keyboard, except after Connect (the launch takes over)
    if (!item.keep) onClose(item.id !== 'connect');
  }

  // hang it from y (the row's bottom edge); with no room below, stand it on
  // `above` (the row's top edge); flip left of x at the window's right edge
  useLayoutEffect(() => {
    const r = ref.current.getBoundingClientRect(); // visual px
    const left = x + r.width + EDGE > window.innerWidth ? Math.max(EDGE, x - r.width) : x;
    let top = y;
    if (y + r.height + EDGE > window.innerHeight) {
      top = above - r.height >= EDGE ? above - r.height : Math.max(EDGE, window.innerHeight - r.height - EDGE);
    }
    setPos({ left: left / zoom, top: top / zoom });
  }, [x, y, above, zoom]);

  // first action takes focus once placed
  useEffect(() => {
    if (pos) ref.current.querySelector('[role="menuitem"]:not(:disabled)')?.focus();
  }, [pos]);

  // anything else the pointer or window does closes it (the list re-renders
  // under it on every ping batch, so the handler is read through a ref)
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    // a click elsewhere takes focus where it lands; anything else hands it back to the row
    const outside = (e) => {
      if (!ref.current.contains(e.target)) closeRef.current(false);
    };
    const close = () => closeRef.current(true);
    window.addEventListener('pointerdown', outside, true);
    window.addEventListener('wheel', close, { passive: true });
    window.addEventListener('resize', close);
    window.addEventListener('blur', close);
    return () => {
      window.removeEventListener('pointerdown', outside, true);
      window.removeEventListener('wheel', close);
      window.removeEventListener('resize', close);
      window.removeEventListener('blur', close);
      window.clearTimeout(closeTimer.current);
    };
  }, []);

  function onKeyDown(e) {
    // keys the window acts on (Enter, F, Q/E, Ctrl+F) belong to the menu while it is open
    e.stopPropagation();
    const rows = [...ref.current.querySelectorAll('[role="menuitem"]:not(:disabled)')];
    const at = rows.indexOf(document.activeElement);
    let next = null;
    if (e.key === 'ArrowDown') next = rows[(at + 1) % rows.length];
    else if (e.key === 'ArrowUp') next = rows[(at - 1 + rows.length) % rows.length];
    else if (e.key === 'Home') next = rows[0];
    else if (e.key === 'End') next = rows[rows.length - 1];
    else if (e.key === 'Escape' || e.key === 'Tab') {
      e.preventDefault();
      onClose(true);
      return;
    } else if (e.key === 'f' || e.key === 'F') {
      e.preventDefault();
      pick(items.find((i) => i.id === 'favorite'));
      return;
    }
    if (next) {
      e.preventDefault();
      next.focus();
    }
  }

  return createPortal(
    <div
      ref={ref}
      className="row-menu app-no-drag"
      role="menu"
      aria-label={`${displayName(server)} actions`}
      onKeyDown={onKeyDown}
      onContextMenu={(e) => e.preventDefault()}
      style={{ zoom: zoom > 1 ? zoom : undefined, left: pos?.left ?? x / zoom, top: pos?.top ?? y / zoom, visibility: pos ? 'visible' : 'hidden' }}
    >
      <div className="row-menu-head">
        <span className="truncate">{displayName(server)}</span>
        <span className="row-menu-addr">{address}</span>
      </div>
      {items.map((item) => (
        <button
          key={item.id}
          type="button"
          role="menuitem"
          className="row-menu-item"
          disabled={item.disabled}
          aria-keyshortcuts={item.key}
          onMouseEnter={(e) => e.currentTarget.focus()}
          onClick={() => pick(item)}
        >
          <span className="truncate">{item.label}</span>
          {/* the cap is for the eye; the shortcut is announced by aria-keyshortcuts */}
          {item.key && (
            <span aria-hidden="true">
              <Key>{item.key}</Key>
            </span>
          )}
        </button>
      ))}
    </div>,
    document.body,
  );
}
