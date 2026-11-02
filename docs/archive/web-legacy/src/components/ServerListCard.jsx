import { useMemo, useRef } from 'react';
import { serverKey } from '../lib/format.js';
import { PanelMessage } from './PanelMessage.jsx';
import ServerGrid, { useCompact } from './ServerGrid.jsx';

/**
 * Favorites and Recents: the same menu list as the server list, in saved
 * order (recency or the order starred), with the live list's players and
 * ping for any entry it carries. Clearing a whole list lives in Settings.
 */
export default function ServerListCard({
  title,
  servers,
  emptyTitle,
  emptyHint,
  liveServers = [],
  pings,
  favoriteKeys,
  onToggleFavorite,
  selectedKey,
  onSelect,
  onConnect,
  launching = false,
  onMenuChange,
}) {
  const sectionRef = useRef(null);
  const compact = useCompact(sectionRef);
  // saved entries carry no ping or list facts; show the live list's when it has the server
  const rows = useMemo(() => {
    const live = new Map(liveServers.map((s) => [serverKey(s), s]));
    return servers.map((s) => {
      const now = live.get(serverKey(s));
      return now
        ? { ...s, players: now.players, ping: now.ping, maxPlayers: now.maxPlayers, mode: now.mode, lang: now.lang }
        : { ...s, ping: -1 };
    });
  }, [servers, liveServers]);

  return (
    <section ref={sectionRef} className="panel app-no-drag" aria-label={title}>
      <h2 className="sr-only">{title}</h2>
      {servers.length === 0 ? (
        <PanelMessage title={emptyTitle} hint={emptyHint} />
      ) : (
        // the tab already carries the count; the list starts at its header
        <ServerGrid
          label={title}
          rows={rows}
          compact={compact}
          favoriteKeys={favoriteKeys}
          selectedKey={selectedKey}
          onSelect={onSelect}
          onConnect={onConnect}
          onToggleFavorite={onToggleFavorite}
          pings={pings}
          launching={launching}
          onMenuChange={onMenuChange}
        />
      )}
    </section>
  );
}
