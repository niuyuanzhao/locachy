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

## Package contracts

Locally patched packages are built unprivileged in disposable roots against the
pinned repository snapshot. Their exact sealed artifacts are installed together
in another disposable root; trusted contracts run there, with no host display
or input devices. The proof records the complete artifact set, target packages,
source hash and required contract identifier. A mismatch stops the host update;
it is not bypassed by a successful compiler exit.

`tools/test-local-packages.py`, `test-update-transaction.py` and
`test-prepared-builder.py` check this orchestration with doubles. These are not
evidence that a new set of real package archives passed its contracts. Actual
runs and their limits are recorded separately in the repository's
`REGRESSIONS.md`.

## Adding package names

Host's Add field accepts names separated by spaces. Each is passed as its own
argument. The backend validates the entire request and looks up local recipes,
repositories and then the AUR before changing the manifest. Unknown names and
lookup failures leave the manifest untouched. Existing lines containing several
names are read as separate packages; removing one keeps the others.

Adding or removing leaves still runs reconciliation, not only a one-package
pacman command. A failed operation retains its output in
`/run/locachy/packages.log`; a failed prepared update retains its plan and gate
logs under `/var/lib/locachy/local-packages/transactions/`. Do not delete those
logs or skip their contracts to get past a failure.
