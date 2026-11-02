// The one list of things that must never be inside the VPS kit. Used by kit-scan.mjs and sanitize-settings.mjs on GI's PC.
// Nothing here ever prints a matched value: callers report class names and file names only.

// File-name rules. Paths are relative, with forward slashes. `strict` rules apply to the Claude setup only: the repository
// tarball has legitimate files such as design tokens or a "sessions" source folder (pack-source.sh vets that tree itself).
export const NAME_RULES = [
  { id: 'pem', re: /\.pem$/i },
  { id: 'key-file', re: /\.key$/i },
  { id: 'env-file', re: /(^|\/)\.env[^/]*$/i },
  { id: 'credentials-json', re: /(^|\/)\.credentials\.json$/i },
  { id: 'claude-json', re: /(^|\/)\.claude\.json$/i },
  { id: 'ssh-key', re: /(^|\/)id_(rsa|dsa|ecdsa|ed25519)[^/]*$/i },
  { id: 'gage-keys-dir', re: /(^|\/)\.gage-keys(\/|$)/i },
  { id: 'signing-key', re: /update-signing-key/i },
  { id: 'keystore', re: /\.(p12|pfx|jks|keystore)$/i },
  { id: 'token-name', re: /(^|\/)[^/]*token[^/]*$/i, strict: true },
  { id: 'secret-name', re: /(^|\/)[^/]*secret[^/]*$/i, strict: true },
  { id: 'history', re: /(^|\/)history\.jsonl$/i, strict: true },
  { id: 'transcript-dir', re: /(^|\/)(sessions|todos|shell-snapshots|statsig|session-env|paste-cache|file-history)\//i, strict: true },
  { id: 'transcript-file', re: /\.jsonl$/i, strict: true },
];

// Content rules for text files. `block` hits stop the packer. `warn` hits are listed (file names) but do not stop it.
export const CONTENT_RULES = [
  { id: 'private-key-block', re: /BEGIN [A-Z0-9 ]*PRIVATE KEY/, block: true },
  { id: 'anthropic-key', re: /sk-ant-[A-Za-z0-9_-]{16,}/, block: true },
  { id: 'openai-style-key', re: /\bsk-[A-Za-z0-9]{32,}\b/, block: true },
  { id: 'github-token', re: /\b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b/, block: true },
  { id: 'aws-access-key', re: /\bAKIA[0-9A-Z]{16}\b/, block: true },
  { id: 'slack-token', re: /\bxox[abprs]-[0-9A-Za-z-]{10,}/, block: true },
  { id: 'google-api-key', re: /\bAIza[0-9A-Za-z_-]{35}\b/, block: true },
  { id: 'jwt', re: /\beyJ[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{10,}/, block: true },
  { id: 'apikey-json-literal', re: /"api[_-]?key"\s*:\s*"[^"]{8,}"/i, block: true },
  { id: 'secret-assignment', re: /\b(token|secret|password|passwd|api[_-]?key|access[_-]?key)["']?\s*[:=]\s*["'][A-Za-z0-9+/_=.-]{20,}["']/i, block: true },
  { id: 'url-with-credentials', re: /:\/\/[^/\s:@"']{1,64}:[^/\s@"']{3,}@[A-Za-z0-9.-]+/, block: false },
];

// Words that are only reported as a count (prose in docs and lessons mentions them; they are not secrets by themselves).
export const REPORT_WORDS = [/token/i, /secret/i, /password/i];

// Object property names that look like they hold a credential. A key with such a name and a non-empty scalar value is dropped.
export const SECRET_KEY_NAME = /token|secret|passw|api[_-]?key|credential|bearer|private[_-]?key|access[_-]?key|auth(?!or)/i;

// Strings that point at Windows: drive paths (not URLs: a drive letter is not preceded by another letter), .cmd/.bat/.exe/.ps1,
// powershell/pwsh, %VAR% expansion, backslashes.
export const WINDOWS_STRING = [
  { id: 'drive-path', re: /(^|[^A-Za-z])[A-Za-z]:[\\/]/ },
  { id: 'windows-executable', re: /\.(cmd|bat|exe|ps1)\b/i },
  { id: 'powershell', re: /\b(powershell|pwsh)\b/i },
  { id: 'percent-variable', re: /%[A-Za-z_][A-Za-z0-9_]*%/ },
  { id: 'backslash', re: /\\/ },
];

export function nameHits(relPath, strict) {
  const out = [];
  for (const r of NAME_RULES) {
    if (r.strict && !strict) continue;
    if (r.re.test(relPath)) out.push(r.id);
  }
  return out;
}

export function contentHits(text) {
  const out = [];
  for (const r of CONTENT_RULES) if (r.re.test(text)) out.push({ id: r.id, block: r.block });
  return out;
}

export function windowsReason(s) {
  for (const r of WINDOWS_STRING) if (r.re.test(s)) return r.id;
  return null;
}
