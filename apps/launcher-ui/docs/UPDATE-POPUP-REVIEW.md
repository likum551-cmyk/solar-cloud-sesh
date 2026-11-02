# Forced-update popup: how it maps and what to capture

Reviewer note for the PC. Source: `src/state/updatePopup.ts` (all rules, pure, tested in `tests/update-popup*.test.ts`), `src/screens/UpdatePopup.tsx` (drawing), `.upd*` in `src/styles/screens.css`. Contract: `apps/launcher/BRIDGE.md` (`update` message, `launchResult` codes 14 and 15) and `apps/launcher/version_gate.h`.

## What the host says -> what the popup shows

| Host | Popup | Dismissable |
|---|---|---|
| `launchResult` code 14, nothing known about an update yet (or `update.state` `checking`) | **pending**: "Updating Solar RP...", indeterminate gold sweep | no |
| `update` with `available` or `mandatory` and `state` `idle` / `checking` (about to start) | pending | no |
| `update.state` `downloading` + `progress` | **downloading**: percent figure, gold fill on DOWNLOAD (aria-valuenow = percent) | no |
| `downloading` without `progress` | downloading, indeterminate sweep, no figure | no |
| `verifying` | **verifying**: DOWNLOAD done, VERIFY sweeping | no |
| `ready`, `installing` | **installing**: INSTALL running; the launcher exits about 1.5 s later and restarts by itself | no |
| `deferred` | **waiting**: "Update ready", installs when GTA V closes | no |
| `state` `error` (or non-empty `error`) while an update is pending | **failed**: the host's error line, Try again (sends `checkUpdate`) | no |
| `launchResult` code 14 and the check finds nothing (`update` idle, `available` false) | **noUpdate**: "No update yet ... Try again in a minute." | yes (OK, Esc, click outside) |
| `launchResult` code 14 and the check failed, nothing pending | failed, Try again + Close | yes |
| `launchResult` code 15 | **serverBehind**: "This server has not been updated yet. Try again in a minute." | yes |
| `update` idle / checking / failed check, no refusal, nothing pending | nothing (a background poll never opens it) | - |

Reading of BRIDGE.md where it is ambiguous:

- Every available update is mandatory (`mandatory == available` on the host). The popup does not trust the flag alone: `available`, `mandatory`, or an installer in flight (`downloading`, `verifying`, `ready`, `installing`, `deferred`) all make it mandatory.
- `ready` and `installing` share one view (setup is about to start / has started).
- `deferred` (GTA V is running with Solar RP loaded) is also mandatory, so it is not dismissable; the rail's minimise and close-to-tray still work, and the game is unaffected.
- BRIDGE.md has no separate error event: a failure is an `update` message with `state: "error"` (or a non-empty `error`). Try again sends `checkUpdate`; the host restarts the pass ("failed and being retried" is what code 14 on PLAY does too).
- `ready` does not carry the update state, so the SPA sends `checkUpdate` once after `ready`, and again on code 14 (the host only starts its own check if the last one is older than a minute; `checkUpdate` is never rate-limited and re-sends where a running pass stands).
- Code 14 also covers "the server speaks a newer protocol" where no newer launcher is published yet; then the check finds nothing and the popup resolves from pending to "No update yet" (dismissable) instead of trapping the player.

## States to capture (browser, demo mode; 1240x800, then 1024x700)

Serve the build (`npm run build && npm run preview` -> http://127.0.0.1:5318/) or `npm run dev` (http://127.0.0.1:5317/). Add `signedIn=1` for the Play screen behind the veil (without it the Auth screen is behind). The page is dark only (no light theme).

| # | URL query | State |
|---|---|---|
| 1 | `?signedIn=1&update=pending` | pending (code 14, nothing known), sweep running |
| 2 | `?signedIn=1&update=downloading&progress=0.42` | downloading, 42% (try `0` and `1` too) |
| 3 | `?signedIn=1&update=downloading-unknown` | downloading, indeterminate |
| 4 | `?signedIn=1&update=verifying` | verifying |
| 5 | `?signedIn=1&update=installing` | installing / restarting |
| 6 | `?signedIn=1&update=deferred` | waiting for GTA V to close |
| 7 | `?signedIn=1&update=error` | failed; press Try again: it restarts the update (downloading, then verifying, installing) |
| 8 | `?signedIn=1&update=noupdate` | code 14, nothing newer found; OK / Esc / click outside dismiss it |
| 9 | `?signedIn=1&update=server-behind` | code 15 copy; OK / Esc / click outside dismiss it |
| 10 | `?signedIn=1&update=run` | the whole sequence, about 12 s: 0 to 100%, verifying, installing |

Also check: Esc, a click on the veil and Tab do not get out of states 1-7; the rail's minimise / maximise / close buttons still work over the veil; the sections and account are locked; reduced motion (the sweep and the entrance stop); Windows High Contrast (forced colours); 150% DPI. In the exe the same states are reachable only with a real host, or with `?demo=1` and a host that sends them; `update=` is read by the mock bridge only.
