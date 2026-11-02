import { IconRefresh, IconSearch } from './icons.jsx';

const TABS = [
  { id: 'browse', label: 'Browse' },
  { id: 'favorites', label: 'Favorites' },
  { id: 'recents', label: 'Recents' },
  { id: 'settings', label: 'Settings' },
];

export default function TabHeader({
  tab,
  onTab,
  query,
  onQuery,
  loading,
  counts,
  onFetch,
}) {
  return (
    <div className="mt-3 flex flex-wrap items-center justify-between gap-x-4 gap-y-2 border-b border-white/[0.08]">
      <nav className="flex items-end gap-1" aria-label="Views">
        {TABS.map((t) => {
          const active = tab === t.id;
          const badge =
            t.id === 'favorites'
              ? counts.favorites
              : t.id === 'recents'
                ? counts.recents
                : null;
          return (
            <button
              key={t.id}
              type="button"
              onClick={() => onTab(t.id)}
              aria-current={active ? 'page' : undefined}
              className={`relative px-3.5 pb-2.5 pt-1.5 text-sm font-medium transition-colors duration-150 focus:outline-none focus-visible:ring-2 focus-visible:ring-[#7c5cff]/60 ${
                active ? 'text-white' : 'text-muted hover:text-slate-200'
              }`}
            >
              <span className="inline-flex items-center gap-1.5">
                {t.label}
                {badge != null && badge > 0 && (
                  <span className="rounded-full bg-white/10 px-1.5 py-px text-[10px] font-semibold text-slate-300">
                    {badge}
                  </span>
                )}
              </span>
              {active && (
                <span className="absolute inset-x-2 -bottom-px h-0.5 rounded-full bg-gradient-to-r from-[#4f8cff] to-[#7c5cff]" />
              )}
            </button>
          );
        })}
      </nav>

      {tab === 'browse' && (
        <div className="app-no-drag flex items-center gap-2 pb-2">
          <label className="relative block">
            <span className="sr-only">Search servers</span>
            <IconSearch className="pointer-events-none absolute left-2.5 top-1/2 h-4 w-4 -translate-y-1/2 text-muted" />
            <input
              value={query}
              onChange={(e) => onQuery(e.target.value)}
              placeholder="Search servers…"
              spellCheck="false"
              className="input !w-56 !py-1.5 !pl-8"
            />
          </label>
          <button
            type="button"
            onClick={onFetch}
            title="Refresh server list"
            className="btn-ghost !px-2.5 !py-1.5"
          >
            <IconRefresh className={`h-4 w-4 ${loading ? 'animate-spin' : ''}`} />
          </button>
        </div>
      )}
    </div>
  );
}
