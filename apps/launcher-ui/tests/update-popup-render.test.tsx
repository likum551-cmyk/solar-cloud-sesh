import { describe, expect, it } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import type { UpdateMsg } from '../src/lib/bridge';
import { SERVER_BEHIND_TEXT } from '../src/lib/bridge';
import { UpdateDialog } from '../src/screens/UpdatePopup';
import { deriveUpdateView, initialUpdateGate, updateGateReducer, type UpdateGateEvent } from '../src/state/updatePopup';

// The popup as it renders (static markup): the dialog semantics, the progress bar, and that a mandatory
// popup offers no way out. The behaviour rules themselves are in update-popup.test.ts.

const msg = (over: Partial<UpdateMsg> = {}): UpdateGateEvent => ({
  type: 'UPDATE', msg: { type: 'update', current: '0.4.3', latest: '0.4.4', available: true, error: '', state: 'downloading', progress: 0.42, mandatory: true, notes: '', ...over },
});
const html = (...events: UpdateGateEvent[]) => renderToStaticMarkup(
  <UpdateDialog view={deriveUpdateView(events.reduce(updateGateReducer, initialUpdateGate))} onRetry={() => {}} onDismiss={() => {}} />,
);

describe('UpdateDialog markup', () => {
  it('renders nothing when there is no update and no refusal', () => {
    expect(html()).toBe('');
    expect(html(msg({ state: 'idle', available: false, mandatory: false, latest: '' }))).toBe('');
  });

  it('a downloading update is a modal dialog with a labelled progress bar and a live status line', () => {
    const h = html(msg());
    expect(h).toContain('role="dialog"');
    expect(h).toContain('aria-modal="true"');
    expect(h).toMatch(/aria-labelledby="[^"]+"/);
    expect(h).toMatch(/aria-describedby="[^"]+"/);
    expect(h).toContain('Updating Solar RP...');
    expect(h).toContain('role="progressbar"');
    expect(h).toContain('aria-valuenow="42"');
    expect(h).toContain('aria-live="polite"');
    expect(h).toContain('Downloading version 0.4.4: 40%.');
    expect(h).toContain('>42<');
    expect(h).toContain('--fill:0.42');
    expect(h).toContain('data-mandatory="true"');
  });

  it('an indeterminate step has no value and sweeps instead of filling', () => {
    const h = html({ type: 'LAUNCH_RESULT', ok: false, code: 14, error: '' });
    expect(h).toContain('role="progressbar"');
    expect(h).not.toContain('aria-valuenow');
    expect(h).toContain('is-sweep');
    expect(h).not.toContain('--fill');
  });

  it('a mandatory popup has no button of any kind (no close, no OK) while it works', () => {
    for (const over of [{}, { state: 'verifying' as const }, { state: 'installing' as const }, { state: 'deferred' as const }]) {
      expect(html(msg(over))).not.toContain('<button');
    }
    expect(html({ type: 'LAUNCH_RESULT', ok: false, code: 14, error: '' })).not.toContain('<button');
  });

  it('a failure offers Try again (an alert) and, mandatory, still no Close', () => {
    const h = html(msg({ state: 'error', error: 'offline' }));
    expect(h).toContain('Try again');
    expect(h).not.toContain('>Close<');
    expect(h).toContain('role="alert"');
    expect(h).toContain('offline');
    expect(h).not.toContain('role="progressbar"');
  });

  it('code 15 shows its own copy with an OK that dismisses, and no progress bar', () => {
    const h = html({ type: 'LAUNCH_RESULT', ok: false, code: 15, error: '' });
    expect(h).toContain('Server not updated yet');
    expect(h).toContain(SERVER_BEHIND_TEXT);
    expect(h).toContain('>OK<');
    expect(h).not.toContain('Updating Solar RP');
    expect(h).not.toContain('role="progressbar"');
    expect(h).toContain('data-mandatory="false"');
  });

  it('escapes the host error text', () => {
    expect(html(msg({ state: 'error', error: '<img src=x onerror=alert(1)>' }))).not.toContain('<img');
  });
});
