# Development workflow

The project is developed in small stages, each ending in a working, tagged version you can go back to.

## Branches and tags

* **`main`** always works: it builds, the installer runs, the docs match.
* Work happens on short-lived branches named `feat/<what>`, `fix/<what>` or `docs/<what>`, and is merged into
  `main` with `git merge --no-ff` so each piece of work stays visible in the history as one unit.
* At the end of a roadmap stage `main` is tagged `vX.Y.Z` (annotated tag, with a one-line summary of what was verified).
* Commit messages: a short imperative summary line, then a paragraph on *why* when it isn't obvious.

## Rolling back

```bash
git log --oneline --decorate        # find the stage (tags) or commit
git checkout v0.1.0                 # look at / build an old stage (detached HEAD)
git revert <commit>                 # undo one change, keeping history
git switch -c fix/something v0.1.0  # branch from an old stage and continue from there
```

## Keeping the pieces in step

Four things must agree, so changes that touch one usually touch the others:

| If you change | Also update |
|---|---|
| An event code | `firmware/src/mood.h`, `bridge/claude_buddy_bridge.py` (`EVENT_CODES`), `docs/PROTOCOL.md`, `install.py` (`HOOKS`) |
| The BLE service | firmware, bridge, `docs/PROTOCOL.md`, and bump the protocol version once it exists |
| A config key | `install.py`, bridge, `hooks/emit.py`, `docs/SETUP.md` |
| Wiring or pins | the board overlay and `docs/HARDWARE.md` |

## Before merging

* Firmware: build every supported board target (`docs/FIRMWARE.md`) with no warnings from our code.
* Installer: run it in a throwaway home directory (set `HOME` / `USERPROFILE`), install twice, then uninstall.
* Bridge: run with `--no-ble` and send events through `hooks/emit.py`. Use `--simulate` to try the web page without a buddy.
* Docs: follow your own change as a newcomer would.

## What must never be committed

Hostnames, IP addresses, passwords or tokens, internal documents or files from your employer, and anything marked
confidential (including hardware schematics that are not yet public). `config.json` and compiled `.hex` files are in `.gitignore`.
