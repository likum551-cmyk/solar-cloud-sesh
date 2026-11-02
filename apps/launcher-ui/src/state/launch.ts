import { launchErrorCopy } from '../lib/bridge';

// PLAY -> join token -> native launch -> launched / failed, as a pure reducer. The flash
// animation keys off phase changes; the 3D render loop pauses for every phase except idle/failed.

export type LaunchStage = 'preflight' | 'starting_store' | 'waiting_for_game' | 'injecting' | 'injected';

export type LaunchState =
  | { phase: 'idle' }
  | { phase: 'joining'; serverId: string; serverName: string }
  | { phase: 'launching'; serverId: string; serverName: string; stage: LaunchStage | null; statusText: string | null }
  | { phase: 'launched'; serverId: string; serverName: string; gameRunning: boolean }
  | { phase: 'failed'; serverId: string; serverName: string; title: string; body: string; action?: 'gameFolder' | 'verifyEmail' };

export type LaunchEvent =
  | { type: 'PLAY'; serverId: string; serverName: string }
  | { type: 'JOINED' }
  | { type: 'JOIN_FAILED'; title: string; body: string; action?: 'verifyEmail' }
  | { type: 'STAGE'; stage: LaunchStage }
  | { type: 'STATUS'; text: string }
  | { type: 'RESULT'; ok: boolean; code: number; error: string }
  | { type: 'GAME'; running: boolean }
  | { type: 'DISMISS' };

export const initialLaunch: LaunchState = { phase: 'idle' };

/** Ordered stages, for the progress readout (index + 1 of length). */
export const STAGES: readonly LaunchStage[] = ['preflight', 'starting_store', 'waiting_for_game', 'injecting', 'injected'];

/** True while a launch owns the window: PLAY is disabled and the 3D loop is paused. */
export function isBusy(s: LaunchState): boolean {
  return s.phase === 'joining' || s.phase === 'launching';
}

export function launchReducer(s: LaunchState, e: LaunchEvent): LaunchState {
  switch (e.type) {
    case 'PLAY':
      if (isBusy(s)) return s;
      return { phase: 'joining', serverId: e.serverId, serverName: e.serverName };
    case 'JOINED':
      return s.phase === 'joining' ? { phase: 'launching', serverId: s.serverId, serverName: s.serverName, stage: null, statusText: null } : s;
    case 'JOIN_FAILED':
      return s.phase === 'joining' ? { phase: 'failed', serverId: s.serverId, serverName: s.serverName, title: e.title, body: e.body, ...(e.action ? { action: e.action } : {}) } : s;
    case 'STAGE':
      return s.phase === 'launching' ? { ...s, stage: e.stage } : s;
    case 'STATUS':
      return s.phase === 'launching' ? { ...s, statusText: e.text } : s;
    case 'RESULT': {
      if (s.phase !== 'launching') return s;
      if (e.ok) return { phase: 'launched', serverId: s.serverId, serverName: s.serverName, gameRunning: true };
      const copy = launchErrorCopy(e.code, e.error);
      return { phase: 'failed', serverId: s.serverId, serverName: s.serverName, title: copy.title, body: copy.body, ...(copy.action ? { action: copy.action } : {}) };
    }
    case 'GAME':
      if (s.phase === 'launched') return e.running ? { ...s, gameRunning: true } : { phase: 'idle' };
      return s;
    case 'DISMISS':
      return s.phase === 'failed' || s.phase === 'launched' ? { phase: 'idle' } : s;
  }
}

/** Human label for a native launch stage. */
export function stageLabel(stage: LaunchStage | null): string {
  switch (stage) {
    case 'preflight': return 'Checking Solar RP files';
    case 'starting_store': return 'Starting GTA V through your store';
    case 'waiting_for_game': return 'Waiting for GTA V';
    case 'injecting': return 'Loading Solar RP into the game';
    case 'injected': return 'Loaded';
    case null: return 'Handing over to the launcher';
  }
}
