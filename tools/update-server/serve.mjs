#!/usr/bin/env node
// Solar RP local update server: serves the launcher's signed update manifest and setup
// downloads from this machine. Loopback only (127.0.0.1); no dependencies. It serves the
// manifest bytes untouched: the signature is made by tools/update-sign/sign.mjs, never here.
//
//   node tools/update-server/serve.mjs [--port N] [--version-file PATH] [--installer-dir DIR]
//
//   GET /version.json  (or /update/version.json)
//                              -> tools/update-server/version.json (or --version-file)
//   GET /download/<setup.exe>  (or /update/<setup.exe>)
//                              -> build/win/installer/<setup.exe> (or --installer-dir),
//                                 only names like SolarRP-Setup-1.2.3.exe (or the pre-rename
//                                 GAGE-MP-Setup-1.2.3.exe)
//   anything else              -> 404
//
// /update/... mirrors the VPS layout (https://solar-rp.com/update/version.json), so a manifest signed for
// the VPS can be tried here by pointing HKCU\Software\GAGE-MP update_url at this server.
// --port 0 picks a free port; the real one is printed in the "listening" line.
// Exits non-zero if the port is taken or the version file is missing/invalid. Warns on stderr
// when the version file is the legacy unsigned format, which launchers 0.4.0 and newer ignore.

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HOST = "127.0.0.1";
const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(SCRIPT_DIR, "..", "..");
const SETUP_NAME = /^(?:SolarRP|GAGE-MP)-Setup-\d+(?:\.\d+)*\.exe$/;

function die(msg) {
  process.stderr.write(`update-server: error: ${msg}\n`);
  process.exit(1);
}

function parseArgs(argv) {
  const opts = {
    port: 18600,
    versionFile: path.join(SCRIPT_DIR, "version.json"),
    installerDir: path.join(REPO_ROOT, "build", "win", "installer"),
  };
  for (let i = 0; i < argv.length; i++) {
    const flag = argv[i];
    const value = argv[i + 1];
    const need = () => {
      if (value === undefined || value.startsWith("--")) die(`${flag} needs a value`);
      i++;
      return value;
    };
    switch (flag) {
      case "--port": {
        const v = need();
        if (!/^\d+$/.test(v) || Number(v) > 65535) die(`invalid --port '${v}' (0..65535)`);
        opts.port = Number(v);
        break;
      }
      case "--version-file":
        opts.versionFile = path.resolve(need());
        break;
      case "--installer-dir":
        opts.installerDir = path.resolve(need());
        break;
      case "--help":
      case "-h":
        process.stdout.write("usage: node serve.mjs [--port N] [--version-file PATH] [--installer-dir DIR]\n");
        process.exit(0);
        break;
      default:
        die(`unknown argument '${flag}'`);
    }
  }
  return opts;
}

// Read and validate the version file. Throws with a readable reason.
function readVersionFile(file) {
  const raw = fs.readFileSync(file);
  let doc;
  try {
    doc = JSON.parse(raw.toString("utf8"));
  } catch (e) {
    throw new Error(`${file} is not valid JSON: ${e.message}`);
  }
  if (!doc || typeof doc.version !== "string" || doc.version.trim() === "")
    throw new Error(`${file} has no "version" string`);
  return raw;
}

const opts = parseArgs(process.argv.slice(2));

try {
  const doc = JSON.parse(readVersionFile(opts.versionFile).toString("utf8"));
  if (typeof doc.signed !== "string" || typeof doc.sig !== "string")
    process.stderr.write(
      `update-server: warning: ${opts.versionFile} has no "signed"/"sig": this is the legacy unsigned format, ` +
        `and launchers 0.4.0 and newer ignore it (no signed payload). Write one with tools/update-sign/sign.mjs.\n`,
    );
} catch (e) {
  die(e.message);
}

function send(res, status, body, headers = {}) {
  const buf = Buffer.from(body);
  res.writeHead(status, {
    "Content-Type": "text/plain; charset=utf-8",
    "Content-Length": buf.length,
    ...headers,
  });
  res.end(res.req.method === "HEAD" ? undefined : buf);
}

const notFound = (res) => send(res, 404, "not found\n");

function serveVersion(res) {
  let buf;
  try {
    buf = readVersionFile(opts.versionFile); // re-read per request: edits apply without a restart
  } catch (e) {
    process.stderr.write(`update-server: ${e.message}\n`);
    send(res, 500, "version file unavailable\n");
    return;
  }
  send(res, 200, buf, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
  });
}

function serveDownload(res, rawName) {
  // rawName is still percent-encoded; decode once, then whitelist the result,
  // so "..%2F" and "%5C" forms are refused like literal ones.
  let name;
  try {
    name = decodeURIComponent(rawName);
  } catch {
    notFound(res);
    return;
  }
  if (!SETUP_NAME.test(name)) {
    notFound(res);
    return;
  }
  const file = path.join(opts.installerDir, name);
  if (path.dirname(file) !== path.resolve(opts.installerDir)) {
    notFound(res); // belt and braces; the whitelist already excludes separators
    return;
  }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) {
      notFound(res);
      return;
    }
    res.writeHead(200, {
      "Content-Type": "application/octet-stream",
      "Content-Length": st.size,
      "Content-Disposition": `attachment; filename="${name}"`,
      "Cache-Control": "no-store",
    });
    if (res.req.method === "HEAD") {
      res.end();
      return;
    }
    const stream = fs.createReadStream(file);
    stream.on("error", (e) => {
      process.stderr.write(`update-server: read failed for ${file}: ${e.message}\n`);
      res.destroy(e); // headers are out: abort so the client sees a short body, not a "complete" one
    });
    stream.pipe(res);
  });
}

const server = http.createServer((req, res) => {
  const started = process.hrtime.bigint();
  res.on("close", () => {
    const ms = Number(process.hrtime.bigint() - started) / 1e6;
    const done = res.writableFinished ? "" : " (aborted)";
    process.stdout.write(
      `${new Date().toISOString()} ${req.socket.remoteAddress} ${req.method} ${req.url} ${res.statusCode} ${ms.toFixed(1)}ms${done}\n`,
    );
  });

  if (req.method !== "GET" && req.method !== "HEAD") {
    notFound(res);
    return;
  }
  const rawPath = (req.url || "").split("?")[0];
  if (rawPath === "/version.json" || rawPath === "/update/version.json") {
    serveVersion(res);
    return;
  }
  for (const prefix of ["/download/", "/update/"]) {
    if (rawPath.startsWith(prefix)) {
      serveDownload(res, rawPath.slice(prefix.length));
      return;
    }
  }
  notFound(res);
});

server.on("error", (e) => {
  if (e.code === "EADDRINUSE") die(`port ${opts.port} on ${HOST} is already in use`);
  die(`server error: ${e.message}`);
});

server.listen(opts.port, HOST, () => {
  const { port } = server.address();
  process.stdout.write(
    `update-server: listening on http://${HOST}:${port} (version file ${opts.versionFile}, installers ${opts.installerDir})\n`,
  );
});

for (const sig of ["SIGINT", "SIGTERM"]) {
  process.on(sig, () => {
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 1000).unref();
  });
}
