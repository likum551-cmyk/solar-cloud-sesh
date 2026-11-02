import { useRef, useState } from 'react';
import { serverKey } from './format.js';

// Native pings every fresh list itself, busiest servers first (up to
// AUTO_PING_MAX), and streams the replies as `pings` batches (BRIDGE.md):
// ~0.3 s for the master list once its hosts know us, ~1.5 s on first contact
// (measured 2026-10-25). A click re-pings one row with a `ping` command.
// Native drops replies for a list that was refetched meanwhile, so every
// pending row also frees itself on a timeout.
const AUTO_PING_MAX = 250; // gage_gui.cpp kAutoPingMax
const AUTO_TIMEOUT_MS = 8000;
const PING_CONCURRENCY = 4; // clicked re-pings in flight
const PING_TIMEOUT_MS = 5000;

/** 'pending' while queued or in flight, 'failed' when the last ping got no reply. */
export function pingStateOf(key, { pending, failed }) {
  if (pending.has(key)) return 'pending';
  if (failed.has(key)) return 'failed';
  return null;
}

/**
 * Ping bookkeeping. Feed it every `servers`, `pings` and `ping` message; it
 * returns the keys waiting on a reply and the keys whose last ping got none.
 */
export function usePingQueue(send) {
  const sendRef = useRef(send);
  sendRef.current = send;
  const [pending, setPending] = useState(() => new Set());
  const [failed, setFailed] = useState(() => new Set());
  // native's pass over the current list: rows it set out to ping, rows still out
  const [auto, setAuto] = useState({ total: 0, left: 0 });

  const q = useRef(null);
  if (!q.current) {
    // auto: rows native is pinging on its own; inflight: clicked re-pings
    const s = { list: [], auto: new Set(), autoTotal: 0, autoTimer: 0, queue: [], inflight: new Map(), failed: new Set() };

    const sync = () => {
      setPending(new Set([...s.auto, ...s.inflight.keys(), ...s.queue]));
      setFailed(new Set(s.failed));
      setAuto((a) => (a.total === s.autoTotal && a.left === s.auto.size ? a : { total: s.autoTotal, left: s.auto.size }));
    };

    const pump = () => {
      while (s.inflight.size < PING_CONCURRENCY && s.queue.length) {
        const key = s.queue.shift();
        // `row` is the index into native's master list, which is the last list received.
        const row = s.list.findIndex((sv) => serverKey(sv) === key);
        if (row < 0) continue;
        s.inflight.set(
          key,
          window.setTimeout(() => settle(key, -1), PING_TIMEOUT_MS),
        );
        sendRef.current({ type: 'ping', row });
      }
      sync();
    };

    // one reply for `key`, from either source
    function record(key, ms) {
      s.auto.delete(key);
      if (s.inflight.has(key)) {
        window.clearTimeout(s.inflight.get(key));
        s.inflight.delete(key);
      }
      if (ms < 0) s.failed.add(key);
      else s.failed.delete(key);
    }

    function settle(key, ms) {
      if (!s.inflight.has(key)) return;
      record(key, ms);
      pump();
    }

    const reset = () => {
      for (const t of s.inflight.values()) window.clearTimeout(t);
      window.clearTimeout(s.autoTimer);
      s.inflight.clear();
      s.auto.clear();
      s.queue = [];
      s.failed.clear();
      s.autoTotal = 0;
    };

    const keyOfRow = (row) => (typeof row === 'number' && s.list[row] ? serverKey(s.list[row]) : null);

    q.current = {
      onServers(d) {
        const list = Array.isArray(d.servers) ? d.servers : [];
        reset();
        s.list = list;
        if (d.status === 'ok') {
          // what native is pinging now: the busiest unmeasured rows
          s.auto = new Set(
            list
              .filter((sv) => !(sv.ping >= 0))
              .sort((a, b) => (b.players ?? -1) - (a.players ?? -1))
              .slice(0, AUTO_PING_MAX)
              .map(serverKey),
          );
          s.autoTotal = s.auto.size;
          if (s.auto.size)
            s.autoTimer = window.setTimeout(() => {
              for (const key of s.auto) s.failed.add(key);
              s.auto.clear();
              sync();
            }, AUTO_TIMEOUT_MS);
        }
        sync();
      },
      onPings(d) {
        for (const r of Array.isArray(d.results) ? d.results : []) {
          const key = keyOfRow(r?.row);
          if (key) record(key, typeof r.ms === 'number' ? r.ms : -1);
        }
        if (!s.auto.size) window.clearTimeout(s.autoTimer);
        pump();
      },
      onPing(d) {
        const key = keyOfRow(d.row);
        if (key) settle(key, typeof d.ms === 'number' ? d.ms : -1);
      },
      request(server) {
        const key = serverKey(server);
        if (s.inflight.has(key)) return;
        s.auto.delete(key);
        s.queue = [key, ...s.queue.filter((k) => k !== key)];
        pump();
      },
      dispose: reset,
    };
  }

  return { pending, failed, auto, queue: q.current };
}
