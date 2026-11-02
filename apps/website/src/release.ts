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
  const { version, url, sha256, size } = p as Record<string, unknown>;
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
  return { version, url: u.href, sha256, size };
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
