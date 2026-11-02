import { z } from 'zod';

// Limits that come from outside this service:
//  - name <= 64 and players/maxPlayers <= 1024: libs/protocol/protocol.h (kMaxServerName, kMaxPlayers).
//  - gamemode <= 32 and lang <= 8 BYTES: libs/net/serverfinder.cpp ReadLabel() blanks a longer
//    label, so a longer value would silently vanish in the launcher instead of failing here.
export const MAX_NAME_CHARS = 64;
export const MAX_GAMEMODE_BYTES = 32;
export const MAX_URL_CHARS = 256;
export const MAX_PLAYERS = 1024;

// Code points that must not appear in a label shown in the launcher, besides C0/C1 controls
// (\p{Cc}): U+2028/U+2029 line and paragraph separators, U+202A..U+202E bidi embeddings and
// overrides, U+2066..U+2069 bidi isolates. Written as numbers: invisible characters in this
// source file would not survive editing tools.
function isUnsafeCodePoint(cp: number): boolean {
  return (
    cp === 0x2028 ||
    cp === 0x2029 ||
    (cp >= 0x202a && cp <= 0x202e) ||
    (cp >= 0x2066 && cp <= 0x2069)
  );
}

const hasUnsafeCodePoint = (s: string): boolean =>
  /\p{Cc}/u.test(s) || Array.from(s).some((ch) => isUnsafeCodePoint(ch.codePointAt(0) ?? 0));

const isCleanText = (s: string): boolean => s.isWellFormed() && !hasUnsafeCodePoint(s);
const charCount = (s: string): number => Array.from(s).length;
const byteCount = (s: string): number => Buffer.byteLength(s, 'utf8');
const CLEAN_MESSAGE = 'must not contain control, bidi-override or unpaired-surrogate characters';

function isHttpUrl(v: string): boolean {
  let u: URL;
  try {
    u = new URL(v);
  } catch {
    return false; // not parseable as a URL: the refine message reports it
  }
  return (
    (u.protocol === 'http:' || u.protocol === 'https:') &&
    u.hostname !== '' &&
    u.username === '' &&
    u.password === '' &&
    isCleanText(v)
  );
}

export const announceSchema = z
  .object({
    port: z.number().int().min(1).max(65535),
    name: z
      .string()
      .trim()
      .min(1, 'must not be empty')
      .refine((s) => charCount(s) <= MAX_NAME_CHARS, `at most ${MAX_NAME_CHARS} characters`)
      .refine(isCleanText, CLEAN_MESSAGE),
    gamemode: z
      .string()
      .trim()
      .min(1, 'must not be empty')
      .refine((s) => byteCount(s) <= MAX_GAMEMODE_BYTES, `at most ${MAX_GAMEMODE_BYTES} UTF-8 bytes`)
      .refine(isCleanText, CLEAN_MESSAGE),
    lang: z
      .string()
      .trim()
      .regex(/^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,4})?$/, 'must be a language tag such as "en" or "pt-BR" (at most 8 characters)'),
    url: z
      .string()
      .trim()
      .max(MAX_URL_CHARS)
      .refine((v) => v === '' || isHttpUrl(v), 'must be an http(s) URL without credentials')
      .optional(),
    players: z.number().int().min(0).max(MAX_PLAYERS),
    maxPlayers: z.number().int().min(1).max(MAX_PLAYERS),
    protocol: z.number().int().min(0).max(65535),
  })
  .refine((v) => v.players <= v.maxPlayers, {
    path: ['players'],
    message: 'must not exceed maxPlayers',
  });

export type Announce = z.infer<typeof announceSchema>;
