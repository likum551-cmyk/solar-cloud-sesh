import { Pricedown } from './gta.jsx';

// Blocking screen for a production build that has no native host
// (window.chrome.webview missing). Nothing else is rendered: no fake data.
export default function BridgeMissing() {
  return (
    <div className="alert" role="alert" aria-label="Launcher bridge not found">
      <div className="alert-body">
        <h1>
          <span className="sr-only">Alert: launcher bridge not found</span>
          <Pricedown k="alert" size={96} />
        </h1>
        <p className="alert-text">
          This page only works inside the GAGE-MP launcher window.
          <span className="alert-fix">Close it and start GAGE-MP.exe.</span>
        </p>
      </div>
    </div>
  );
}
