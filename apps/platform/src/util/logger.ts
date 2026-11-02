// Structured logging: one JSON object per line on stdout. Field values are JSON-encoded, so
// client-controlled strings (server names, headers) cannot forge extra log lines.

export type LogLevel = 'debug' | 'info' | 'warn' | 'error';
export type LogFields = Record<string, unknown>;

export interface Logger {
  debug(msg: string, fields?: LogFields): void;
  info(msg: string, fields?: LogFields): void;
  warn(msg: string, fields?: LogFields): void;
  error(msg: string, fields?: LogFields): void;
}

const ORDER: Record<LogLevel, number> = { debug: 10, info: 20, warn: 30, error: 40 };
const RESERVED = new Set(['ts', 'level', 'msg']);

export interface LoggerOptions {
  level: LogLevel;
  /** Receives one complete line without the trailing newline. Defaults to stdout. */
  write?: (line: string) => void;
  now?: () => number;
}

/** Error -> loggable fields (message, code, stack); anything else is stringified. */
export function errorFields(err: unknown): LogFields {
  if (err instanceof Error) {
    const code = (err as NodeJS.ErrnoException).code;
    return { err: err.message, ...(code === undefined ? {} : { code }), stack: err.stack };
  }
  return { err: String(err) };
}

export function createLogger(opts: LoggerOptions): Logger {
  const write = opts.write ?? ((line: string) => void process.stdout.write(`${line}\n`));
  const now = opts.now ?? (() => Date.now());
  const threshold = ORDER[opts.level];

  const emit = (level: LogLevel, msg: string, fields?: LogFields): void => {
    if (ORDER[level] < threshold) return;
    const entry: Record<string, unknown> = { ts: new Date(now()).toISOString(), level, msg };
    if (fields) {
      for (const [k, v] of Object.entries(fields)) {
        entry[RESERVED.has(k) ? `field_${k}` : k] = v;
      }
    }
    write(JSON.stringify(entry));
  };

  return {
    debug: (m, f) => emit('debug', m, f),
    info: (m, f) => emit('info', m, f),
    warn: (m, f) => emit('warn', m, f),
    error: (m, f) => emit('error', m, f),
  };
}
