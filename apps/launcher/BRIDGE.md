# Solar Launcher web ↔ native bridge contract (AUTHORITATIVE; code name GAGE-MP)

Both the React SPA and the C++ WebView2 host MUST follow this exactly.

## Transport
- JS → native: `window.chrome.webview.postMessage(obj)` (obj is a JS object; it is
  serialized to JSON on the wire).
- native → JS: native calls `PostWebMessageAsJson(jsonString)`. JS receives it via
  `window.chrome.webview.addEventListener("message", (e) => { const data = e.data; ... })`
  where `e.data` is the **already-parsed object**.
- Every message is a plain object with a required `type` string discriminator.
- The SPA is served from memory by the host at `https://app.gage/index.html`
  (WebResourceRequested; there is no server and no size limit). Only that origin
  may use the bridge: native ignores a message whose source is not
  `https://app.gage/…`, cancels every navigation away from it, and never opens a
  second WebView window. A user-initiated `http(s)` link (`<a target="_blank">`,
  `window.open`) is opened in the default browser; other schemes are dropped.
  Any other path under `https://app.gage/` answers 404.

## Window chrome (frameless)
The native window has **no title bar and no caption buttons**; the SPA draws its own
and drives them with `minimize` / `close` / `quit`. Native keeps the taskbar button,
Alt+Tab, Win11 snap, the minimize animation, the DWM shadow and rounded corners.
- Drag region: CSS `app-region: drag` makes an element behave like the title bar
  (drag the window, snap, double-click = maximize/restore, right-click = system menu).
  `app-region: no-drag` carves buttons, inputs and anything clickable out of it.
  Children of a drag element inherit the drag, so every interactive element inside
  a drag region needs its own `no-drag`. `app-region` is a no-op in a plain browser.
- Resizing: left/right/bottom edges are the native sizing frame outside the page; the
  top edge is served by WebView2 itself, which treats roughly the first 8 px of the
  page as the top resize border. Do not place clickable controls in the top 8 px.
- The client area opens on about a third of the work area in 1240x800 proportions,
  never below 1240x800 DIP (scaled by DPI) and never past 90% of the work area
  (smaller screens clamp to it). The SPA scales its layout up to the window (main.jsx).
  When maximized the page fills the work area exactly.
- All server objects use this exact shape:
  `{ "host": string, "port": number, "name": string, "players": number, "ping": number, "url": string,
     "maxPlayers": number, "mode": string, "lang": string }`
  (`ping`, `players` and `maxPlayers` are `-1` when unknown; `mode` (game mode, e.g. `"roleplay"`)
  and `lang` (language code, e.g. `"ru"`) are `""` when unknown). `maxPlayers`, `mode` and `lang`
  come from the list fetch only: saved favorites/recents carry the unknown values, and the SPA
  fills them from the live list. The SPA treats a missing field as unknown.
  `ping` is the TCP connect time of whichever answers first, the game port or the port after
  it. Solar RP game ports are UDP (default 24800) and a Solar RP server listens on no TCP port, so
  today a Solar RP server never answers and `ping` stays -1; a UDP ping replaces this in
  `docs/ENGINE-PLAN.md` phase P2.

## JS → native (commands). JS calls these; native acts and replies with a *result* message.
  `{"type":"ready"}`
      SPA finished mounting. Native must then send `settings`, `favorites`, `recents`
      and, if a fetch is already in flight, the current `servers` message.
  `{"type":"fetch"}`
      Fetch the server list. Native replies (one or more):
        `{"type":"servers","status":"loading","servers":[]}`
        then `{"type":"servers","status":"ok","servers":[...]}` or
             `{"type":"servers","status":"error","servers":[...],"error":"<msg>"}`
      After an `ok` list native pings it on its own: every row whose `ping` is -1, busiest
      (`players`) first, at most 250, all at once (net::PingMany), and streams
      `{"type":"pings","results":[{"row":<masterIndex>,"ms":<int>},...]}` batches (at most
      every 40 ms; `ms` -1 = no reply). Every pinged row gets exactly one result unless a
      newer fetch starts, which stops the pinger. The SPA patches those rows itself.
  `{"type":"refresh"}`  — same behavior as `fetch`.
  `{"type":"ping","row": <masterIndex:number>}`
      Re-ping one server (a click). Native replies `{"type":"ping","row":<masterIndex>,"ms":<int>}`
      (`ms` -1 = no reply, or no such row) and updates its own copy of the list; it does
      not re-send the list, so the SPA patches that row itself. A reply whose list was
      refetched meanwhile is dropped. Returns on the first reply (worst case ~1.6 s).
  `{"type":"launch","server":<serverObj>,"gamePath":"<string>","playerName":"<string>"}`
      Start GTA V and load Solar RP into it. A running game is attached to;
      otherwise the store that installed it starts it (PlayGTAV.exe for the
      Rockstar Games Launcher, steam:// for Steam, the Epic launcher URI),
      never GTA5.exe directly, with `-nobattleye` where the store passes it on.
      The chosen server reaches the engine through HKCU\Software\GAGE-MP
      (`server_host`, `server_port`, `player_name`); gage_mp.dll connects to it
      over the GAGE protocol (UDP, default port 24800) in the background after
      injection. The launcher cannot see whether that join succeeded, and no
      message may claim it. Native replies
        `{"type":"status","text":"<human>"}` (0..n times during launch; at least
        every 5 s while waiting for the store to bring the game up), and finally
        `{"type":"launchResult","ok":<bool>,"code":<int>,"error":"<msg|empty>"}`
      (There is no `assetProgress` message any more. Since 0.4.14 the launcher downloads the
      server's car packs on a Legacy PLAY before the game starts (docs/UPDATES.md "Car packs");
      that progress arrives as `status` lines ("Checking cars…", "Downloading cars: X of Y MB (P%)…"),
      and a failed download never fails the launch.)
      `error` is always empty when `ok` is true (success detail arrives as a `status`).
      While a launch is in progress, further `launch` messages are ignored; an
      invalid server (empty host / port outside 0..65535) gets `launchResult` code 5.
      Codes: 1 no GTA5.exe found (the SPA offers a jump to the game folder
      setting); 2 the game process could not be opened; 3 gageinjector.dll
      missing; 4 injection failed; 6 registry write failure; 7 BattlEye is
      running (Solar RP never loads into it); 8 the store could not be asked to
      start the game; 9 the game did not start within 3 minutes; 10 unused
      (was: the server's assets could not be downloaded); 11 one of Solar RP's own DLLs is missing or differs from the build
      (checked before anything else starts; nothing is injected); 12 no join ticket and
      13 demo mode (apps/launcher-ui/NATIVE-INTEGRATION.md); 14 an update stands in the way:
      an update is pending (**every** available update counts: the host reports `mandatory: true`
      for all of them; downloading, waiting for GTA V to close, or failed
      and being retried by this very press), or setup has already been started and the
      launcher is about to exit, or the server speaks a newer protocol than this launcher, or the server's UDP
      query (format v2, the "update ping") reports a latest client newer than this launcher even though the
      protocol is the same
      (`error` = `"Solar RP was updated. Updating the launcher..."`; the host has started an update
      check); 15 the server is **behind** this launcher: it has not been updated yet
      (`error` = `"This server has not been updated yet. Try again in a minute."`; nothing to install,
      retry later; an SPA that does not know code 15 falls back to the generic "Launch failed" with this
      `error` text). `error` carries the player-facing sentence, e.g.
      `"Solar Launcher 0.3.5 is required to play. It is being installed now."` Nothing is started or
      injected; the update check runs before every other `launch` step (docs/UPDATES.md).
      If the last update check is older than a minute, `launch` first starts one and waits for it, at most
      8 s (a `status` message `"Checking for updates…"` is sent meanwhile; a second `launch` during the wait
      is ignored), then continues or refuses with code 14 as above. The protocol check of codes 14 / 15
      asks the target server's UDP query (1.5 s at most) as the first step of the launch worker.
      `gamePath` may be the game folder or the exe itself.
  `{"type":"browseGamePath"}`
      Open a file picker for `GTA5.exe` / `GTA5_Enhanced.exe` (any other file is refused
      with a message box and the setting is unchanged). Native stores the game *folder*
      for `GTA5.exe` and the exact exe path for `GTA5_Enhanced.exe`, then replies with
      the full `settings` object (see below), `gamePath` = the stored value.
      On `ready`, when no game path is stored, native first searches the stores' records
      (`launch::FindInstalledGame`: Rockstar registry and standard folders, every Steam
      library, Epic's install list; Legacy `GTA5.exe` before `GTA5_Enhanced.exe`) and
      stores what it finds the same way, so the first `settings` already carries it. An
      empty `gamePath` therefore means "not found": the SPA asks the player to choose it.
  `{"type":"getSettings"}`
      Native replies `settings` (see below).
  `{"type":"saveSetting","key":"<string>","value":"<string|number|bool>"}`
      Persist one setting. Native replies the full `settings`. Accepted keys:
      gamePath, playerName, language, autoUpdate, startMinimized, closeGame; anything
      else is ignored (logged). String values are trimmed.
      The server list always comes from the built-in Solar RP master server URL
      (`kDefaultListUrl`, `http://127.0.0.1:8080/servers` until a public master
      exists); the only override is a hand-edited `list_url=` line in gage_mp.cfg
      (a deployed master, testing). There is
      no setting for it: the old `serverEndpoint` key is refused and its saved value
      ignored.
  `{"type":"addFavorite","server":<serverObj>}`
      Native replies `favorites` (see below). A server with an empty `host` or a
      `port` outside 1..65535 is rejected (logged); the reply is then the unchanged list.
  `{"type":"removeFavorite","key":"<host>:<port>"}`
      Native replies `favorites`.
  `{"type":"clearFavorites"}`
      Native replies `favorites`.
  `{"type":"addRecent","server":<serverObj>}`
      Native replies `recents` (same validation as `addFavorite`).
  `{"type":"clearRecents"}`
      Native replies `recents`.
  `{"type":"checkUpdate"}`
      Native replies with the `update` message (see below). The launcher also checks once
      by itself right after start (whether or not `autoUpdate` is on) and reports through the
      same message. While an earlier pass is still downloading, installing or waiting for the
      game to close, `checkUpdate` starts nothing new: it re-sends where that pass stands.
      The manifest is fetched from `update_url` (HKCU\Software\GAGE-MP, set only by a signed
      manifest's `updateUrl`) first and then from the URL baked into the exe
      (`GAGE_UPDATE_URL`); see docs/UPDATES.md.
  `{"type":"copyText","text":"<string, 1..512 chars>"}`
      Put plain text on the Windows clipboard (the row menu's Copy address). Native
      replies `{"type":"copied","ok":<bool>}`.
  `{"type":"minimize"}`
  `{"type":"close"}`      — hide to tray (keep running); minimizes instead if the tray icon could not be created.
  `{"type":"quit"}`       — exit process. With `closeGame` on, native closes the game
      process **this launcher started or injected into** (never another GTA5.exe),
      asking it to close first and terminating it after 3 s; the window is hidden
      meanwhile and the UI thread is not blocked.

## native → JS (events/results)
  `{"type":"settings","gamePath":"..","playerName":"..","language":"..","autoUpdate":bool,
      "startMinimized":bool,"closeGame":bool,"version":"<semver>"}`
  `{"type":"copied","ok":<bool>}`
  `{"type":"servers","status":"loading"|"ok"|"error","servers":[<serverObj>...],"error":"<msg|empty>"}`
  `{"type":"ping","row":<masterIndex>,"ms":<int>}`
  `{"type":"pings","results":[{"row":<masterIndex>,"ms":<int>},...]}`
  `{"type":"favorites","servers":[<serverObj>...]}`
  `{"type":"recents","servers":[<serverObj>...]}`
  `{"type":"launchResult","ok":<bool>,"code":<int>,"error":"<msg|empty>"}`
  `{"type":"status","text":"<human>"}`
  `{"type":"update","current":"<semver>","latest":"<semver>"|"","available":<bool>,"error":"<msg|empty>",
      "state":"idle|checking|downloading|verifying|ready|installing|deferred|error",
      "progress":<0..1>,"mandatory":<bool>,"notes":"<text|empty>"}`
      Sent on every state or progress change (progress: at most once per percent) and as the
      reply to `checkUpdate`. Everything but `current` comes from a manifest whose signature
      verified, or from the updater itself; nothing unsigned is ever relayed.
      - `latest` / `available`: the newest version a verified manifest names, and whether it is
        strictly newer than `current`. After a failed check `latest` is `""` and `available`
        false, except that a known mandatory update stays pending.
      - `state`: `idle` (nothing running; also "update known, automatic updates off"),
        `checking`, `downloading` (`progress` moves 0..1), `verifying` (SHA-256 of the
        finished download), `ready` (verified, about to start), `installing` (setup started;
        the launcher exits about 1.5 s later and the new version starts by itself),
        `deferred` (verified installer in hand, but GTA V runs with Solar RP loaded: it is
        started when the game exits, retried every 15 s), `error` (`error` says which step).
      - `mandatory`: the pending update must be installed before `launch` works (code 14). The host
        now reports `mandatory: true` for **every** available update (`mandatory == available`), whatever
        the signed manifest's flag says, so the SPA can rely on it. Every available update downloads and
        installs at once, even with `autoUpdate` off (the setting is stored and echoed in `settings`, but no
        longer gates updates).
      - When updates are looked for (no SPA action needed; each pass reports through this message): at
        start, every 2 minutes while the launcher is open, and (at most once a minute) when the window is
        brought forward or restored from the tray, when the game exits, on `launch`, and when a server
        reports a newer protocol. `checkUpdate` is never rate-limited.
      - `notes`: release notes from the signed payload, at most 2000 characters.
      A host that predates this contract sends the first five fields only; the SPA treats a
      missing `state` as "unknown" (apps/launcher-ui/src/lib/bridge.ts `updateStatusText`).

## Ordering & invariants
- The SPA must call `ready` exactly once after mount.
- Native may send `servers` at any time (fetch results, ping updates). The SPA
  treats each `servers` message as a full replace of the list.
- Native is the single source of truth for settings, favorites, and recents.
  The SPA never persists them itself (no localStorage required).
- No network requests are made by the SPA; all I/O goes through the bridge.
  (The SPA must therefore be fully self-contained: inline CSS + JS, no external
  URLs, no font CDN, system font stack.)
