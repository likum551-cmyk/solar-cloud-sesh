import { describe, expect, it } from 'vitest';
import { checkDocs } from '../scripts/check-docs.js';
import { referenceProblems, renderReference, START_MARKER, END_MARKER, storedReference } from '../scripts/gen-reference.js';
import { listRoutes } from '../src/http/app.js';

// The documentation is part of the contract: a route, table or environment variable that the code
// has and the docs lack (or a stale generated block) fails here, not in a launcher developer's hands.
describe('documentation', () => {
  it('covers every route, table and environment variable and the generated block is current', () => {
    expect(checkDocs()).toEqual([]);
  });

  it('has an example for every route and an explanation for every error code', () => {
    expect(referenceProblems()).toEqual([]);
  });

  it('renders every route (a vacuous pass would be an empty registry)', () => {
    const routes = listRoutes();
    expect(routes.length).toBeGreaterThanOrEqual(40);
    const text = renderReference();
    for (const r of routes) expect(text).toContain(`### ${r.method.toUpperCase()} ${r.path}`);
  });

  it('detects a stale block: the checker would notice an edit to the stored reference', () => {
    const api = `${START_MARKER}\n\n${renderReference()}\n${END_MARKER}`;
    expect(storedReference(api)?.trimEnd()).toBe(renderReference().trimEnd());
    const edited = api.replace('### GET /health', '### GET /healthz');
    expect(storedReference(edited)?.trimEnd()).not.toBe(renderReference().trimEnd());
    expect(storedReference('no markers here')).toBeNull();
  });
});
