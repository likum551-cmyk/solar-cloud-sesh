# Discord login and linking: what the launcher must do

The platform side is finished and tested. The launcher (the native `GAGE-MP.exe` and the WebView2 UI it hosts) is **not changed by this work**, and until it does what this page says a Discord login or link cannot complete: the browser page ends in a `gage-mp://` URL that nothing handles yet. Nothing else about Discord changed for the launcher: it still starts a flow and polls; it now also has to receive a claim and send it.

Request and response shapes are in [api.md](api.md#discord) (reference) and the reasoning is in [api.md](api.md#discord-login-and-linking) (rules). This page is the launcher's side of the contract.

## Why there is a claim

The launcher cannot receive Discord's redirect, so it starts a flow, the player approves in a browser, and the launcher polls for the result. With only a poll secret that is phishable: an attacker starts a flow, sends the Discord authorize URL to a victim, the victim approves, and the attacker's own poll collects a session for the victim's account.

The platform now requires two things to read a result:

| secret | who holds it | how it travels |
| --- | --- | --- |
| `pollSecret` | the launcher that started the flow | returned by `start`, kept in memory, sent in every poll and complete. Never in a URL, a log or the WebView UI |
| `claim` | the browser that approved on Discord | the callback page puts it in a `gage-mp://auth/discord?flow=<flowId>&claim=<claim>` URL; the operating system passes that URL to the launcher on **the same machine** |

Only a machine that holds both can finish the flow. The victim's browser has the claim but the attacker holds the poll secret, and the attacker's launcher never sees the victim's URL.

## 1. Register the `gage-mp` URL scheme, per user

Do it at install time, under the **current user's** hive (no administrator rights, no machine-wide handler for another user to hijack):

```
HKCU\Software\Classes\gage-mp
    (Default)      = "URL:GAGE-MP Protocol"
    "URL Protocol" = ""
HKCU\Software\Classes\gage-mp\DefaultIcon
    (Default)      = "<install dir>\GAGE-MP.exe,0"
HKCU\Software\Classes\gage-mp\shell\open\command
    (Default)      = "\"<install dir>\GAGE-MP.exe\" \"%1\""
```

- Quote `%1`. The URL is attacker-influenced text (anyone can link to `gage-mp://...`); the launcher treats it as untrusted input.
- Re-write the keys on every launcher update and on first start of a portable copy (the path moves), and remove them in the uninstaller.
- The same scheme also serves the links in verification and reset mail (`EMAIL_LINK_BASE_URL`, default `gage-mp://auth`), so route on the path: `gage-mp://auth/discord?...` is this flow, `gage-mp://auth/verify-email?token=...` and `gage-mp://auth/reset-password?token=...` are the mail links.

## 2. Deliver the URL to the running launcher

Windows starts a **new process** with the URL as its argument. The launcher is single-instance (a second instance only wakes the first and exits), so the new process must forward the URL to the running instance before it exits, for example over the existing single-instance channel (`WM_COPYDATA` to the main window, or a per-user named pipe whose ACL admits only the current user), and then exit. If no instance is running, the new process becomes the instance and handles the URL itself after start-up.

Do not open another window or start a second flow when the URL arrives.

## 3. Validate the URL, then use it

Accept the URL only when **all** hold, and ignore (do not report, do not log the claim) anything else:

1. scheme `gage-mp`, host `auth`, path `/discord`, no other path segments;
2. exactly the query parameters `flow` and `claim`;
3. `flow` is a lower-case UUID (`^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$`) and **equals the `flowId` of a flow this launcher started and is still waiting for**. A URL for any other flow is dropped: this is what stops a link on a web page from steering the launcher;
4. `claim` matches `^[A-Za-z0-9_-]{43}$`.

Keep the claim in native memory only. It is not shown, not logged, not written to disk and not passed to the web UI. Never put the `pollSecret` in anything a browser or another process can see.

## 4. Poll and complete with the claim

```
POST /v1/auth/discord/start                                  -> { flowId, authorizeUrl, pollSecret, expiresAt }
   open authorizeUrl in the default browser (ShellExecute)
   wait for the gage-mp://auth/discord URL (section 3)
POST /v1/auth/discord/poll { flowId, pollSecret, claim }     -> { status, ... }
POST /v1/auth/discord/complete { flowId, pollSecret, claim, username }   -> session
```

- **Before the URL arrives** you may poll without a claim (it is optional in the body) and will see `{ "status": "pending" }`; that is also what a wrong claim gets. Polling every 2 seconds is enough; stop at `expiresAt` (10 minutes; `flow_expired`, 410) and offer to start again. Do not treat `pending` after the URL arrived as an error for more than one or two polls.
- **After the URL arrives** poll once with the claim. Results:
  - `{ "status": "complete", "token", "expiresAt", "sessionId", "account" }`: logged in; the flow and the claim are spent (a second poll is 410 `flow_used`);
  - `{ "status": "needs_username", "suggestedUsername", "discordUsername" }`: a new player. Ask for the in-game name (3 to 20 of `[A-Za-z0-9_]`), then `complete` with the same `flowId`, `pollSecret` and `claim`. A taken or reserved name is 409 `username_taken` and does not spend the flow; ask again. `complete` without the claim is 400, with a wrong one 404 `flow_not_found`;
  - `{ "status": "failed", "code": ... }`: show the reason (`access_denied`, `email_in_use`, `discord_error`, `invalid_request`; for a link also `discord_already_linked`, `already_has_discord`, `email_not_verified`). A refused sign-in still hands over a claim so the launcher can read why;
  - `{ "status": "linked" }` (link flows): done.
- `email_in_use` means the Discord email belongs to a GAGE account. Tell the player to log in with their password and link Discord from the settings: there is no automatic merge.
- The `flow`/`claim` URL can arrive **more than once** (a refresh of the page, a second click on "Return to GAGE"). Handle it idempotently: after the flow ended, ignore it.

## 5. Linking from the settings

`POST /v1/me/discord/link` (signed in) returns the same `{ flowId, authorizeUrl, pollSecret, expiresAt }` and the rest is identical, with two differences:

- It is refused with 403 `email_not_verified` while the account's email is unverified. Show "verify your email first" with a resend button (`POST /v1/auth/resend-verification`); the same applies to `DELETE /v1/me/discord` and to setting a first password.
- The identity is attached by the **claimed poll** (`linked`), not by the browser. If the player closes the browser before "Return to GAGE" nothing is linked; start again.

The platform emails the account owner when Discord is linked or unlinked; the launcher does not need to.

## 6. The browser page

`GET /v1/auth/discord/callback` returns a small static page: it redirects to the `gage-mp://` URL immediately (a `<meta http-equiv="refresh">`) and shows a "Return to GAGE" button for the same URL, because some browsers ask for confirmation before opening an external protocol or block a redirect without a click. It never displays the claim. If the scheme is not registered the button does nothing: the launcher should say, while it waits, "Finish in your browser, then click Return to GAGE. If nothing happens, reinstall or repair GAGE-MP", and offer to cancel.

A failed or refused sign-in is HTTP 400 with the same button (so the launcher can show why). An unknown, used or expired `state` is 400 **without** any URL, and the launcher sees `flow_expired` or stays `pending` until it expires.

## 7. Checklist for the native side

- [ ] `gage-mp` registered under `HKCU\Software\Classes` at install and on update, removed on uninstall.
- [ ] A second process started with a `gage-mp://` argument forwards it to the running instance and exits.
- [ ] The URL is validated as in section 3 and matched against a flow this launcher started.
- [ ] `pollSecret` and `claim` live in native memory only; neither is logged or given to the web UI.
- [ ] `claim` is sent with every poll after the URL arrived and with `complete`.
- [ ] `flow_expired` and `flow_used` end the flow in the UI; `needs_username`, `failed` and `email_not_verified` have screens.
- [ ] Manual test on a clean Windows user profile: click login, approve in the browser, the launcher comes to the front, logged in. Repeat with the launcher closed (the URL starts it) and with Discord approval opened on a different machine (the launcher must NOT log in).
