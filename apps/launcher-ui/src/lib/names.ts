// The in-game name rule, mirrored from the GAGE server (libs/protocol: ValidName, kMaxName): 1..24
// characters of A-Z a-z 0-9 _ . - with single inner spaces. A name the server would refuse is
// caught here, before GTA V spends minutes starting.

export const MAX_IN_GAME_NAME = 24;

const ALLOWED = /^[A-Za-z0-9_.-]$/;

export function isValidInGameName(name: string): boolean {
  return nameProblem(name) === null;
}

// Marks, format and control characters render as nothing (or a floating accent), so quoting them helps nobody.
const HIDDEN = /^[\p{M}\p{Cf}\p{Cc}]$/u;

/** What the user meant: any run of whitespace (tab, no-break space) becomes one space, ends trimmed. */
export function normalizeInGameName(name: string): string {
  return name.replace(/\s+/g, ' ').trim();
}

/** The first character no amount of normalising can fix, named; checked while typing. Whitespace is fixable. */
export function charProblem(name: string): string | null {
  const bad = [...name].find((c) => !/\s/.test(c) && !ALLOWED.test(c));
  if (bad === undefined) return null;
  if (HIDDEN.test(bad)) return "A hidden or special character can't be in a name. Use A–Z, 0–9, dot, dash or underscore.";
  return `"${bad}" can't be in a name. Use A–Z, 0–9, dot, dash or underscore.`;
}

/** What is wrong with `name`, naming the culprit; null when the server would accept it. */
export function nameProblem(name: string): string | null {
  if (name.length === 0) return 'A name cannot be empty.';
  const chars = [...name];
  if (chars.length > MAX_IN_GAME_NAME) return `At most ${MAX_IN_GAME_NAME} characters.`;
  const bad = charProblem(name.replace(/ /g, ''));
  if (bad) return bad;
  if (/\s/.test(name.replace(/ /g, '')) || name !== normalizeInGameName(name)) return 'Use one space between words, none at the start or end.';
  return null;
}

/** A default for an install that never chose a name: Player_ and 4 random characters (fits the rule). */
export function randomInGameName(random: () => number = Math.random): string {
  const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  let tail = '';
  for (let i = 0; i < 4; i++) tail += chars[Math.floor(random() * chars.length) % chars.length];
  return `Player_${tail}`;
}
