import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './App';
import { installErrorLog } from './lib/error-log';
import './styles/tokens.css';
import './styles/base.css';
import './styles/ui.css';
import './styles/screens.css';

installErrorLog();

// An app window, not a page: the browser context menu belongs only in text fields.
document.addEventListener('contextmenu', (e) => {
  const t = e.target;
  if (!(t instanceof Element) || !t.closest('input, textarea, [contenteditable="true"]')) e.preventDefault();
});

const root = document.getElementById('root');
if (!root) throw new Error('#root missing from index.html');
createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
