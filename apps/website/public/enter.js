// Runs before the first paint (a classic script in <head>; the CSP allows only same-origin scripts, so it
// cannot be inline): marks the page for the hero arrival, so the title is hidden from the first frame
// instead of being painted, hidden by main.ts, then faded in. styles.css shows it after 4 s whatever
// happens, so a failed main.ts never leaves the page without its title. Reduced motion: no arrival.
if (!window.matchMedia('(prefers-reduced-motion: reduce)').matches) document.documentElement.classList.add('enter');
