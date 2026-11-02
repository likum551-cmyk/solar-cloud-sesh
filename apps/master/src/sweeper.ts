import { errorFields, type Logger } from './logger.js';
import type { TokenBucketLimiter } from './rate-limit.js';
import type { ServerStore } from './store.js';

export interface SweeperDeps {
  store: ServerStore;
  limiter: TokenBucketLimiter;
  clock: () => number;
  logger: Logger;
  intervalMs: number;
}

export interface Sweeper {
  /** One sweep now (also what the timer runs). Never rejects: a failure is logged. */
  runOnce(): Promise<void>;
  stop(): void;
}

/** Periodically removes expired entries and idle rate-limit buckets. */
export function startSweeper(deps: SweeperDeps): Sweeper {
  let running = false;

  const runOnce = async (): Promise<void> => {
    if (running) return; // a slow store must not stack sweeps
    running = true;
    try {
      const now = deps.clock();
      const removed = await deps.store.sweep(now);
      const buckets = deps.limiter.prune(now);
      if (removed > 0) deps.logger.info('expired servers removed', { removed });
      if (buckets > 0) deps.logger.debug('idle rate-limit buckets dropped', { buckets });
    } catch (err) {
      deps.logger.error('sweep failed', errorFields(err));
    } finally {
      running = false;
    }
  };

  // unref: the sweeper alone must never keep the process alive.
  const timer = setInterval(() => void runOnce(), deps.intervalMs);
  timer.unref();

  return { runOnce, stop: () => clearInterval(timer) };
}
