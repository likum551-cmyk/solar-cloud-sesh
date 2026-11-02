import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App.jsx';
import './styles.css';

// An app window, not a page: the browser context menu (Back, Reload, Save as,
// Print) belongs only in text fields, where it carries Cut/Copy/Paste.
document.addEventListener('contextmenu', (e) => {
  const t = e.target;
  if (!(t instanceof Element) || !t.closest('input, textarea, [contenteditable="true"]')) e.preventDefault();
});

// The menu is drawn for 1240x800 (the default window). A larger window scales
// the whole menu up, as the game's does at a higher resolution, instead of
// stretching panels into empty glass; capped so text never turns poster-size.
const root = document.getElementById('root');
function fitZoom() {
  const z = Math.min(1.35, Math.max(1, Math.min(window.innerWidth / 1240, window.innerHeight / 800)));
  root.style.zoom = z > 1.001 ? z.toFixed(3) : '';
}
fitZoom();
window.addEventListener('resize', fitZoom);

createRoot(root).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
