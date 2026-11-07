// Markup shared by several pages, inlined at build time by the `solar-partials` plugin in vite.config.ts:
// a page writes `<!-- @include bar -->` and gets PARTIALS.bar. Static strings only (no data goes in).

const LOCKUP = `<svg class="lockup" viewBox="0 0 430 100" aria-hidden="true"><path d="M0 0L41 0L50 9L50 31L32 31L32 18L18 18L18 41L30 41L12 59L0 59Z" fill="#f7f2e8"/><path d="M50 100L9 100L0 91L0 69L18 69L18 82L32 82L32 59L20 59L38 41L50 41Z" fill="#f2b134"/><g fill="#f7f2e8" fill-rule="evenodd"><path transform="translate(60 0)" d="M0 0L41 0L50 9L50 100L9 100L0 91ZM18 18L32 18L32 82L18 82Z"/><path transform="translate(120 0)" d="M0 0L10 0L18 8L18 82L42 82L42 100L9 100L0 91Z"/><path transform="translate(168 0)" d="M0 100L0 20L20 0L38 0L58 20L58 100L40 100L40 59L18 59L18 100ZM18 22L40 22L40 41L18 41Z"/><path transform="translate(236 0)" d="M0 0L41 0L50 9L50 100L33 100L23 59L18 59L18 100L9 100L0 91ZM18 18L32 18L32 41L18 41Z"/><path transform="translate(320 0)" d="M0 0L41 0L50 9L50 100L33 100L23 59L18 59L18 100L9 100L0 91ZM18 18L32 18L32 41L18 41Z"/><path transform="translate(380 0)" d="M0 0L41 0L50 9L50 59L18 59L18 100L9 100L0 91ZM18 18L32 18L32 41L18 41Z"/></g></svg>`;

const MARK = `<svg class="foot__mark" viewBox="0 0 50 100" aria-hidden="true"><path d="M0 0L41 0L50 9L50 31L32 31L32 18L18 18L18 41L30 41L12 59L0 59Z" fill="#f7f2e8"/><path d="M50 100L9 100L0 91L0 69L18 69L18 82L32 82L32 59L20 59L38 41L50 41Z" fill="#f2b134"/></svg>`;

export const PARTIALS: Record<string, string> = {
  /** charset, viewport and icons (the landing page keeps its own fuller head) */
  head: `<meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover" />
  <meta name="theme-color" content="#0a0907" />
  <link rel="icon" href="/favicon.svg" type="image/svg+xml" />
  <link rel="icon" href="/favicon-32.png" sizes="32x32" type="image/png" />
  <link rel="apple-touch-icon" href="/apple-touch-icon.png" />`,

  /** the top bar of the account pages: always solid (the art starts under it), links back into the landing page */
  bar: `<header class="bar is-solid" id="top">
    <a class="bar__brand" href="/" aria-label="Solar RP home">${LOCKUP}</a>
    <nav class="bar__nav" aria-label="Site">
      <a href="/#servers">Servers</a>
      <a href="/#features">Features</a>
      <a href="/#join">How to join</a>
      <a href="/#faq">FAQ</a>
    </nav>
    <a class="bar__account js-account-link" href="/login/">Log in</a>
    <a class="plate plate--sm" href="/#download">Download</a>
  </header>`,

  /** every page's footer */
  foot: `<footer class="foot">
    <div class="wrap foot__row">
      ${MARK}
      <nav class="foot__nav" aria-label="Account">
        <a class="js-foot-guest" href="/signup/">Create account</a>
        <a class="js-foot-guest" href="/login/">Log in</a>
        <a class="js-foot-member" href="/account/" hidden>Your account</a>
      </nav>
      <p class="foot__legal">Solar RP is not affiliated with Rockstar Games or Take-Two Interactive. Footage and artwork: Grand Theft Auto V, &copy; Rockstar Games.</p>
    </div>
  </footer>`,
};
