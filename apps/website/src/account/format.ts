// Display helpers for the account pages. Dates are built from numeric parts (never from the
// browser's locale strings), in the viewer's own time zone.

import type { SessionInfo } from './api';

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/** "6 Nov 2026" in the viewer's zone; "" for an unreadable date. */
export function formatDate(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  return `${d.getDate()} ${MONTHS[d.getMonth()]} ${d.getFullYear()}`;
}

/** "6 Nov 2026, 14:05" in the viewer's zone. */
export function formatDateTime(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const hh = String(d.getHours()).padStart(2, '0');
  const mm = String(d.getMinutes()).padStart(2, '0');
  return `${formatDate(iso)}, ${hh}:${mm}`;
}

/** "just now", "4 min ago", "3 h ago", "yesterday", "5 days ago", then the date. */
export function relativeTime(iso: string, now: Date): string {
  const t = new Date(iso).getTime();
  if (Number.isNaN(t)) return '';
  const s = Math.max(0, (now.getTime() - t) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86_400) return `${Math.floor(s / 3600)} h ago`;
  const days = Math.floor(s / 86_400);
  if (days === 1) return 'yesterday';
  if (days < 7) return `${days} days ago`;
  return formatDate(iso);
}

/** "Chrome on Windows", "Solar Launcher", or null when the user agent says nothing usable. */
export function describeUserAgent(ua: string | null | undefined): string | null {
  if (!ua) return null;
  if (/GAGE-MP-Launcher|SolarLauncher|Solar-Launcher/i.test(ua)) return 'Solar Launcher';
  const browser =
    /Edg\//.test(ua) ? 'Edge'
    : /OPR\/|Opera/.test(ua) ? 'Opera'
    : /Firefox\//.test(ua) ? 'Firefox'
    : /Chrome\//.test(ua) ? 'Chrome'
    : /Safari\//.test(ua) ? 'Safari'
    : null;
  const os =
    /Windows/.test(ua) ? 'Windows'
    : /iPhone|iPad|iPod/.test(ua) ? 'iOS'
    : /Android/.test(ua) ? 'Android'
    : /Mac OS X|Macintosh/.test(ua) ? 'macOS'
    : /Linux/.test(ua) ? 'Linux'
    : null;
  if (browser && os) return `${browser} on ${os}`;
  return browser ?? os;
}

/** The name a session row shows: the device name given at login, else the browser, else a plain fallback. */
export function sessionLabel(s: Pick<SessionInfo, 'deviceName' | 'userAgent'>): string {
  return s.deviceName?.trim() || describeUserAgent(s.userAgent) || 'Unknown device';
}

/** The deviceName this site sends at login (the platform keeps at most 64 characters). */
export function websiteDeviceName(ua: string): string {
  const what = describeUserAgent(ua);
  return (what ? `Website, ${what}` : 'Website').slice(0, 64);
}
