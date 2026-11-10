# System jobs (locachy-jobs)

Long root operations — package transactions, kernel and repository changes,
the boot image, container builds, firmware — run as **system jobs**: one
service owns a single queue and runs them **one at a time, in the order they
were asked for**, apart from the programs that asked. Closing, quitting,
freezing or stopping a program never touches its jobs, and two programs can
no longer run pacman (or pacman and locachy-boot, or two flashrom runs) at
once.

## The service

`/usr/local/lib/locachy/locachy-jobs` (`progs/locachy-jobs.c`) is a service
of rc.conf, `service_jobs` (make.sh adds it to an older rc.conf). Process 1
starts it like any other service, and again 5 s after it ends; process 1 has
nothing else of it.

- It runs as root in a cgroup of its own, `/sys/fs/cgroup/locachy-jobs/service`.
- Each job runs in `/sys/fs/cgroup/locachy-jobs/job-ID`, in a session of its
  own: a *runner* (a child of the service) enters that cgroup, then runs the
  job's steps with their output in the job's log. When the job has ended,
  what it left in its cgroup ends (`cgroup.kill`) and the cgroup goes.
- The service keeps its own files to root (umask 077); each step runs with
  umask 022, as a root shell does: what a step writes (pacman's
  configuration and mirror lists, rc.conf, the manifest) others read too,
  pacman as the build user among them.
- None of these is a program's cgroup (`pN`) or a container's (`app_NAME`):
  process 1 freezes and kills those, never these. A job is never frozen when
  the screen changes hands and never ends because a window closed.

## Kinds

A fixed table; each kind runs a fixed program of the system with fixed
arguments before the job's own, through `execv` — no shell. The service
checks every argument; anything else is refused.

| Kind | Command | Arguments |
|---|---|---|
| `pkg-install`, `pkg-remove` | `locachy-pkg install` / `remove` | 1–32 package names: letters, digits, `@ . _ + -`, not starting with `-` or `.` |
| `pkg-swap` | `locachy-pkg remove OLD`, then `locachy-pkg install NEW` | 2 package names |
| `pkg-sync`, `pkg-plan` | `locachy-pkg sync` / `plan` | none |
| `pkg-apply` | `locachy-pkg apply` | up to 63 target names (container names) |
| `kernel-select` | `locachy-pkg kernel select` | a package `linux…` of `[a-z0-9+._-]` |
| `kernel-headers` | `locachy-pkg kernel headers` | `on` or `off` |
| `repos-rank` | `locachy-repos rank` | none |
| `repos-set` | `locachy-repos apply --set` | `core`, `extra` or `multilib`, `=on` or `=off` |
| `boot` | `locachy-boot` | none, `--luks`, or `--register-only` |
| `locachy-rebuild` | `locachy-pkg rebuild` | none or `--force` |
| `tpm-pending` | `locachy-tpm enroll --pending` | none |
| `tpm-enroll` | `locachy-tpm enroll --passphrase-stdin` | optional PCR list and `/dev` devices; passphrase is an anonymous RAM pipe |
| `tpm-prepare` | `locachy-tpm prepare --passphrase-stdin` | no arguments; passphrase is an anonymous RAM pipe |
| `tpm-clear` | `locachy-tpm clear` | `/dev` devices |
| `secure-boot` | `locachy-tpm secure-boot` | `--enroll`, optionally `--microsoft` and passphrase over an anonymous RAM pipe |
| `app-build` | `locachy-app build` | a container name whose `host` is not `"1"`, checked at acceptance (not in the package domain) |
| `app-host-build` | `locachy-app build` | a container name whose `host` is `"1"`, checked at acceptance (in the package domain: its packages go on the host) |
| `app-rollback` | `locachy-app rollback` | a container name (as locachy-app's) |
| `app-remove` | `locachy-app remove` | a container name, optionally `--data` |
| `fw-refresh`, `fw-update` | `locachy-firmware fwupd-action refresh` / `update` | none |
| `fw-verify`, `fw-unlock` | `locachy-firmware fwupd-action verify` / `unlock` | an fwupd device id |
| `fw-local-install` | `locachy-firmware fwupd-action local-install` | an absolute path of a regular file |
| `fw-install-release` | `locachy-firmware fwupd-action install-release` | a device id; an `http(s)://` address or `install`, `upgrade`, `downgrade`, `reinstall`, `update` |
| `spi-probe` | `locachy-firmware spi-probe` | a flashrom programmer; `--chip NAME` |
| `spi-backup`, `spi-verify` | `locachy-firmware spi-backup` / `spi-verify` | a programmer and a folder (backup) or an image file; `--chip`, `--regions fd,bios,…` |
| `spi-write` | `locachy-firmware spi-write` | as `spi-verify`, and `--expected-size BYTES` |
| `bios-set` | `locachy-firmware bios-set` | an attribute (a letter or digit, then letters, digits, `_ . : -`; 96 bytes at most) and a value (0–256 bytes, no control character; a leading `-` is text) |
| `security-policy` | `locachy-security` | `set login-skip`/`duress`/`fingerprint`/`face` `on`/`off`, `set lock-mode low`/`high`, `remove-duress`, or `refresh` |

**The package domain.** The package, kernel and repository kinds, and every
kind that builds the boot image (`boot`, `locachy-rebuild`, `tpm-*`,
`secure-boot`, `security-policy`), hold `/run/locachy/pkg.lock` as
`locachy-pkg` does: the job's runner takes it (`flock`, waiting: an outside
pacman, whose guard holds it, finishes first) inside the job's cgroup before
the first step, and every step inherits that descriptor, so the tools find the
domain held and never open it again. Their own locks come inside it (domain,
then security, then configuration); the tools never take `pkg.lock`
themselves. The firmware kinds do not hold it (`locachy-firmware` holds
`firmware.lock`), nor do the container kinds (`app-build`: an app whose build
installs nothing on the host). `app-host-build` holds it: an app with
`host="1"` installs its packages on the host, a host package transaction. The
kind is the app's own host setting, read and checked by the service at
acceptance (a mismatch is refused: "the app's host setting changed; open it
again"); no client argument decides it. The path is fixed: no runtime
setting changes it, and the runner removes `LOCACHY_PKG_LOCK` from every
step's environment (a test build moves it at compile time only).

**Images.** A kind whose argument is an image file (`fw-local-install`,
`spi-verify`, `spi-write`) has that file copied when the job is accepted, not
when it runs: opened without following a link, a regular file of 64 MiB at
most, into `/run/locachy/jobs/ID.input` (root's, 0600), its size and SHA-256
kept in `ID.state`; the job's argument then names the copy. A copy that cannot
be made refuses the request. Before the first step the runner checks the copy
against that size and digest, and a job whose copy changed does not run. The
copy goes when the job ends or is removed from the queue. What runs is what
was named at acceptance, whatever becomes of the person's file afterwards.

Every accepted request is a separate queue event, including repeated requests with the same kind and arguments. Interactive TPM input is passed once through a complete anonymous pipe with its writer closed; it is never an argument, environment value, state field, or log. The service does not persist that descriptor: if it restarts before the job runs, that job is marked failed and must be submitted again.

`locachy-boot` and all mutating `locachy-tpm` commands route through this queue on the installed running system. In a verified `/locachy-jobs/job-ID` cgroup, they execute inline, including nested boot rebuilds and pacman hooks; no nested enqueue/wait deadlock. A chroot or a host not running LOCachy's PID 1 is explicitly reported as offline and builds inline. A missing service on the installed running system is an error, never a fallback to direct execution.

An external pacman PostTransaction hook registers its boot event without waiting (pacman still holds its database lock); its result is in home/jobs, not pacman's exit status. Pacman launched inside a job runs the hook inline and propagates failures normally. External root CLI/package transactions themselves are not retroactively managed or made atomic by this integration. The existing advisory locks remain additional defense, not queue substitutes. Secure Boot key enrollment remains an explicit person's choice; no new automatic key registration is added.

## States

`queued` → `running` → `done` (status 0), `failed` (its status), or
`keys`: a build (a job of locachy-pkg or locachy-app) that stopped for
source signing keys a person has not trusted yet — locachy-build exits 3,
or lists them in `/var/lib/locachy/builder/untrusted-keys` during the job
(locachy-pkg then only says it failed). Not a failure: the program that asked
shows the keys and asks for the rest once they are trusted (packages: `g`).
A running job being stopped is `stopping`, then `cancelled`; a queued job
removed is `cancelled` at once. Every state but `queued`, `running` and
`stopping` is an end, and the next job starts.

## Files

`/run/locachy/jobs/` (0700, root's), each file 0600:

- `ID.state` — `kind=`, `state=`, `status=`, `from=` (the program that asked,
  by its name), `runner=`, `submitted=`, `started=`, `ended=` (seconds since
  1970), an `arg=` line per argument, `last=` (its output's last line, once
  it has ended).
- `ID.log` — its output: a first line `locachy-jobs: job ID: KIND ARG... (from
  PROGRAM)`, then what its programs wrote.
- `ID.exit` — written by the runner: how it ended; `ID.run` — locked by the
  runner while it lives. Both go when the service has taken the job's end.

The 32 newest ended jobs are kept, with their files. `/run` is a tmpfs: the
queue does not outlive a reboot.

## The control socket

`/run/locachy/jobs/control`: `SOCK_SEQPACKET`, mode 0600 in a folder of
mode 0700, so only root reaches it. It is never bound into a container
(`link/locachy-app` gives a container a `/run` of its own; a host program of
locachy-app runs as the user). A peer is checked with `SO_PEERCRED` (uid 0)
and its cgroup (`/proc/PID/cgroup`): a container's process (`app_NAME`) is
refused, a native window (`app_window-PROGRAM-N`, root) and a direct program
(`pN`) are not. Direct programs and windows connect to it alike; nothing is
forwarded through process 1 or the seat.

One request a message, one reply a message, fields separated by NUL:

| Request | Reply |
|---|---|
| `submit KIND ARG...` | `ok ID POSITION` (0: it runs now; N: N jobs before it), or `error WHY` |
| `list` | `ok ROW...`: every job kept, oldest first |
| `status ID` | `ok ROW` |
| `log ID` | `ok TEXT`: the end of its output (8 KiB at most, from a line's start) |
| `cancel ID` | `ok removed` (it was queued), `ok ended`, or `confirm WHY` (it runs) |
| `cancel ID confirm` | `ok stopping` |
| `drain EPOCH` | (process 1 only) no reply until no job is accepted, queued or running, then `drained EPOCH`; meanwhile every `submit` is refused |

Any request can be answered `error WHY` (unknown id, malformed request). A
ROW is `ID KIND STATE STATUS POSITION FROM ARGS LAST`, tab separated: STATUS
-1 until it ends (128 + N: signal N), POSITION 0 running, N queued, -1 ended,
ARGS its arguments separated by spaces, LAST the last line of its output.

`base/include/jobs.h` (`base/jobs.c`, linked into the programs, not into
process 1) wraps it: `jobs_submit`, `jobs_list`, `jobs_status`, `jobs_log`,
`jobs_cancel`, `jobs_mine` (the jobs this program follows: its own by name,
`-window` stripped, and any running or queued job in the same domain so
`packages`, `settings` and `firmware` see each other's active work),
`jobs_pending`. `locachy-pkg` and `locachy-repos` also hold `/run/locachy/pkg.lock`
across mutating runs so a direct root terminal command cannot collide with a
running job.

## Stopping a job

A queued job is removed at once. A running one only with `confirm`, which
the service asks for, saying why: a package transaction stopped midway can
leave the system half-changed. Stopping sends SIGINT, as Ctrl+C does, to
what the job's step runs (pacman's own interrupt: it stops between two
packages and releases its lock), not to the step itself (a Python tool
interrupted while it waits for pacman kills pacman 0.25 s later); to the step
when it runs nothing else. If the job still runs 20 s later, everything in its
cgroup is killed.

home is where a person stops one: `J` lists the jobs, `K` asks (Y/N, with
the warning for a running one), and Y removes or stops it.

## The service ended or restarted

SIGTERM (process 1 ending the service, its line gone from rc.conf) ends the
service alone: a running job goes on in its own session and cgroup. The next
start reads the state files, finds the running job by the lock its runner
holds (and watches it through a pidfd), takes the end of one that ended
meanwhile from `ID.exit`, and goes on with the queue. A service that is
killed starts again the same way.

## Shutdown

home refuses to reboot or power off while a job runs or waits, saying which
(`jobs_pending`; with no service, a job cgroup that still has processes
counts).

Process 1's own power off and reboot wait while a job runs, before anything
is ended: `poweroff` and `reboot` as root (SIGUSR2, SIGTERM), Ctrl+Alt+Del,
the power button, home's request and the critical battery. A job runs while
a `/sys/fs/cgroup/locachy-jobs/job-*/cgroup.events` says `populated 1`;
process 1 looks at nothing else of the jobs (no socket, no queue), and reads
the first line of `/run/locachy/jobs/ID.state` only for the kind it shows.

- It watches those files and their folder (inotify), with no timer. Nothing
  gets the screens meanwhile: process 1's own screen and the kernel log say
  which job (its id and kind) and how to go ahead now. The service and every
  other service keep running, so a job that needs the network or a driver
  goes on. The lid does nothing meanwhile, as logind ignores it while an
  action is under way.
- The service starts the next queued job as soon as one ends, and that one is
  waited for too. Once no job has run for 1 s, the shutdown goes on as
  before: everything ended (SIGTERM, then SIGKILL 2 s later), the ESP
  unmounted, the root read-only.
- **Going ahead at once**, cutting the job (the log names it):
  Ctrl+Alt+Del while it waits (a reboot), the power button (a power off), or
  `poweroff --force` / `reboot --force` (also `-f`) at any time, waiting or
  not. The two commands send systemd's signals for an immediate power off
  and reboot, SIGRTMIN+14 and +15, by number: 48 and 49 (glibc's SIGRTMIN,
  34; process 1's musl has 35); the rest of the shutdown is the same. A key
  counts as asking again when it is a new press: Delete held (it repeats), or
  one press seen on two devices, within 1 s of the last, does not. `poweroff`
  or `reboot` without `--force` while it waits, or home's request, does not
  force it; the last one says whether it powers off or reboots.
- **The critical battery** hibernates where there is a resume device, as
  without a job: the job is in the image and goes on after the resume.
  Without one, or when the image cannot be written, it powers off, and a job
  that runs gets **60 s at most**, said on the screen; then it is cut,
  logged as `POWER: the critical battery's power off waited 60 s; system job
  ID (KIND) still runs and is cut`. Cut at once, pacman or a firmware write
  is left half done for certain; waited for without end, the empty battery
  would cut it as well, with the root still writable. A power off already
  waiting gets the same bound.
- Resuming boots the firmware and the boot image again: a job that writes
  them (`boot`, `kernel-select`, the firmware kinds) is, hibernated midway,
  only as safe as a power cut there.
- The waiting screen is in English: process 1 draws with Terminus alone,
  which has no CJK (as its boot passphrase prompt).

**The drain.** A power off or reboot that does not go ahead at once first
asks the service to drain (`drain EPOCH`, accepted from process 1 only), then
waits for its answer as well as for the job cgroups: from that request no new
job is accepted, and the jobs already accepted, the queued ones included, run
and end before `drained EPOCH` comes. The epoch is kept in
`/run/locachy/jobs/drain`, so a service started again refuses new jobs too,
until the next boot. When there is no service, or the connection ends
without an answer (the service has gone), that counts as drained: what it had
queued stays on disk, unstarted, through the shutdown, and nothing can start
it, since only the service starts jobs; a job that still runs is waited for by
its cgroup as before. `--force` and a second press go ahead as before.

## The lock

Job requests are not refused while the screen is locked: the queue is not
the screen's, and whoever can reach the socket is root outside the
containers. A program frozen under the lock sends nothing; a job asked for
before the lock goes on.

## The programs

- **settings**: its package rows (Tailscale, Clash, Tor, Bluetooth, faces and
  their dlib, cloud drives, the input method and Chinese Pinyin, the optional
  modules, fingerprints), the boot kernel and its headers, the repositories
  and the mirrors, and the boot image ask for jobs; a row shows the last line
  of its job's output, or its place in the queue. Closing settings leaves at
  once; opened again, it follows the jobs it asked for that have not ended.
- **packages**: checking for updates, applying them, syncing the host to its
  manifest, adding and removing host packages, building, rolling back and
  removing containers. Its pane shows the last job it asked for (also when it
  is opened again); a job that stopped for source signing keys shows them,
  and `g` asks for what is left of it as a new job.
- **firmware**: fwupd's refresh, update, verify, unlock and installs,
  installing fwupd or flashrom, and every flashrom operation.
- **home**: the queue (`J`), stopping a job (`K`, then Y), reboot and power
  off held while jobs run or wait.

## Inventory

What the programs start, and whether it is a job:

| Where | Operation | Job? | Why |
|---|---|---|---|
| settings | `locachy-pkg install/remove`, the dlib swap | yes | pacman transaction and the rebuild after it: minutes |
| settings | `locachy-pkg kernel select / headers` | yes | a pacman transaction and the boot image |
| settings | `locachy-repos apply --set`, `rank` | yes | rewrites pacman's configuration, checks the declared packages, ranks mirrors on the network |
| settings | `locachy-boot` | yes | writes the ESP; pacman's hooks run it too, so it must not overlap a transaction |
| settings | `locachy-tpm enroll`, Secure Boot keys | yes | terminal collects the passphrase and preserves the explicit key choices; writes and nested boot rebuilds are one system job |
| settings | fwupd's report, `tailscale up --qr`, howdy and its camera, bluetui | no | interactive reports or device interaction |
| settings | `fprintd-enroll` | no | a finger on the reader, cancelled with q or Esc |
| settings | `locachy-tpm clear` | yes | LUKS/pending changes and the boot rebuild share one job |
| settings | `tor-enforce`, `locachy-setup debug-link`, `nmcli`, `keytable`, the status queries | no | short helpers |
| packages | `locachy-pkg plan/apply/sync/install/remove`, `locachy-app build/rollback/remove` | yes | package transactions, builds, network |
| packages | `locachy-pkg plan --cached`, `status`, `info`, `search`, `commands` | no | queries |
| firmware | fwupd refresh, update, verify, unlock, local install, release install | yes | firmware writes and downloads |
| firmware | installing fwupd or flashrom | yes | a pacman transaction |
| firmware | SPI probe, backup, verify, write | yes | flashrom on the chip: never two at once |
| firmware | `bios-set`, the device, release, attribute and programmer lists | no | one sysfs write, or queries |
| process 1 | `locachy-tpm enroll --pending --submit-only` at boot | yes | submitted after services start; no boot write outside the queue |
| locachy-bar | `locachy-app open/stop` of the tray | no | container lifecycle, process 1's registry |

packages and firmware ran their jobs in the root cgroup before, out of any
management; settings held its window's close while one ran. Both are gone.

## Tests

- `tools/test-jobs.sh` (in `test-interaction-regressions.sh`): the service as
  root of a user namespace with a private `/run`, fixture cgroups and peers'
  `/proc/PID/cgroup`, and `test-jobs-tool.sh` in place of the programs;
  plain and with AddressSanitizer/UBSan. The kinds and their arguments, one at
  a time and in order, every repeated request registered separately, the stopped-for-keys state,
  the files and their modes, a job outliving the client that asked, removing
  and stopping (SIGINT to what the step runs, the kill after the grace),
  the peers (a container, a window, a direct program, the user), the service
  ended (SIGTERM) and killed and started again, the queue seen without the
  service.
- `tools/test-home.c`: the queue listed, stopping and removing only after Y,
  reboot and power off refused while jobs run or wait.
- `tools/test-init-shutdown.c` (in `test-interaction-regressions.sh`):
  process 1 with fixture job cgroups and state files: no job, it goes at
  once; one runs, it waits (the screen, the log, the watch with no timer),
  sees the next queued one start, goes on 1 s after the last ended and only
  then ends everything; Ctrl+Alt+Del and the power button again, `--force`;
  the critical battery hibernating, or waiting 60 s at most; `link/poweroff`
  and `link/reboot` run as root of a user and PID namespace, whose process 1
  checks the signal each sends.
- `tools/test-settings-close.c`: settings leaves at once under a job, asks
  for its kinds with their arguments, follows its jobs again when reopened.
- `tools/test-packages.c`, `tools/test-firmware.c`, `tools/test-settings-*.c`:
  the programs with a queue double (`tools/jobs-double.h`, or one that runs
  the doubles of their tools).
