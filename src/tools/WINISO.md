# Installation ISOs for Windows (`locachy-winiso`)

[中文](WINISO.zh.md)

Windows VM apps ([VM-APPS.md](VM-APPS.md)) and Maintenance Windows
([WINMAINT.md](WINMAINT.md)) need two ISOs: the Windows installation ISO and the
virtio-win driver ISO. `locachy-winiso` supplies both, so nobody has to give
their paths:

- **virtio-win** comes from the AUR package `virtio-win` (in the `winmaint`
  module). Its recipe downloads one pinned release from Fedora's
  `fedorapeople.org` archive, checked against the SHA-256 in the PKGBUILD, and
  installs it as `/var/lib/libvirt/images/virtio-win.iso`. Updates come as
  package updates; LOCachy does not download it itself.
- **Windows** is downloaded from Microsoft by `locachy-winiso fetch windows` and
  kept only when its SHA-256 equals the value Microsoft publishes for it.

## Commands

```
locachy-winiso path windows|virtio-win [--lang L]
locachy-winiso fetch windows [--lang L]
locachy-winiso check [--lang L]
```

- `path virtio-win` prints the package's ISO; `path windows` prints the Windows 11
  ISO the last successful `fetch` verified (with `--lang`, that language).
  Exactly one absolute path on stdout and status 0, or status 3 when that ISO is
  not there. `locachy-app` (Windows custom VMs) and `locachy-winmaint` call it.
- `fetch windows` downloads Windows 11. The language is
  `--lang`, else `language` in `/etc/locachy/rc.conf`, else `en-US`. Both
  language tags (`de-DE`) and locales (`zh_CN.UTF-8`) are accepted; a language
  without its region takes Microsoft's main one (`zh` is `zh-CN`, `pt` is
  `pt-PT`). A language Microsoft has no ISO in is refused with `--lang`; from
  rc.conf it falls back to `en-US`, saying so.
- `check [--lang L]` uses the same language rules and tests Microsoft's download
  chain without downloading or installing the ISO. It needs no write access to
  the ISO directory and runs directly, not as a system job.

Exit status: 0 done, 1 local failure, 2 wrong use, 3 not available (`path`),
4 Microsoft refused or another upstream error occurred (`fetch` or `check`),
5 the downloaded ISO's SHA-256 did not match (`fetch`).

## How `fetch` works

One chain of requests per run, each made once. A refusal or any answer from
Microsoft that is not a success ends the chain at once; nothing is retried,
no header, user agent or session is changed to get past it, and no other URL,
language or version is tried.

1. The download page (`https://www.microsoft.com/en-us/software-download/windows11`).
   Its "Verify your download" table gives the SHA-256 for
   the language (`English 64-bit`, `Chinese Simplified 64-bit`, …); without
   exactly one such value nothing else is requested. The page also names the
   product edition (`option value`) the download service is asked for.
2. If the Windows 11 ISO already kept for that language has that SHA-256,
   steps 3 to 7 are not made at all.
3. Session registration: at the start of a fetch, any leftover regular
   `.session.*.cookies` files in `.partial/dl/` (checked with `lstat`, never
   following symlinks, and restricted to the process's uid and `nobody` when
   running as root) are removed. Then the session permit
   (`vlscppe.microsoft.com/tags`) is fetched with the org id and a new session
   id, followed by the two-step session check on `ov-df.microsoft.com`
   (`mdt.js` and its reply). All steps of a fetch share a per-fetch cookie jar
   under `.partial/dl/` (removed once the link step ends) so Microsoft's
   download service sees one coherent session. Each metadata/API request runs
   with a 20-second connect timeout and 60-second total timeout.
4. The language list of that edition (`getskuinformationbyproductedition`),
   the same session. The entry for the language gives its SKU.
5. The link (`GetProductDownloadLinksBySku`), the same session, with the
   download page as referer. Sentinel refusals are detected from the parsed
   JSON `Errors` entries (`Key`, `Value`, or `Type`), falling back to raw text
   only when the response is not valid JSON. From `ProductDownloadOptions`, the
   x64 link is selected by `DownloadType == 1` first; only if no such option
   exists does it fall back to an option whose URL path (excluding the query
   string) contains `x64` case-insensitively, refusing if there is not exactly
   one candidate. The link is valid for a day.
6. curl (as `nobody` when run as root) downloads the link into
   `/var/lib/locachy/isos/.partial/dl/` (with a 30-second connect timeout),
   continuing a partial file left by an earlier attempt of the same build. A
   transfer that stops ends the fetch (status 4); its partial file stays, and
   the next `fetch` continues it.
7. Root opens the file without following links, verifies it is one regular file
   with a single hard link, and hashes that descriptor against Microsoft's
   SHA-256. A mismatch deletes it (status 5). A match links that verified inode
   into `/var/lib/locachy/isos/windows-11-LANG.iso` (root-owned, mode 0644,
   so a VM's uid can read it), unlinks the `.partial/dl/` entry, writes the
   record `windows-11-LANG.iso.json` (SHA-256, size, Microsoft's file name,
   the table row, the time) and points `windows.iso` at it.

When `fetch` runs as root, every network request runs as `nobody`: the download
page, session registration, the two API requests and the ISO transfer. Only `nobody` can write into
`.partial/dl/`. Root never trusts a path that `nobody` can swap: it opens the
downloaded file without following links, checks that it is one regular file
with one hard link, hashes that descriptor, and installs the same inode. A process running as `nobody` that already holds a
write descriptor on the partial file keeps it after root takes ownership, so it
could still change the bytes after they were hashed. That residual risk is
stated here, not closed. The requests run inside `locachy-net` (through the
network layers that are on).
Every request carries the User-Agent `winiso/1` and nothing else of the system,
the host, the person or a mail address.

**No other source.** When Microsoft refuses (it commonly does over Tor, VPNs and
some networks: its answer then carries "Sentinel marked this request as
rejected", or it answers with an error status), `fetch` stops with status 4 and
tells the person to download the ISO in a browser and give its path instead
(`vm_iso="..."` for a VM app, `locachy-winmaint vm --iso PATH`). It never
switches to a mirror or another site.

## How `check` works

`check` runs steps 1, 3, 4 and 5 even if an ISO is already installed. It uses a
private temporary directory for the session cookie jar and removes it when
link resolution ends. It does not read or change the installed ISO, `.partial/`
or `windows.iso`.

After validating the x64 link against the allowed Microsoft URL prefix, curl
sends one `Range: bytes=0-0` GET, with `winiso/1`, a 20-second connect timeout and
a 60-second total timeout. A valid HTTP 206 response must report `Content-Range:
bytes 0-0/<size>` and transfer exactly one byte. If the server ignores the range
and responds with HTTP 200, curl limits the size and the check fails; no ISO is
saved. Redirects are not followed for this probe, and there are no
retries or alternate sources. On success stdout reports the language, ISO
filename, Microsoft's published SHA-256 and the HTTP status:
`ok: windows-11 en-US <filename> sha256=<64hex> range=206`. This does not verify
the ISO's contents. A refusal, unexpected status or unusable Microsoft response
ends the check with status 4.

## The system job

On the running system `fetch` runs as the fixed system job `winiso-fetch`
([JOBS.md](JOBS.md), registered in `progs/locachy-jobs.c`): `locachy-winiso
fetch windows --lang TAG`, the language decided before the job is
submitted. The terminal follows the job; closing it does not stop the download.
In a job, or offline (a chroot), it runs inline. A job service that refuses or
is missing is a failure, never a fallback to running outside the queue.

## Files

| Path | What |
| --- | --- |
| `/var/lib/locachy/isos/windows-11-en-US.iso` | a verified Windows 11 ISO (0644; only `windows-11-<LANG>.iso` names are accepted) |
| `/var/lib/locachy/isos/windows-11-en-US.iso.json` | its record; `path` needs it, and its size must match |
| `/var/lib/locachy/isos/windows.iso` | symlink: the ISO `path windows` prints |
| `/var/lib/locachy/isos/.partial/` | root 0711 (`nobody` can enter it, not list it): the record `*.part.json` and `.lock` (root 0600) |
| `/var/lib/locachy/isos/.partial/dl/` | `nobody` 0700: `<name>.part` while a transfer runs and the per-fetch `.session.*.cookies` jar (stale regular jars cleaned at fetch start) |
| `/var/lib/libvirt/images/virtio-win.iso` | the virtio-win package's ISO |

A newer build of the same language replaces the old file.

## Packages and licences

| Package | Where | Source | License |
| --- | --- | --- | --- |
| `virtio-win` | AUR (`winmaint` module) | `fedorapeople.org/groups/virt/virtio-win/direct-downloads/archive-virtio/virtio-win-VERSION/virtio-win.iso`, SHA-256 pinned in the PKGBUILD | BSD-3-Clause (package) |

The package is built from its AUR recipe as it is; it is not in the Arch
repositories. Nothing else is installed for the Windows download: the link step
is in `locachy-winiso`, which reads Microsoft's own pages and answers.

**Credit.** The request sequence of the link step follows the public flow
documented by Fido (github.com/pbatard/Fido, GPL-3.0; Windows-only by design),
Mido (github.com/ElliotKillick/Mido, MIT), dockur/windows
(github.com/dockur/windows, `src/mido.sh`, MIT; actively maintained) and quickget
(github.com/quickemu-project/quickemu, MIT, the quickemu project), with
Microsoft's page constants. None of them is installed or run; they are the
reference, and the code follows their flow if Microsoft changes it.

## Test seams

Environment settings for tests (never consulted as a fallback):
`LOCACHY_ISO_DIR`, `LOCACHY_VIRTIO_WIN_ISO`, `LOCACHY_RC_CONF`,
`LOCACHY_WINISO_MS_BASE` (the download page and the download service),
`LOCACHY_WINISO_TAGS_BASE` (the session permit),
`LOCACHY_WINISO_OVDF_BASE` (the `ov-df` session check) and
`LOCACHY_WINISO_LINK_PREFIX` (where links must point); the URL seams accept
`https://` only.

## Test Coverage

`tools/test-winiso.py` runs the real program and curl against a local HTTPS
server standing in for Microsoft: the download page with its table, the session
permit, the two-step `ov-df` check, session cookies, the language list, the
link (or a refusal), and the ISO's address with ranges. A job queue double
stands in for the system job service. The tests cover: `path`, a good download
with one chain of requests, cookie forwarding and cleanup of the temporary and
stale cookie jars (without following symlinks), Sentinel refusals from parsed
JSON `Errors` entries and from a non-JSON fallback (exit 4, nothing retried),
non-Sentinel JSON errors and HTTP error responses, x64 link selection
(`DownloadType == 1` preferred, case-insensitive `x64` URL-path fallback
ignoring query strings, ambiguous links refused), `windows-11-*` ISO name
enforcement, the `--connect-timeout 20` flag on metadata fetches, a checksum
mismatch (deleted), a transfer that stops (no retry in the run; the next fetch
continues it), resume of an earlier partial file, a partial of another build
discarded, a complete partial (HTTP 416), the job route, the User-Agent and the
requests' contents (the local server checks every request for the generic
client, the session, the cookies, the referer, and the absence of the project
name, the current runtime user name, the host name and `@`), the source's
credit, the layout of the partial directories, planted symlinks and hard links
refused, the fd-based install (the same inode; a partial unlinked, swapped or
renamed aside after it was opened is refused), and the argument order of the
privilege drop (static, and with the drop mocked). Not run in the offline test
suite: the real privilege drop (`setpriv` to `nobody` under real root, and what
`nobody` can see of the root-owned `.partial/`). That needs a real-root host
and is UNVERIFIED.

The `check` tests cover language selection, the full chain with a one-byte
HTTP 206 response, a size-limited HTTP 200 response when the range is ignored,
no retry after Sentinel refusal or an unexpected status such as 403 or 404,
and rejection of a link outside the allowed prefix. They also verify that the
check works with a missing or read-only ISO directory, submits no job, sends
no private identifiers, and cleans up the temporary cookie jar.

From the repository root, `python3 src/tools/test-winiso.py` runs only the
mock suite. Add `--live` (or set `LOCACHY_WINISO_LIVE=1`) to run exactly one
live `locachy-winiso check --lang en-US` after the mock suite. The output
separately reports success, Microsoft refusal or upstream error, and
response-format/parsing errors. The live result depends on Microsoft and the
network it sees and does not change the offline suite's result.
