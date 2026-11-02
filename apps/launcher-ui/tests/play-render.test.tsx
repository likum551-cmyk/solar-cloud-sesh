import { describe, expect, it } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import { createApi } from '../src/lib/api';
import { createMockFetch } from '../src/lib/mock-api';
import { ServerList, blockedReason } from '../src/screens/PlayScreen';

// Renders the server column from the baked Solar RP lineup (lib/servers.ts, served through the real
// mock network) to static markup, so the 3-server lineup — SOL live, LUNA and TERRA "Coming soon" — is
// verified as it renders, not just as data. Guards against a stray text node from a JSX edit and
// against a coming-soon server looking playable.

describe('PlayScreen server lineup', () => {
  it('is the three Solar RP servers: SOL live, LUNA and TERRA coming soon', async () => {
    const api = createApi({ baseUrl: 'mock://platform', getToken: () => null, fetchImpl: createMockFetch() });
    const { servers } = await api.servers();

    expect(servers.map((s) => s.name)).toEqual(['SOL', 'LUNA', 'TERRA']);

    const live = servers.find((s) => !s.comingSoon)!;
    const soon = servers.filter((s) => s.comingSoon);
    expect(live.name).toBe('SOL');
    expect(live.host).toBe('5.175.253.187');
    expect(live.port).toBe(24800);
    expect(soon).toHaveLength(2);

    // The live server is playable; every coming-soon server is blocked with the same reason.
    expect(blockedReason(live)).toBeNull();
    for (const s of soon) expect(blockedReason(s)).toBe('This server is not live yet.');
  });

  it('renders one live card with a count and two greyed "Coming soon" cards', async () => {
    const api = createApi({ baseUrl: 'mock://platform', getToken: () => null, fetchImpl: createMockFetch() });
    const { servers } = await api.servers();
    const live = servers.find((s) => !s.comingSoon)!;

    const html = renderToStaticMarkup(
      <ServerList servers={servers} selectedId={live.id} onSelect={() => {}} demo />,
    );

    for (const s of servers) expect(html).toContain(s.name);
    expect(html.match(/Coming soon/g)?.length).toBe(2);   // exactly the two upcoming servers
    expect(html).toContain('scard__count');               // the live server shows its count
    expect(html).toContain('is-chosen');                  // the live card is the chosen one
    expect((html.match(/is-out/g) ?? []).length).toBe(2); // the two coming-soon cards are greyed
    // No value leaked undefined/NaN into the markup, and no stray brace from a moved JSX expression.
    expect(html).not.toMatch(/undefined|NaN|&gt;\}|\}&lt;/);
  });
});
