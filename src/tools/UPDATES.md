# Package and source updates

[中文](UPDATES.zh.md)

`packages` shows the plan before applying it. `locachy-pkg plan` and
`locachy-pkg apply [TARGET...]` are the same backend used by that interface.
Checking does not install repository packages or execute a downloaded recipe on
the host.

## Source first, then the matching host

When LOCachy's source update is selected, Apply installs that source and its
package tools first. The updated updater checks again before changing host
packages or rebuilding containers. Recipes and contracts therefore come from
one installed source tree, rather than old recipes with new tests. Host-only
Apply keeps the installed source and its tests.

A source installed before a failed build remains marked as not built. A later
Check must keep offering its program rebuild; matching host package versions
alone do not mean its new programs were built. If package-tool installation or
package declaration failed, Apply retries that source setup before planning the
host. The second phase stays on the selected source; a newer commit discovered
at the end is offered in the next plan, not silently installed mid-update.

`/usr/local/lib/locachy/source-commit` is the source applied: the commit whose
update has all been done. Fetching a commit, or Apply putting its tree in
place, does not change it. Until a plan has nothing of that update left (its
local packages built, its declared packages installed, its programs built),
the commit is pending in `/usr/local/lib/locachy/source-pending`, and the
plan carries a note naming both. An update that stopped (a build waiting for
source signing keys, say) therefore leaves the earlier commit as the applied
one; the Apply that finishes it records the new one. `packages` and the
About page of `settings` show the applied commit, `packages` the pending one
too. When the commit fetched is the applied one and nothing of it is pending,
Check offers no source update: an installation puts only part of that commit
in `/usr/local/src/locachy` and adds its chosen modules to the packages and
rc.conf there, so the tree is not compared with it.

`declare-packages` records the base and optional-module package definitions.
Source additions for an enabled module enter `/etc/locachy/packages`
automatically. Explicit removals stay removed. A missing record starts from the
shipped 08d2a66 definitions, not from an assumption that every optional module
is enabled. A module is enabled here when its first package is declared. The
script only declares packages; it does not install them.

Missing declared repository packages are pinned with the host transaction and
installed in each candidate's disposable build/test root as well as the host's
final transaction. A newly required library must not be supplied manually just
to make the new tests compile. AUR and local builds retain their separate,
explicit failure paths.

## AUR source signing keys

AUR packages are built after the host's pacman transaction. Before Apply changes
the app presets, the repository databases or the packages, it looks up the source
signing keys that those builds will check against, using
`locachy-build --check-keys`. When the same Apply also installs a LOCachy source
commit, that source is installed first, as always, and the updated updater looks
the keys up before any host target; a refusal then leaves that source in place
and changes nothing else. That command reads each recipe's
`validpgpkeys` from the AUR and looks the keys up in the build user's keyring.
It builds nothing and does not fetch, import or trust any key. If any key is not
trusted yet, the whole Apply is refused with status 3 (the job's `keys` state).
For each key it shows the package, the fingerprint, the recipe that declares it
(with its `.SRCINFO` link), the recipe's signature sources, and the upstream project
to check it with. It also shows the command to run as root once the key has
been checked: `locachy-build --trust-key FINGERPRINT`. Nothing was changed, so
Apply can simply run again after the keys are trusted. This refusal is reported
as `refused before any change`, never as `not done`. `not done` means a target
failed after it had begun. Only a person adds a key, and no option or variable
skips the lookup. The build itself still stops for an untrusted key as a
second line of defence.

## App presets

The app presets come from their own repository (`apps_repo`, `apps_branch`),
not from the source. Check pins their catalog's commit as the `apps` target;
Apply installs it after the source and before the host, merges the
configurations that follow a preset, and never builds or installs packages
for it. A failure there stops nothing else. See [APPS.md](APPS.md).

## If the running updater cannot check

The `c` / Check action runs the current updater first. If it reports that it cannot fetch updates, the message must include pacman's actual repository, HTTP, signature, database, or filesystem reason; `checkupdates`'s generic `Cannot fetch updates` is not enough. A failed check changes no host package database, published repository database, package, or successful plan. The full job log is available in the output pane.

If an older installed updater is the thing that hides the reason, repair it in the managed administrator terminal without reinstalling or rebooting:

1. Obtain the candidate `locachy-pkg` file from the exact reviewed LOCachy source and save it as a normal file. Do not pipe a download into a shell. Verify the complete SHA-256 supplied with that source:

   ```sh
   sha256sum locachy-pkg
   ```

2. Run the repository's `tools/repair-updater.py` with the candidate path and the complete lowercase SHA-256. The helper parses the candidate without executing it, refuses a symlink or writable target, takes `/run/locachy/pkg.lock` without waiting, refuses a running pacman database, saves the old file as a hash-named hard link, and atomically installs the checked file. It performs no package transaction and does not restart a service.

   ```sh
   python3 -I tools/repair-updater.py ./locachy-pkg FULL_SHA256
   ```

3. Press `c` again. The next Check is a normal `pkg-plan` job and uses the repaired updater. If it still fails, the displayed pacman error is the reason to fix; no network failure should be guessed from it.

An accepted package job is not killed by the repair helper. If the package-domain lock is busy, wait for the job shown in Home/Packages and run the repair again. The old updater remains in place on every failed verification or interrupted publication.

This is an administrator recovery path for a stale updater, not a second update mechanism. Once the normal source-first update completes, `install-pkg-tools` installs the updater from that committed source in the ordinary job.

## Repository stack

LOCachy reads its repositories as one stack, most optimised first: CachyOS's
builds for this CPU and each level below it, then ALHP's, then `[cachyos]`, then
Arch's (`locachy-repos` writes the order). Of a package, the newest version wins,
whichever layer carries it. Only an exact version tie goes to the higher layer.
A rebuild of the same release carries a higher `pkgrel` suffix (`3.6.5-1.1` is
newer than `3.6.5-1`), so a layer that has rebuilt a release wins over the old
build below it.

pacman by itself takes each package from the first repository that has it, however
old that build is. A layer that has not rebuilt a new release yet keeps its old
version, and the sonames it links, in front of the newer build below. That is how
an optimised openssl and a tor built against it could be held back while Arch's
newer openssl was offered: the update then broke.

`locachy_stack.py` applies the rule. It is used by:

- Check and the plan. A bounded `pacman -Sy` with an explicit private DBPath
  fetches the databases into `/var/lib/locachy/checkupdates`, each time whole:
  they are dated at the epoch
  first. pacman's refresh keeps a database that is newer than the mirror's copy,
  and fetches the signature from the mirror it asks. When a stalled mirror fails
  over to an older one, that older signature would be checked against the kept
  database, and the Check would fail until a newer copy turned up. The stack's
  view of them is `/var/lib/locachy/checkupdates/merged`. The updates, the
  information shown and the search all read that view. ALHP's mirrors are ranked
  mostly by speed (Mirror ranking, below), so an older mirror can still supply
  the databases; the update then follows that mirror's data.
- Apply on the host. The merged databases replace `/var/lib/pacman/sync` under
  pacman's own lock and are recorded in `/var/lib/locachy/sync-published.json`.
  Then `pacman -Su` runs on them. LOCachy never runs `pacman -Syu`: a raw refresh
  would take the packages back out of the stack.
- Fresh roots (`make-root.sh`). The databases are fetched aside and merged into
  the new system, which records them as its published set and installs with
  `pacman -S`.
- The repository checks of `locachy-repos`, before a section is turned on or off.

The merged databases are rewritten, so they carry no signature of their own. Their
packages are still signature-checked: `SigLevel` is `Required DatabaseOptional`,
which accepts the unsigned database, and every package keeps its own signature.

A rewritten database is dated at the epoch. pacman's refresh downloads a database
only when the mirror's copy is newer than the local file, and still fetches a
signature the file lacks. Dated when it was merged, a rewritten database could
look newer than the mirror's: pacman kept it, paired it with the mirror's
signature, which it never matches, and every refresh failed. Dated at the epoch,
it is fetched again whole, with its signature, by a plain `pacman -Sy`, and the
guard below then sees that it was replaced.

`locachy-pacman-guard` refuses a pacman transaction typed by hand when the host's
databases are no longer the ones LOCachy published, for example after a plain
`pacman -Sy`, and it changes nothing. Run `locachy-pkg plan`, then
`locachy-pkg apply host`, to publish the stack again. A system that has not had a
LOCachy update since the record was introduced has no record yet, and nothing is
refused for that reason until its first update writes one.

## Mirror ranking

`locachy-repos rank` ranks the mirror lists in `/etc/pacman.d` from this
machine: Arch's with reflector, CachyOS's and ALHP's with rate-mirrors. It runs
as a system job and goes through the network layers that are on. A list is
replaced only when its ranking gave mirrors. A list that an earlier ranking
wrote also stays when the new ranking gave fewer than half as many mirrors: one
test round in which most mirrors timed out once cut CachyOS's list from 16
mirrors to 1. The job log then says so, and the ranking still counts as
complete. A list that came with a package is replaced by any ranking that gives
mirrors. Settings ranks them by hand under
Software Sources, on the Mirrors line (Enter). That line also shows when the
last complete ranking ran: the one that gave every list mirrors, recorded in
`/etc/pacman.d/.locachy-ranked`.

What a list holds: CachyOS's mirror API lists every CachyOS mirror with its
health and sync delay. Every mirror it lists as https, healthy, not out of date
and at most 10000 s behind (the RATE_MIRRORS_MAX_DELAY that cachyos-rate-mirrors
uses) is timed, and the list holds every one that answered (27 such mirrors on
2026-11-11). rate-mirrors' own cachyos mode times only two mirrors per country
near this machine, so its list was as long as the few it happened to reach: 16
in one test, 1 a few minutes later. Without an answer from the API, the mirrors
of the present list are timed. ALHP's list holds every known ALHP mirror that
answered.

ALHP's and CachyOS's order follows the user's rule. It only orders; every mirror
that answered stays:

- The base order is speed, the fastest first.
- Among the mirrors at least half as fast as the fastest, the newest data comes
  first. ALHP's date is the Last-Modified of the database that was timed, read
  with a one-byte GET (a mirror may redirect to storage that refuses HEAD).
  CachyOS's is the sync delay its API reported, so no request is added. A mirror
  with a known date comes before one without; of two with the same date, or
  with none, the faster one comes first. So the fastest mirror is not always
  first: a mirror with newer data than its own, or with a date when it has
  none, comes before it.
- The other mirrors follow by speed.

The mirrors are also ranked again periodically:

- Who runs it. Process 1 starts the service `service_repos_rank`
  (`locachy-repos rank-timer`). It sleeps until the next ranking is due. Then it
  submits the fixed system job `repos-rank-due` (`locachy-repos rank --due`) and
  waits for it. The job checks again whether a ranking is due before it ranks.
- Interval. The interval is rc.conf's `repos_rank_days`, in days. Fractions are
  allowed: `0.5` means every 12 hours. Install writes `repos_rank_days="1"`
  (once a day) into rc.conf, and every update adds it where it is missing. 0 or
  less means only by hand; a value that is not a number counts as 1; more than
  3650 counts as 3650. In settings it is the line "Rank
  mirrors automatically" under Software Sources. Its choices are only by hand,
  or every 1, 2, 3, 5, 7 or 14 days. The line also shows the next ranking, which
  is the `next` line of `locachy-repos status`: a local time, `now`, or `off`.
- When it is due. The interval counts from the last complete ranking, whoever
  ran it, so a ranking by hand moves the next one later. Without a record, or
  when the last ranking left a list without mirrors, a ranking is due at once.
- Failure. A ranking that failed is tried again one hour later, not sooner.
  That covers a failed or cancelled job, and a ranking that left a list without
  mirrors.
- Changes. The service reads rc.conf and the record every time it wakes. It
  wakes when either changes, through inotify on `/etc/locachy` and
  `/etc/pacman.d`, and at the latest once a day. A new interval therefore needs
  no restart. Its timer runs on the wall clock (`CLOCK_REALTIME`), so a ranking
  that fell due during suspend or hibernation runs at resume.
- Turning it off. Set `repos_rank_days="0"`, or choose "only by hand" in
  settings. The service keeps running and ranks nothing. Do not delete the
  `service_repos_rank` line to turn ranking off: the next update writes it back.

## Package contracts

A prepared update is built and tested against one pinned host snapshot.
`locachy-build --validate-prepared DIR` installs the sealed candidates together
in a disposable target root and runs the trusted update smoke checks before
any host package transaction. The proof binds the complete artifact set,
target packages, source hash and required contract ID. Failed checks, missing
proofs and mismatches stop the update before the host package transaction.

| Package | Retained in update smoke | Full checks run separately |
| --- | --- | --- |
| `labwc-locachy` | Installed headless startup, Wayland and clipboard-guard sockets | Target host-client rebuild; clipboard, restricted clipboard, guard policy and cgroup freezer; inner-edge snapping, bar-corner unit and corner snapping tests, and drop gate |
| `libmupdf` | Standalone Markdown heading anchors without system `cmark-gfm` | PDF, EPUB and `view-render` |
| `libpaper` | Repeated init/done, catalog cleanup and XDG ownership | ASan/UBSan with leak detection |
| `libinput` | Raised epoch, `libinput.so` provides, Panasonic circular-pad quirks, link and context creation | Patched upstream circular-scroll litest and pointer VM tests remain separate release checks |
| `libinput-tools` | CLI commands and helpers, no debug GUI, ELF/runtime dependencies and matching `libinput` release | No separate suite |

For development only, run `locachy-build --regress-prepared DIR SHA256`
against the same sealed candidates, normally on a disposable development host
or root before Apply. Pass the trusted SHA-256 of the fixed `plan.json` emitted
by the transaction snapshot/Check lock. Do not hash an untrusted plan after
the fact to authorize it. The command verifies the pinned inputs and the
original installed-package fingerprint before running any tests.

The full labwc gate creates a dedicated cgroup subtree and namespace and fails
without writable delegated cgroup v2. Logs go to `DIR/regression.*`; key-trust
notifications, update proofs and host packages are unchanged. The two libinput
packages use the same installed checks in both modes.

Each independent package build has its own disposable root, package database,
build home, keyring and writable cache. CPU affinity and quota, plus available
memory and disk space at runtime, determine how many roots can run at once.
`LOCACHY_BUILD_WORKERS` optionally caps that count.

GNU Make and Ninja 1.13+ share CPU slots through a GNU Make FIFO jobserver.
Each active root has one implicit slot; every additional job claims a slot
from the shared pool. Workers are not pinned to a static CPU set, and explicit
upstream `-j1` is preserved. Compression and tests that do not use the jobserver
are limited per worker.

Unsupported build tools or dependencies that require AUR use the original
serial phase. Compilation and signature failures never trigger fallback.
The per-package logs include actual `makepkg` start and end times. Final
validation still installs all candidates together in a fresh disposable root
before the single host transaction.

The builder reuses identical file-manager objects within its trusted build
check, and logs separate preparation, package, contract and host-transaction
stages. This exposes the longest path instead of attributing the whole update
to the last package named in the output. It does not skip a contract or a
post-transaction rebuild.

The output pane keeps the actual stage timings and any failed stage. The
prepared update retains its plan and gate logs under
`/var/lib/locachy/local-packages/transactions/`; a failed batch can be resumed
there without silently reusing an unverified artifact.

`tools/test-local-packages.py`, `test-update-transaction.py` and
`test-prepared-builder.py` check this orchestration with doubles.

## Adding package names

Host's Add field accepts names separated by spaces. Each is passed as its own
argument. The backend validates the entire request and looks up local recipes,
repositories and then the AUR before changing the manifest. Unknown names and
lookup failures leave the manifest untouched. Existing lines containing several
names are read as separate packages; removing one keeps the others.

Adding or removing leaves still runs reconciliation, not only a one-package
pacman command. A failed operation retains its output in
its system job's log, `/run/locachy/jobs/ID.log` (home lists the jobs,
[JOBS.md](JOBS.md)); a failed prepared update retains its plan and gate
logs under `/var/lib/locachy/local-packages/transactions/`.
