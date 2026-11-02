# VPS addendum (installed by the Solar RP VPS kit as rules/00-vps-linux.md)

This machine is the Solar RP VPS: Ubuntu/Debian x86-64 Linux, root, no desktop, no GTA, no Windows. Your global CLAUDE.md and rules/*.md were written on GI's Windows 11 PC (Git Bash). Read them with these corrections.

## Does not apply here (PC only)

- Every item under "Tooling and shell (Windows + Git Bash)" in lessons-core.md: MSYS path mangling and `MSYS_NO_PATHCONV`, `tar --force-local` for `H:` paths, `cygpath`, Git Bash `$!` PIDs, `taskkill`, PowerShell, `.cmd` shims, `ADMINI~1` short paths, the Windows node v20/v22 split, CRLF checks of files edited on the PC (the VPS scripts still refuse CRLF in the tree).
- Windows paths (`H:\`, `C:\`, `E:\`), `%LOCALAPPDATA%`, the registry, `build/win`, WinLibs, `.toolchain/winlibs-*`, `SolarLauncher.exe` runs, and anything that launches GTA.
- "Never build over a live dev server" lessons that name `next dev` on the PC: there is no dev server here; the site is built by `deploy-site.sh`.

## Still applies here

- Read before editing, small verifiable steps, evidence before claims, read the FIRST failure line, never `| tail` a failure, ask one sharp question when it matters, terse reports, address GI as "GI".
- No inline python/node through heredocs or `-c`/`-e` for anything with quotes, backslashes or non-ASCII: write the script into `/opt/gage/tmp/<task>/` and run it by path. (Plain bash is fine here.)
- Never kill by search result (no `pkill`, `killall`, `kill $(pgrep ...)`); end only a process you started, through its own PID. Services go through `systemctl` or the deploy scripts.
- Never print, copy or log the update signing key or `GAGE_ADMIN_TOKEN`; check existence with `test -s` or `stat -c %a`.
- "Never without asking": commit or push (there is no git here), add dependencies, delete files or data, broad refactors.

## Where the rules for THIS machine are

1. `/opt/gage/CLAUDE.md` (installed from `tools/deploy/VPS-CLAUDE.md`): hard rules, layout, the script table. It wins over the repository's own CLAUDE.md (which describes the PC build) and over this addendum.
2. `/opt/gage/src/docs/OPS.md`: the order to run the scripts in. Every script takes `--dry-run`: run that first.
3. Start sessions in `/opt/gage/src` so both CLAUDE.md files load. The source tree there is REPLACED on every `unpack-source.sh`; never edit it on this machine (changes are lost and never reach the PC).

## Tools

- Node 22 comes from `bootstrap-vps.sh`. The tools in `~/.claude/tools` (`scroll-capture.mjs`, `asset-vet.mjs`) need Playwright and a browser, which the kit does not install: ask GI before installing anything. Visual checks of the website therefore need his yes first (the quality bar in CLAUDE.md still stands; say "not verified: no browser on the VPS" instead of skipping it silently).
- `claude plugin` installs (impeccable, claude-mem, frontend-design) are done by the kit installer or printed by it; do not re-add them on your own.
