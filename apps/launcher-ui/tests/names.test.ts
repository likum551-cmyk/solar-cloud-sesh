import { describe, expect, it } from 'vitest';
import { charProblem, isValidInGameName, MAX_IN_GAME_NAME, nameProblem, normalizeInGameName, randomInGameName } from '../src/lib/names';

// Mirrors libs/protocol ValidName: the server refuses anything this accepts at its peril.
describe('in-game name rule', () => {
  it('accepts the server charset with single inner spaces', () => {
    for (const n of ['nightshift', 'a', 'Big_Boss.99-x', 'Two Words', 'A B C', 'x'.repeat(MAX_IN_GAME_NAME)]) expect(isValidInGameName(n), n).toBe(true);
  });
  it('refuses what the server refuses', () => {
    for (const n of ['', ' lead', 'trail ', 'two  spaces', 'x'.repeat(MAX_IN_GAME_NAME + 1), 'emoji😀', 'tab\tname', 'ünï', 'semi;colon']) expect(isValidInGameName(n), JSON.stringify(n)).toBe(false);
  });
  it('names the problem', () => {
    expect(nameProblem('bad!')).toBe('"!" can\'t be in a name. Use A–Z, 0–9, dot, dash or underscore.');
    expect(nameProblem('Игрок')).toMatch(/^"И" can't be in a name/);
    expect(nameProblem('two  spaces')).toBe('Use one space between words, none at the start or end.');
    expect(nameProblem('')).toBe('A name cannot be empty.');
    expect(nameProblem('ok name')).toBeNull();
    // A combining accent or zero-width joiner would print as nothing between the quotes.
    expect(nameProblem('e' + String.fromCharCode(0x301))).toMatch(/^A hidden or special character/);
    expect(nameProblem('a' + String.fromCharCode(0x200d) + 'b')).toMatch(/^A hidden or special character/);
    expect(nameProblem('no' + String.fromCharCode(0xa0) + 'break')).toBe('Use one space between words, none at the start or end.');
  });
  it('normalises whitespace the user cannot see into what the server accepts', () => {
    const nbsp = String.fromCharCode(0xa0);
    const cases: [string, string][] = [['  two   words ', 'two words'], [`no${nbsp}break`, 'no break'], ['tab\tname', 'tab name'], ['   ', '']];
    for (const [raw, want] of cases) {
      expect(normalizeInGameName(raw)).toBe(want);
      if (want) expect(nameProblem(normalizeInGameName(raw)), raw).toBeNull();
    }
  });
  it('flags unfixable characters while typing, never fixable whitespace', () => {
    expect(charProblem('Giorgi!')).toMatch(/^"!" can't be in a name/);
    expect(charProblem('გიორგი')).toMatch(/^"გ" can't be in a name/);
    expect(charProblem(' half typed  ')).toBeNull();
    expect(charProblem('')).toBeNull();
  });
  it('random defaults are valid and differ', () => {
    let seed = 1;
    const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
    const a = randomInGameName(rnd), b = randomInGameName(rnd);
    expect(isValidInGameName(a)).toBe(true);
    expect(a).toMatch(/^Player_[A-Z0-9]{4}$/);
    expect(a).not.toBe(b);
  });
});
