// The current Solar Launcher release, read from the signed update manifest the launcher itself uses
// (/update/version.json, written by tools/update-sign/sign.mjs). Only the signed payload is read: the
// top-level fields are unsigned display copies for old launchers. The page does not verify the
// signature (the page and the manifest come from the same server, so that would prove nothing); it
// shows the SHA-256 so a player can check the file, and the launcher verifies every update itself.

export interface Release {
  version: string;
  url: string;
  sha256: string;
  size: number;
  /** The signed release notes the launcher shows ('' when absent or unusable; never fails the release). */
  notes: string;
  /** When the manifest was signed, or null. */
  issued: Date | null;
}

const NOTES_MAX = 2000; // tools/update-sign/sign.mjs refuses longer notes

function readNotes(v: unknown): string {
  if (typeof v !== 'string') return '';
  const t = v.trim();
  return t.length <= NOTES_MAX ? t : '';
}

function readIssued(v: unknown): Date | null {
  if (typeof v !== 'string' || !/^\d{4}-\d{2}-\d{2}T/.test(v)) return null;
  const d = new Date(v);
  return Number.isNaN(d.getTime()) ? null : d;
}

const NOTE_ITEMS_MAX = 40;

/**
 * The notes as items: split at "; " and at a sentence end (". " or the final "."), never inside
 * parentheses ("(Legacy; Enhanced keeps the 0.5.4 map)" stays one item) and never in a number
 * ("0.5.4"). Items are trimmed, start with a capital and lose their closing "." or ";".
 */
export function splitNotes(notes: string): string[] {
  const out: string[] = [];
  let depth = 0;
  let cur = '';
  const push = () => {
    const t = cur.trim().replace(/[.;]+$/, '').trim();
    if (t) out.push(t.charAt(0).toUpperCase() + t.slice(1));
    cur = '';
  };
  for (let i = 0; i < notes.length; i++) {
    const c = notes[i]!;
    if (c === '(') depth++;
    else if (c === ')') depth = Math.max(0, depth - 1);
    const next = notes[i + 1];
    if (depth === 0 && (c === ';' || c === '.') && (next === undefined || /\s/.test(next))) {
      push();
      continue;
    }
    cur += c;
  }
  push();
  return out.slice(0, NOTE_ITEMS_MAX);
}

/** "7 November 2026" (UTC, so every visitor reads the same day the release was signed). */
export function formatIssued(d: Date): string {
  return new Intl.DateTimeFormat('en-GB', { day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC' }).format(d);
}

const VERSION = /^\d{1,4}\.\d{1,4}\.\d{1,4}$/;
const SHA256 = /^[0-9a-f]{64}$/;
const FILE = /^\/update\/SolarRP-Setup-\d{1,4}\.\d{1,4}\.\d{1,4}\.exe$/;

function decodeBase64(b64: string): string | null {
  try {
    const bin = atob(b64);
    const bytes = Uint8Array.from(bin, (c) => c.charCodeAt(0));
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    return null;
  }
}

/**
 * The release named by a manifest, or null when it is missing, malformed or points anywhere other
 * than a SolarRP-Setup-<version>.exe under /update/ on `siteHost` (the manifest never steers the
 * download button to another host).
 */
export function parseManifest(json: unknown, siteHost: string): Release | null {
  if (typeof json !== 'object' || json === null) return null;
  const signed = (json as { signed?: unknown }).signed;
  if (typeof signed !== 'string' || signed.length === 0 || signed.length > 8192) return null;
  const text = decodeBase64(signed);
  if (text === null) return null;
  let p: unknown;
  try {
    p = JSON.parse(text);
  } catch {
    return null;
  }
  if (typeof p !== 'object' || p === null) return null;
  const { version, url, sha256, size, notes, issued } = p as Record<string, unknown>;
  if (typeof version !== 'string' || !VERSION.test(version)) return null;
  if (typeof sha256 !== 'string' || !SHA256.test(sha256)) return null;
  if (typeof size !== 'number' || !Number.isSafeInteger(size) || size <= 0) return null;
  if (typeof url !== 'string') return null;
  let u: URL;
  try {
    u = new URL(url);
  } catch {
    return null;
  }
  if (u.protocol !== 'https:' || u.hostname !== siteHost || u.search || u.hash || !FILE.test(u.pathname)) return null;
  if (!u.pathname.endsWith(`-${version}.exe`)) return null;
  return { version, url: u.href, sha256, size, notes: readNotes(notes), issued: readIssued(issued) };
}

/** "18.7 MB" (decimal megabytes, one place). */
export function formatSize(bytes: number): string {
  return `${(bytes / 1_000_000).toFixed(1)} MB`;
}

/** Fetches /update/version.json (never cached: it changes under one name). */
export async function loadRelease(siteHost: string, signal?: AbortSignal): Promise<Release | null> {
  const res = await fetch('/update/version.json', { cache: 'no-store', signal });
  if (!res.ok) return null;
  return parseManifest(await res.json(), siteHost);
}
