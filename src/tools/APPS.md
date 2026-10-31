# App presets

[中文](APPS.zh.md)

The presets offered for containers (Edge, Firefox, WeChat and the others) live
in a repository of their own, `locachy-apps`, not in this source. This
repository keeps only the host programs' presets (`src/containers/`:
`foot.conf`, `code.conf`, `host="1"`): their packages are the host's manifest
and `code.conf` runs a system wrapper. An app preset may never be a host
program; `host=` in the apps repository rejects its catalog.

One helper reads and installs the catalog: `src/link/locachy_apps.py`
(installed by `install-pkg-tools` into `/usr/local/lib/locachy/`). The
updater loads it; `make.sh`, the live entry and `make-installer.sh` run it as
`python3 -I locachy_apps.py validate|catalog|bootstrap ...`.

## The repository (format 1)

```
FORMAT          one line: 1
apps/NAME.conf  the presets
README.md, README.zh.md, LICENSE
tools/check.py  checks it with a system's own helper:
                python3 -I tools/check.py PATH/TO/src/link/locachy_apps.py
```

Its files are data. They are parsed and validated, never sourced or run.
The first invalid file rejects the whole catalog, and the installed catalog
stays as it is:

- `FORMAT` must be a format this system reads (`1`); otherwise
  `not compatible: format N`.
- `NAME` is `[a-z0-9][a-z0-9._-]{0,62}`. Each preset is a regular file (not a
  link), 16 KiB at most, and UTF-8. `apps/` holds nothing else and at least
  one preset.
- A line is empty, a `#` comment or `KEY="VALUE"`. Keys are those of
  `locachy-app` except `host`: `package`, `extras`, `command`, `arguments`,
  `network`, `camera`, `directories`, `desktop`, `x11`. An unknown key, a key
  given twice, `host=` or any other line rejects the catalog.
- A value has no `"`, `\`, `$`, backquote or control character. `package` is
  one pacman package name; `extras` are package names separated by spaces;
  `command` is an absolute path without `..` or spaces; `network`, `camera`
  and `x11` are `0` or `1`; `directories` are absolute paths without `..`
  separated by colons; `desktop` are `*.desktop` names. `package` and
  `command` are required.

## On the installed system

- `/var/lib/locachy/apps/catalog/`: `NAME.conf` and `CATALOG`
  (`format`, `origin` = `repository`, `medium` or `bootstrap`, `repo`,
  `branch`, `commit`, `installed`, and for a bootstrap catalog
  `system_commit`).
- `/var/lib/locachy/apps/repo.git`: the shallow fetches.
- `/var/lib/locachy/apps/baseline/NAME.conf`: the preset each configuration
  last followed.
- `rc.conf`: `apps_repo` (absent: `https://github.com/niuyuanzhao/locachy-apps.git`
  on the host; `""`, `0` or `off`: no app preset updates) and `apps_branch`
  (absent: `main`). `LOCACHY_APPS_REPO` and `LOCACHY_APPS_BRANCH` override them,
  as `LOCACHY_UPDATE_REPO` and `LOCACHY_UPDATE_BRANCH` do for the source.
- Signatures: when `/etc/locachy/trusted-signers` is not empty, the catalog's
  commit needs a good SSH signature of one of its keys, exactly as the
  source's commits do. No key is imported automatically.

`locachy-pkg search`, the Packages form and the installer read the same
catalog (`LOCACHY_APPS_CATALOG` overrides where it is, for tests and the
live installer). Search still skips any `host="1"` preset there.

## Updates

Check fetches `apps_branch` of `apps_repo` into `repo.git`, verifies and
validates it, and compares its commit with the installed catalog. Its plan
rows (target `apps`):

- `repo locachy-apps OLD NEW_LABEL` and
  `catalog "app presets from branch BRANCH" OLD COMMIT` when they differ;
- `merge NAME "KEY KEY ..."` for each configuration that will follow its new
  preset, and `NAME rebuild "its preset names other packages" OLD NEW` when
  that changes the container's `package` or `extras`, so that the same Apply
  builds it again;
- `note "not checked: WHY"` when it cannot be fetched, verified or
  validated, and `note "bootstrap catalog from the system source, not from
  the apps repository"` while the catalog is the bootstrap one. With app
  preset updates off there are no `apps` rows at all.

Apply goes in this order: the source first (as before), then `apps`, `host`,
`locachy` and the containers. `apps` installs the commit Check pinned, from
`repo.git`, without fetching again: a remote that moved on after Check does
not change what is applied. A re-plan that does not check (after the source
is installed) reuses that pinned commit if `repo.git` still has it;
otherwise the apps part waits for the next Check. A failed `apps` says
`===> apps: not done` and stops nothing else; the containers it would have
rebuilt for a merge are left for later.

An apps-only Apply never runs `make.sh`, `locachy-boot`, pacman or
`locachy-build`, and never writes the source-unbuilt marker. A system
update never removes or overwrites the catalog.

The new catalog is written next to the old one and the two change places
in one step (`renameat2` `RENAME_EXCHANGE`). An interruption leaves either
the old or the new catalog, complete; the next switch removes what was
left aside.

### Merge

For each `/etc/locachy/containers/NAME.conf` that is not a host program and
whose `NAME` is in the new catalog, the base is `baseline/NAME.conf`, else the
old catalog's `NAME.conf`; without either it is a configuration the user
made and is never touched. For each key of the base or the new preset, when
the configuration still has the base's value and the new preset's differs,
the configuration takes the new value (or loses the key when the new preset
has none). Comments, other keys and values the user changed stay. The file
is replaced in one step, then its baseline becomes the new preset. A preset
removed from the catalog leaves its configuration as it is. Apply prints
each change.

## The first update after the split

`make.sh`, before it replaces `/usr/local/share/locachy/containers` (now with
the host programs' presets only), runs `locachy_apps.py bootstrap` on it.
When there is no catalog yet, the app presets the old system installed
there become one (`origin="bootstrap"`, no commit, `system_commit` from
`/usr/local/lib/locachy/source-commit`). An existing catalog is never
touched. That first Apply runs the old updater's Check; the new updater only
re-plans after the source, without fetching app presets. The Check at the
end of that Apply, and every later one, fetches them; until the first
catalog from the apps repository is installed, the bootstrap note says so.

## Installation

- The live entry (`installer/locachy-install`) fetches the apps repository
  the way it fetches the source: `--apps-repo` and `--apps-branch`, else
  those of the installation being continued, else `apps_repo` and
  `apps_branch` of `/etc/locachy/source-origin.json`. The fetched source's
  own helper makes the catalog. When that fails, the catalog fetched before
  from there is used, else the medium's own
  (`/usr/local/share/locachy/apps-catalog`). It exports
  `LOCACHY_APPS_CATALOG` and `LOCACHY_APPS` (`fetched`, `fetched earlier`,
  `fetched before: WHY`, `bundled: WHY`).
- `locachy-install` offers the host programs (`SOURCE/containers`, host only)
  and the catalog's presets. `--describe-json` has
  `"apps": {"text", "not_newest"}` and `defaults.apps_repo`/`apps_branch`.
  An app it does not have stops the installation before any disk is
  touched (`Unknown app: NAME`); there is no generic preset for unknown
  names. The whole catalog goes into the target's
  `/var/lib/locachy/apps/catalog`, the chosen presets into
  `/etc/locachy/containers`, and `apps_repo` (and `apps_branch` when not
  `main`) into `rc.conf`. The resume record keeps the catalog's commit; a
  continued installation uses the catalog its target has already.
- `make-installer.sh` needs `LOCACHY_APPS_SRC`, a checkout of the apps
  repository. A missing or invalid one stops the build with the reason,
  before anything is made; the medium never has an empty catalog.
  `LOCACHY_APPS_REPO`, `LOCACHY_APPS_BRANCH` and `LOCACHY_APPS_COMMIT` (else
  the checkout's git) are recorded in `source-origin.json` and the catalog.
  `.github/workflows/release-iso.yml` checks out
  `niuyuanzhao/locachy-apps` for it.

## Tests

`tools/test-apps-catalog.py` (in `test-interaction-regressions.sh`) uses
local git repositories as the apps repository. It covers every validation
rule, the incompatible format and the fetch failure that keep the old
catalog, an interrupted switch, a remote that moves on after Check, required
signatures, the merge rules, the rebuild row, an apps-only Apply that builds
nothing, the bootstrap, and search. `test-install.py` and
`test-installer-profile.py` cover the installer, the live entry and the
medium's catalog.
