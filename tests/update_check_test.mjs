#!/usr/bin/env node
// Driver for update_check_test.exe, the test of the launcher's signed self-update
// (apps/launcher/updater.cpp, docs/UPDATES.md).
//
//   node tests/update_check_test.mjs --exe <path\to\update_check_test.exe>
//
// It generates two THROWAWAY ECDSA keys (tools/update-sign/keygen.mjs, in a temp folder; the real
// private key is never touched), signs every fixture with tools/update-sign/sign.mjs (payloads
// that sign.mjs refuses to write are signed here with the same algorithm), starts a local HTTP
// server for the download scenarios plus tools/update-server/serve.mjs, runs the exe against
// them, and cross-checks the exe's verdicts with tools/update-sign/verify.mjs (Node's own
// verifier). Servers are started as children and stopped through their own handles.
//
// Nothing here, and nothing in the exe, ever starts an installer: the fixture "installer" is
// random bytes, and a static guard below fails the run if the exe's source mentions a way to run one.
// Exit 0 only if every scenario ran and passed.

import { spawn, execFile } from "node:child_process";
import { createHash, createPrivateKey, sign as cryptoSign } from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..");
const SERVE = path.join(ROOT, "tools", "update-server", "serve.mjs");
const KEYGEN = path.join(ROOT, "tools", "update-sign", "keygen.mjs");
const SIGN = path.join(ROOT, "tools", "update-sign", "sign.mjs");
const VERIFY = path.join(ROOT, "tools", "update-sign", "verify.mjs");
const GUI_CPP = path.join(ROOT, "apps", "launcher", "gage_gui.cpp");
const TEST_CPP = path.join(HERE, "update_check_test.cpp");
const EXPECTED_SCENARIOS = 8;
const MAX_INSTALLER = 512 * 1024 * 1024; // updater.h kMaxInstallerBytes

function fatal(msg) {
  process.stderr.write(`update_check_test: ${msg}\n`);
  process.exit(2);
}

// ---- args -------------------------------------------------------------------
let exe = null;
{
  const argv = process.argv.slice(2);
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--exe" && argv[i + 1]) exe = path.resolve(argv[++i]);
    else fatal(`unknown or incomplete argument '${argv[i]}'`);
  }
}
if (!exe) fatal("--exe <update_check_test.exe> is required");
if (!fs.existsSync(exe)) fatal(`exe not found: ${exe}`);

const guiSrc = fs.readFileSync(GUI_CPP, "utf8");
const cm = guiSrc.match(/kClientVersion\s*=\s*L"([^"]*)"/);
if (!cm || !/^\d+\.\d+\.\d+$/.test(cm[1])) fatal(`no kClientVersion in ${GUI_CPP}`);
const CLIENT = cm[1];

// ---- helpers ----------------------------------------------------------------
const results = [];
let currentScenario = null;
let failed = false;

function check(name, ok, detail = "") {
  if (!currentScenario) fatal("check() outside a scenario");
  currentScenario.checks++;
  if (!ok) {
    currentScenario.ok = false;
    failed = true;
  }
  console.log(`  ${ok ? "ok  " : "FAIL"} ${name}${detail ? `  (${detail})` : ""}`);
}

async function scenario(name, fn) {
  currentScenario = { name, ok: true, checks: 0 };
  console.log(`[${name}]`);
  try {
    await fn();
  } catch (e) {
    check("scenario threw", false, e && e.stack ? e.stack : String(e));
  }
  if (currentScenario.checks === 0) check("scenario made at least one check", false);
  results.push(currentScenario);
  currentScenario = null;
}

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.once("error", reject);
    s.listen(0, "127.0.0.1", () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });
}

function run(file, args, opts = {}) {
  return new Promise((resolve) => {
    execFile(file, args, { timeout: 120000, windowsHide: true, maxBuffer: 16 * 1024 * 1024, ...opts }, (error, stdout, stderr) => {
      const code = error ? (typeof error.code === "number" ? error.code : -1) : 0;
      resolve({ code, stdout: String(stdout), stderr: String(stderr) });
    });
  });
}

function startServeMjs(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [SERVE, ...args], { stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    let err = "";
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`serve.mjs did not start in 10 s\nstdout: ${out}\nstderr: ${err}`));
    }, 10000);
    child.stdout.on("data", (d) => {
      out += d;
      const hit = out.match(/listening on http:\/\/127\.0\.0\.1:(\d+)/);
      if (hit) {
        clearTimeout(timer);
        resolve({ child, port: Number(hit[1]), log: () => out, errLog: () => err });
      }
    });
    child.stderr.on("data", (d) => (err += d));
    child.once("exit", (code) => {
      clearTimeout(timer);
      reject(new Error(`serve.mjs exited early (code ${code})\nstdout: ${out}\nstderr: ${err}`));
    });
  });
}

function stopChild(srv) {
  return new Promise((resolve) => {
    if (srv.child.exitCode !== null || srv.child.signalCode !== null) return resolve();
    srv.child.once("exit", () => resolve());
    srv.child.kill();
  });
}

function rawGet(port, rawPath) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: "127.0.0.1", port, path: rawPath, method: "GET", agent: false }, (res) => {
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
    });
    req.on("error", reject);
    req.end();
  });
}

// ---- fixtures ---------------------------------------------------------------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "gage-update-test-"));
const fx = path.join(tmp, "fx");
const work = path.join(tmp, "work");
const keys1 = path.join(tmp, "k1");
const keys2 = path.join(tmp, "k2");
for (const d of [fx, work]) fs.mkdirSync(d);

// A deterministic stand-in installer: 300 KiB + 7 bytes of non-text.
const installer = Buffer.alloc(300 * 1024 + 7);
for (let i = 0; i < installer.length; i++) installer[i] = (i * 31 + 7) & 0xff;
const installerPath = path.join(fx, "installer.bin");
fs.writeFileSync(installerPath, installer);
const installerSha = createHash("sha256").update(installer).digest("hex");

async function keygen(dir) {
  const r = await run(process.execPath, [KEYGEN, "--out-dir", dir]);
  if (r.code !== 0) fatal(`keygen failed: ${r.stderr}`);
  return {
    pem: path.join(dir, "update-signing-key.pem"),
    pub: fs.readFileSync(path.join(dir, "update-signing-pub.txt"), "utf8").trim(),
  };
}

// tools/update-sign/sign.mjs, exactly as a release would run it.
async function signWith(key, out, { version, url, notes = "", mandatory = false, updateUrl = "", installerFile = installerPath }) {
  const args = [SIGN, "--key", key.pem, "--installer", installerFile, "--version", version, "--url", url, "--out", out];
  if (notes) args.push("--notes", notes);
  if (mandatory) args.push("--mandatory");
  if (updateUrl) args.push("--update-url", updateUrl);
  const r = await run(process.execPath, args);
  if (r.code !== 0) fatal(`sign.mjs failed for ${out}: ${r.stderr}`);
}

// The same algorithm for payloads sign.mjs would refuse to write (it validates its inputs).
function craft(key, payload, topLevel = {}) {
  const bytes = Buffer.from(typeof payload === "string" ? payload : JSON.stringify(payload), "utf8");
  const sig = cryptoSign("sha256", bytes, { key: createPrivateKey(fs.readFileSync(key.pem)), dsaEncoding: "ieee-p1363" });
  return JSON.stringify({ version: "9.9.9", url: "", notes: "", ...topLevel, signed: bytes.toString("base64"), sig: sig.toString("base64") }, null, 2) + "\n";
}

const readJson = (f) => JSON.parse(fs.readFileSync(f, "utf8"));
const put = (name, text) => fs.writeFileSync(path.join(fx, name), text);
const flipByte = (buf, at) => { const b = Buffer.from(buf); b[at] ^= 0x20; return b; };

// ---- test HTTP server -------------------------------------------------------
const served = {
  good: installer,
  tamper: Buffer.from(Uint8Array.from(installer, (b, i) => (i === 100 ? b ^ 0xff : b))), // same length, one byte differs
  short: installer.subarray(0, installer.length - 1000),                       // fewer bytes than signed, honest Content-Length
  long: Buffer.concat([installer, Buffer.alloc(1000, 0x41)]),                  // more bytes than signed, honest Content-Length
};
const manifests = {}; // url path -> body, filled below
const requestLog = [];
const server = http.createServer((req, res) => {
  const p = (req.url || "").split("?")[0];
  requestLog.push(`${req.method} ${p}`);
  if (manifests[p]) {
    res.writeHead(200, { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(manifests[p]), "Cache-Control": "no-store" });
    res.end(manifests[p]);
    return;
  }
  const m = p.match(/^\/(good|tamper|short|long)\/SolarRP-Setup-9\.9\.9\.exe$/);
  if (m) {
    const body = served[m[1]];
    res.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Length": body.length });
    res.end(body);
    return;
  }
  if (p === "/chunklong/SolarRP-Setup-9.9.9.exe") {
    // no Content-Length: chunked, and 5000 bytes more than the signed size
    res.writeHead(200, { "Content-Type": "application/octet-stream" });
    const all = Buffer.concat([installer, Buffer.alloc(5000, 0x42)]);
    let at = 0;
    const tick = () => {
      if (at >= all.length) return res.end();
      res.write(all.subarray(at, at + 16384));
      at += 16384;
      setImmediate(tick);
    };
    tick();
    return;
  }
  res.writeHead(404, { "Content-Type": "text/plain" });
  res.end("not found\n");
});
await new Promise((resolve, reject) => { server.once("error", reject); server.listen(0, "127.0.0.1", resolve); });
const BASE = `http://127.0.0.1:${server.address().port}`;

// ---- run --------------------------------------------------------------------
const servers = [];

try {
  const k1 = await keygen(keys1);
  const k2 = await keygen(keys2);
  put("pub1.txt", k1.pub + "\n");
  put("pub2.txt", k2.pub + "\n");
  const deadPort = await freePort();

  // Standard manifests, written by sign.mjs.
  const goodUrl = `${BASE}/good/SolarRP-Setup-9.9.9.exe`;
  const f = (n) => path.join(fx, n);
  await signWith(k1, f("good.json"), { version: "9.9.9", url: goodUrl });
  await signWith(k2, f("wrongkey.json"), { version: "9.9.9", url: goodUrl });
  await signWith(k1, f("equal.json"), { version: CLIENT, url: goodUrl });
  await signWith(k1, f("older.json"), { version: "0.0.1", url: goodUrl });
  await signWith(k1, f("equal-moved.json"), { version: CLIENT, url: goodUrl, updateUrl: `${BASE}/moved/version.json` });
  await signWith(k1, f("older-moved.json"), { version: "0.0.1", url: goodUrl, updateUrl: `${BASE}/moved/version.json` });
  await signWith(k1, f("mandatory.json"), { version: "9.9.8", url: goodUrl, notes: "Fixes the join crash.", mandatory: true, updateUrl: `${BASE}/moved/version.json` });
  for (const [name, dir] of [["tamper", "tamper"], ["short", "short"], ["long", "long"], ["chunklong", "chunklong"], ["missing", "missing"]])
    await signWith(k1, f(`${name}.json`), { version: "9.9.9", url: `${BASE}/${dir}/SolarRP-Setup-9.9.9.exe` });
  await signWith(k1, f("dead.json"), { version: "9.9.9", url: `http://127.0.0.1:${deadPort}/SolarRP-Setup-9.9.9.exe` });
  manifests["/good/version.json"] = fs.readFileSync(f("good.json"), "utf8");

  const good = readJson(f("good.json"));
  const base = JSON.parse(Buffer.from(good.signed, "base64").toString("utf8"));

  // Damaged copies of the good manifest (the signature no longer matches, or the base64 is broken).
  const signedBytes = Buffer.from(good.signed, "base64");
  const sigBytes = Buffer.from(good.sig, "base64");
  const flippedAt = signedBytes.indexOf("9.9.9");
  put("flipped.json", JSON.stringify({ ...good, signed: flipByte(signedBytes, flippedAt).toString("base64") }, null, 2));
  put("badsig.json", JSON.stringify({ ...good, sig: flipByte(sigBytes, 10).toString("base64") }, null, 2));
  put("trunc-signed.json", JSON.stringify({ ...good, signed: good.signed.slice(0, good.signed.length - 5) }, null, 2));
  put("trunc-4.json", JSON.stringify({ ...good, signed: good.signed.slice(0, good.signed.length - 8) }, null, 2));
  put("trunc-sig.json", JSON.stringify({ ...good, sig: good.sig.slice(0, good.sig.length - 3) }, null, 2));
  put("sig63.json", JSON.stringify({ ...good, sig: sigBytes.subarray(0, 63).toString("base64") }, null, 2));
  put("sig65.json", JSON.stringify({ ...good, sig: Buffer.concat([sigBytes, Buffer.from([0])]).toString("base64") }, null, 2));
  put("legacy.json", JSON.stringify({ version: "9.9.9", url: goodUrl, notes: "" }));
  put("unsigned-tampered.json", JSON.stringify({ ...good, version: "1.0.0", url: "http://evil.example/x.exe", notes: "pwned" }, null, 2));
  put("garbage.json", "this is not json at all");
  put("array.json", "[1,2,3]");
  put("big.json", fs.readFileSync(f("good.json"), "utf8") + " ".repeat(70000));
  put("bom.json", Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), fs.readFileSync(f("good.json"))]));

  // Valid signatures over payloads the launcher must refuse (each has a control above or below).
  const withPayload = (name, mutate) => {
    const p = { ...base };
    mutate(p);
    put(name, craft(k1, p, { version: p.version ?? "9.9.9", url: p.url ?? "" }));
  };
  withPayload("neg-v2.json", (p) => { p.v = 2; });
  withPayload("neg-v-missing.json", (p) => { delete p.v; });
  withPayload("neg-version-short.json", (p) => { p.version = "1.2"; });
  withPayload("neg-version-v.json", (p) => { p.version = "v9.9.9"; });
  withPayload("neg-version-four.json", (p) => { p.version = "9.9.9.1"; });
  withPayload("neg-version-number.json", (p) => { p.version = 5; });
  withPayload("neg-sha-short.json", (p) => { p.sha256 = p.sha256.slice(0, 63); });
  withPayload("neg-sha-nothex.json", (p) => { p.sha256 = "z".repeat(64); });
  withPayload("neg-sha-missing.json", (p) => { delete p.sha256; });
  withPayload("neg-size-zero.json", (p) => { p.size = 0; });
  withPayload("neg-size-negative.json", (p) => { p.size = -5; });
  withPayload("neg-size-huge.json", (p) => { p.size = 1e15; });
  withPayload("neg-size-fraction.json", (p) => { p.size = 12.5; });
  withPayload("neg-size-string.json", (p) => { p.size = "123"; });
  withPayload("neg-size-cap-plus-one.json", (p) => { p.size = MAX_INSTALLER + 1; });
  withPayload("pos-size-cap.json", (p) => { p.size = MAX_INSTALLER; });
  withPayload("neg-url-ftp.json", (p) => { p.url = "ftp://127.0.0.1/SolarRP-Setup-9.9.9.exe"; });
  withPayload("neg-url-space.json", (p) => { p.url = "http://127.0.0.1/a b.exe"; });
  withPayload("neg-url-missing.json", (p) => { delete p.url; });
  withPayload("neg-url-long.json", (p) => { p.url = "http://127.0.0.1/" + "a".repeat(2050); });
  withPayload("neg-mandatory-string.json", (p) => { p.mandatory = "yes"; });
  withPayload("neg-notes-long.json", (p) => { p.notes = "n".repeat(2001); });
  withPayload("pos-notes-2000.json", (p) => { p.notes = "n".repeat(2000); });
  withPayload("neg-notes-number.json", (p) => { p.notes = 5; });
  withPayload("neg-updateurl-number.json", (p) => { p.updateUrl = 5; });
  withPayload("pos-sha-upper.json", (p) => { p.sha256 = p.sha256.toUpperCase(); });
  withPayload("updateurl-ftp.json", (p) => { p.updateUrl = "ftp://example.com/version.json"; });
  withPayload("updateurl-long.json", (p) => { p.updateUrl = "http://example.com/" + "a".repeat(300); });
  withPayload("updateurl-256.json", (p) => { p.updateUrl = "http://example.com/" + "a".repeat(256 - 19); });
  put("neg-not-object.json", craft(k1, "[1]"));

  // serve.mjs: the dev update server, with the installer under its /download/ name.
  const servePort = await freePort();
  const serveDir = path.join(tmp, "serve");
  fs.mkdirSync(path.join(serveDir, "installers"), { recursive: true });
  fs.copyFileSync(installerPath, path.join(serveDir, "installers", "SolarRP-Setup-9.9.9.exe"));
  fs.copyFileSync(installerPath, path.join(serveDir, "installers", "GAGE-MP-Setup-9.9.9.exe"));   // a pre-rename release keeps its name
  await signWith(k1, path.join(serveDir, "version.json"), { version: "9.9.9", url: `http://127.0.0.1:${servePort}/download/SolarRP-Setup-9.9.9.exe` });
  const serveSrv = await startServeMjs(["--port", String(servePort), "--version-file", path.join(serveDir, "version.json"), "--installer-dir", path.join(serveDir, "installers")]);
  servers.push(serveSrv);

  // ---- scenarios ------------------------------------------------------------
  await scenario("a: fixtures are what the test thinks they are (Node's own verifier)", async () => {
    const v = await run(process.execPath, [VERIFY, "--pub", k1.pub, "--manifest", f("good.json"), "--installer", installerPath]);
    check("verify.mjs accepts good.json with the installer", v.code === 0, v.stdout.trim() || v.stderr.trim());
    for (const [name, why] of [["flipped.json", "signature does not verify"], ["badsig.json", "signature does not verify"], ["wrongkey.json", "signature does not verify"]]) {
      const r = await run(process.execPath, [VERIFY, "--pub", k1.pub, "--manifest", f(name)]);
      check(`verify.mjs rejects ${name}`, r.code !== 0 && r.stderr.includes(why), r.stderr.trim());
    }
    const t = await run(process.execPath, [VERIFY, "--pub", k1.pub, "--manifest", f("tamper.json"), "--installer", installerPath]);
    check("verify.mjs accepts tamper.json's manifest (its signature is fine; only the served bytes differ)", t.code === 0, t.stderr.trim());
    check("payload carries the installer's real sha256 and size", base.sha256 === installerSha && base.size === installer.length && base.v === 1,
      `${base.sha256.slice(0, 12)} ${base.size}`);
    check("the five payload fields sign.mjs documents are all there", ["version", "url", "sha256", "size", "notes", "mandatory", "updateUrl", "issued"].every((k) => k in base));
    check("tampered download differs from the signed bytes but not in length",
      served.tamper.length === installer.length && !served.tamper.equals(installer) && served.short.length < installer.length && served.long.length > installer.length);
  });

  await scenario("b: static guard - the test never starts an installer", async () => {
    const lines = fs.readFileSync(TEST_CPP, "utf8").split(/\r?\n/).filter((l) => !l.trim().startsWith("//"));
    const code = lines.join("\n");
    check("source is not empty", code.length > 5000, `${code.length} chars`);
    for (const word of ["StartInstaller", "CreateProcess", "ShellExecute", "WinExec", "system(", "_wsystem", "_spawn", "_exec"])
      check(`no '${word}' in the test's code`, !code.includes(word));
  });

  const exeArgs = [
    "--fixtures", fx, "--work", work, "--base", BASE, "--serve-base", `http://127.0.0.1:${servePort}`, "--client", CLIENT,
  ];
  const realManifest = path.join(os.tmpdir(), "signt", "version.json");
  if (fs.existsSync(realManifest)) exeArgs.push("--real-manifest", realManifest);

  await scenario("c: update_check_test.exe (primitives, signature, payload, check, download)", async () => {
    const r = await run(exe, exeArgs);

    const lines = r.stdout.split(/\r?\n/);
    for (const line of lines) if (/FAIL|SKIPPED|^CHECKS|^\[[A-Za-z]/.test(line)) console.log(`      exe> ${line}`);
    if (r.stderr.trim()) console.log(`      exe stderr> ${r.stderr.trim()}`);
    const tail = lines.filter((l) => l.startsWith("CHECKS ")).pop() || "";
    const t = tail.match(/^CHECKS (\d+) FAILED (\d+)$/);
    check("exe printed its CHECKS summary", !!t, tail || "none");
    check("exe exited 0", r.code === 0, `exit ${r.code}`);
    check("no FAIL line", !lines.some((l) => /^\s*FAIL/.test(l)));
    check("at least 150 checks ran (not a vacuous pass)", !!t && Number(t[1]) >= 150, t ? t[1] : "?");
    check("failed count is 0", !!t && t[2] === "0", t ? t[2] : "?");
    const sections = lines.filter((l) => /^\[[A-Za-z]/.test(l)).length; // the logger's lines start with [hh:mm:ss
    check("all 11 test sections ran", sections === 11, `${sections} section headers`);
    check("real-key manifest was checked (or explicitly skipped)", fs.existsSync(realManifest) ? !lines.some((l) => l.includes("SKIPPED")) : lines.some((l) => l.includes("SKIPPED")));
  });

  await scenario("d: the exe really used the network (server saw every download path)", async () => {
    const seen = (re) => requestLog.some((l) => re.test(l));
    check("GET /good/version.json", seen(/^GET \/good\/version\.json$/));
    for (const dir of ["good", "tamper", "short", "long", "chunklong", "missing"])
      check(`GET /${dir}/SolarRP-Setup-9.9.9.exe`, seen(new RegExp(`^GET /${dir}/SolarRP-Setup-9\\.9\\.9\\.exe$`)));
    const serveLog = serveSrv.log();
    check("serve.mjs saw GET /version.json and GET /download/SolarRP-Setup-9.9.9.exe",
      / GET \/version\.json 200/.test(serveLog) && / GET \/download\/SolarRP-Setup-9\.9\.9\.exe 200/.test(serveLog), serveLog.split("\n").filter((l) => /GET/.test(l)).length + " request line(s)");
  });

  // serve.mjs behaviours that the launcher's download depends on (kept from the first version of this test).
  const def = await startServeMjs(["--port", "0"]);
  servers.push(def);

  await scenario("e: serve.mjs /download/ refuses traversal and missing files", async () => {
    const defaultInstallerDir = path.join(ROOT, "build", "win", "installer");
    const traversalTarget = path.resolve(defaultInstallerDir, "..", "..", "..", "CMakeLists.txt");
    check("traversal target is the repo CMakeLists.txt and exists",
      traversalTarget === path.join(ROOT, "CMakeLists.txt") && fs.existsSync(traversalTarget), traversalTarget);
    for (const p of [
      "/download/..%2F..%2F..%2FCMakeLists.txt",
      "/download/../../../CMakeLists.txt",
      "/download/..%5C..%5C..%5CCMakeLists.txt",
      "/download/%2E%2E%2F%2E%2E%2F%2E%2E%2FCMakeLists.txt",
      "/download/SolarRP-Setup-0.1.0.exe%2F..%2F..%2F..%2F..%2FCMakeLists.txt",
      "/download/SolarRP-Setup-9.9.9.exe",
      "/download/",
      "/version.json/../CMakeLists.txt",
      "/CMakeLists.txt",
    ]) {
      const r = await rawGet(def.port, p);
      check(`${p} -> 404`, r.status === 404, `got ${r.status}`);
    }
  });

  await scenario("f: serve.mjs serves a setup file with Content-Length, and the signed manifest unchanged", async () => {
    const r = await rawGet(servePort, "/download/SolarRP-Setup-9.9.9.exe");
    check("200", r.status === 200, `got ${r.status}`);
    check("Content-Length matches file", Number(r.headers["content-length"]) === installer.length, r.headers["content-length"]);
    check("body matches file byte for byte", r.body.equals(installer), `${r.body.length} bytes`);
    const m = await rawGet(servePort, "/version.json");
    check("version.json is served byte for byte as signed", m.body.equals(fs.readFileSync(path.join(serveDir, "version.json"))));
    check("Cache-Control: no-store", m.headers["cache-control"] === "no-store", m.headers["cache-control"]);
    const upd = await rawGet(servePort, "/update/version.json");
    check("the VPS layout /update/version.json answers the same manifest", upd.status === 200 && upd.body.equals(m.body), `got ${upd.status}`);
    const updExe = await rawGet(servePort, "/update/SolarRP-Setup-9.9.9.exe");
    check("the VPS layout /update/SolarRP-Setup-9.9.9.exe answers the installer", updExe.status === 200 && updExe.body.equals(installer), `got ${updExe.status}`);
    const legacyExe = await rawGet(servePort, "/update/GAGE-MP-Setup-9.9.9.exe");
    check("a pre-rename release /update/GAGE-MP-Setup-9.9.9.exe is still served", legacyExe.status === 200 && legacyExe.body.equals(installer), `got ${legacyExe.status}`);
    const otherExe = await rawGet(servePort, "/update/Other-Setup-9.9.9.exe");
    check("a setup name outside both families is a 404", otherExe.status === 404, `got ${otherExe.status}`);
  });

  await scenario("g: serve.mjs exits non-zero when the port is taken", async () => {
    const r = await run(process.execPath, [SERVE, "--port", String(def.port)], { timeout: 10000 });
    check("non-zero exit", typeof r.code === "number" && r.code !== 0, `exit ${r.code}`);
    check("says the port is in use", /already in use/.test(r.stderr), r.stderr.trim());
  });

  await scenario("h: serve.mjs warns about a legacy unsigned version file", async () => {
    const legacy = path.join(tmp, "legacy-version.json");
    fs.writeFileSync(legacy, JSON.stringify({ version: "0.1.3", url: "" }));
    const s = await startServeMjs(["--port", "0", "--version-file", legacy]);
    servers.push(s);
    const text = s.log() + s.errLog();
    check("startup says new launchers will refuse it", /unsigned|no signed payload/i.test(text), text.trim().split("\n").slice(0, 2).join(" | "));
    const ok = await startServeMjs(["--port", "0", "--version-file", path.join(serveDir, "version.json")]);
    servers.push(ok);
    check("a signed file starts without that warning", !/unsigned|no signed payload/i.test(ok.log() + ok.errLog()));
  });
} finally {
  for (const s of servers) await stopChild(s);
  server.closeAllConnections?.();
  await new Promise((resolve) => server.close(resolve));
  fs.rmSync(tmp, { recursive: true, force: true });
}

// ---- summary ----------------------------------------------------------------
const passed = results.filter((r) => r.ok).length;
console.log(`\n${passed}/${results.length} scenarios passed (expected ${EXPECTED_SCENARIOS})`);
if (results.length !== EXPECTED_SCENARIOS) {
  console.log(`FAIL: ran ${results.length} scenarios, expected ${EXPECTED_SCENARIOS}`);
  process.exit(1);
}
process.exit(failed ? 1 : 0);
