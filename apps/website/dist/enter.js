// Runs before the first paint (a classic script in <head>; the CSP allows only same-origin scripts, so it
// cannot be inline): marks the page for the hero arrival. styles.css runs the arrival as a CSS animation
// that always ends visible (.enter .hero__copy / .status), so nothing depends on main.ts. Reduced motion: no arrival.
if (!window.matchMedia('(prefers-reduced-motion: reduce)').matches) document.documentElement.classList.add('enter');
