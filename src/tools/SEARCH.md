# Global search (locachy-searchd)

[中文](SEARCH.zh.md)

LOCachy's global search has two layers behind one shortcut, `key_search` in
`/etc/locachy/rc.conf` (default `Super+Space`):

- **Layer 1 (names, instant):** file names under the configured search roots,
  answered from the SQLite name index without waiting for document scans. The
  search program also lists host programs and containers by name (Settings,
  for example).
- **Layer 2 (contents):** full-text search over document and text file bodies,
  streamed after layer 1 results with no mode switch.

## Components and build

| Part | Installed as | Built by make.sh when | Needs |
|---|---|---|---|
| Index daemon | `/usr/local/lib/locachy/locachy-searchd` (`progs/locachy-searchd.c`, `progs/search-index.c`) | `pkg-config --exists sqlite3` | SQLite; the daemon refuses a library older than 3.43 (FTS5 `contentless_delete`), 3.45+ recommended |
| Text extractor | `/usr/local/bin/locachy-docscan` (`progs/locachy-docscan.c`) | `pkg-config --exists libarchive` | libarchive; MuPDF for PDF (optional) |
| Search program | `/usr/local/bin/search` (`progs/search.c`) | `progs/search.c` exists | the window transport objects, IME and FreeType like `files` |

make.sh checks only that sqlite3 is present; the version check happens when the
daemon opens the index (`SEARCH_ERR_SQLITE`).

### MuPDF in locachy-docscan

The switch is `DOCSCAN_NO_MUPDF`, not `HAVE_MUPDF`: MuPDF is compiled in by
default (`locachy-docscan.c`). make.sh and `build-check.sh` pick it like
`tools/test-docscan.sh` does:

- `pkg-config --exists mupdf`: its `--cflags` and `--libs`, plus `-lm`;
- else `/usr/include/mupdf/fitz.h` exists: `-lmupdf -lm`;
- else `-DDOCSCAN_NO_MUPDF`. The binary still builds, and answers PDF files with
  "PDF support not built in" (exit 4, unsupported); DOCX, ODT, EPUB and text
  work.

The extractor always links libarchive (`pkg-config --cflags --libs libarchive`),
and is built with `$GCC` like `archive-worker`.

## Service and storage

`locachy-searchd` runs as a host service (`service_search` in
`/etc/locachy/rc.conf`) and links the portable index engine
(`progs/search-index.h`, `progs/search-index.c`).

- **Database:** `/var/lib/locachy/search/index.db`.
- **Pragmas:** `auto_vacuum=INCREMENTAL` (set before the first table is
  created), `journal_mode=WAL`, `synchronous=NORMAL`, `temp_store=MEMORY`,
  `cache_size=-4000`, and `user_version=1` for schema compatibility checks.
- **Tables:**
  - `meta(key, value)`: holds the crawl `generation` counter.
  - `dirs(id, parent, name, mtime, gen)`, `UNIQUE(parent, name)`: the directory
    tree. Moving a directory is a single `UPDATE` on `dirs`, and deleting a
    subtree is one recursive query over `parent`. Full paths are rebuilt from the
    parent chain with an in-memory hot-path cache.
  - `files(id, dir, name, size, mtime, kind, gen, text_state)`,
    `UNIQUE(dir, name)`: file metadata, with indexes on `(dir, name)`, `gen`,
    `mtime`, `name COLLATE NOCASE`, and a partial index on pending or stale
    `text_state`.
  - `names_fts`: external-content FTS5 trigram table over `files.name`, kept in
    step by `INSERT`, `DELETE` and `UPDATE OF name` triggers.
  - `docs_fts`: contentless FTS5 trigram table (`content=''`,
    `contentless_delete=1`, rowid = `files.id`) for content queries of 3 or more
    characters.
  - `docs_uni`: `unicode61` FTS5 table (same rowids) for 1–2 character prefix
    content queries (`"q"*`) and snippet context.

## Query matching and safety

- Queries of 3 or more UTF-8 characters use `names_fts` (layer 1) and `docs_fts`
  (layer 2). All user text is double-quote escaped and passed by bound
  parameters, never concatenated into SQL.
- Queries of 1–2 UTF-8 characters use a `NOCASE` prefix range scan, plus a
  `LIKE ... ESCAPE '\'` scan over the 2000 most recently changed files, for
  layer 1; and `docs_uni` prefix matching for layer 2 (CJK prefixes included).
- Every query is capped at 200 results. Before the reply is written, the top 20
  paths are checked with `faccessat(F_OK)`; entries gone from disk are dropped
  from the reply and queued for removal from the database.

## Crawling, exclusions, and live updates

- **Roots:** `search_roots` in `/etc/locachy/rc.conf`, colon-separated absolute
  paths. When unset, the root is the uid 1000 user's home directory from
  `/etc/passwd`. `SIGHUP` reloads the configuration and starts a fresh crawl.
- **Exclusions:** hidden files and directories (leading `.`: `.git`, `.cache`,
  `.local/share/Trash`, ...), `node_modules`, `__pycache__`, and `<root>/Drives`.
- **Mount safety:** the crawler never crosses a filesystem boundary (`st_dev`
  checked on `lstat`/`fstatat` and on the opened directory descriptor). It reads
  `/proc/self/mountinfo` and skips FUSE (`fuse*`, including rclone's `~/Drives`
  and `sshfs`), network filesystems (`nfs*`, `cifs*`, `smb*`, `davfs`, `9p`,
  `afs`, `ceph`, `glusterfs`), `autofs`, and kernel virtual filesystems
  (`proc`, `sysfs`, `devtmpfs`, `devpts`, `cgroup*`, `debugfs`, `tracefs`).
- **Live updates:** inotify watches up to half of
  `/proc/sys/fs/inotify/max_user_watches`. Events are batched into one SQLite
  transaction about every 250 ms. `IN_MOVED_FROM`/`IN_MOVED_TO` pairs are matched
  by cookie and update `dirs`/`files` in place.
- **Overflow and offline deletions:** directories beyond the watch budget are
  re-checked every 10 minutes through a `timerfd` (disarmed when there are none),
  skipping directories whose `mtime` is unchanged. Each full crawl advances the
  generation counter; when it completes, rows of older generations are swept and
  pages reclaimed by incremental vacuum.

## Power and document extraction

- The daemon runs at `nice 19` and with `IOPRIO_CLASS_IDLE`.
- When no crawl, batch flush, move expiry, ghost cleanup or extraction is
  pending, `poll()` blocks without a timeout (zero idle wakeups).
- Document extraction never runs inside the daemon. Eligible files (document
  kinds `pdf`, `docx`, `odt`, `epub`, and the text and code extensions, size
  1 byte to 8 MiB) wait in a queue of at most 512 and are passed one at a time
  to `/usr/local/bin/locachy-docscan FILE`. It writes at most 1 MiB of UTF-8 to
  stdout and exits 0 on success. Each run has a 5 s timeout and is killed on
  overrun.
- The extractor is sandboxed: it drops to the file's owner (to `nobody` when the
  owner is root), sets `PR_SET_NO_NEW_PRIVS`, limits address space to 512 MiB and
  CPU time to 10 s, closes inherited descriptors, and refuses symbolic links.
- Extraction pauses when `search_docs="0"` in `rc.conf`, or on battery below 20%
  (read from `/sys/class/power_supply`; peripheral device batteries are ignored).
  File name indexing continues.

## Query socket protocol

- **Socket:** `/run/locachy/search.sock` (`AF_UNIX`, `SOCK_STREAM`, mode 0666).
- **Access control:** `SO_PEERCRED` accepts only uid `0` or uid `1000`; other
  peers are closed at once.
- **Request:** one line of at most 256 bytes, no NUL bytes:
  `Q <scope> <query>\n`, with `<scope>` `names`, `content` or `all`.
- **Response:** zero or more result lines, then `E\n`. At most 200 results in
  total; layer 1 rows first, then layer 2 rows:
  `R <layer> <kind>\t<path>\t<name>\t<snippet>\n`, where `<layer>` is `1` or `2`
  and `<kind>` is `file`, `text`, `doc`, `image`, `media`, `archive` or `dir`.

## Configuration (`rc.conf`)

| Key | Default | Meaning | Written by make.sh |
|---|---|---|---|
| `service_search` | `"/usr/local/lib/locachy/locachy-searchd"` | starts the daemon under process 1 | when the daemon is built and the key is absent |
| `search_roots` | uid 1000 home | colon-separated absolute paths to index | no |
| `search_docs` | `"1"` | `"1"` extracts document text (layer 2); `"0"` pauses it | when absent |
| `key_search` | `"Super+Space"` | global shortcut that opens search | when absent |

Overrides for tests and manual runs: `LOCACHY_RC_CONF`, `LOCACHY_SEARCH_DB`,
`LOCACHY_SEARCH_SOCK`, `LOCACHY_DOCSCAN_BIN` and `LOCACHY_POWER_SUPPLY_DIR`.

An rc.conf from before the daemon gets `service_search` only when the binary
exists, and process 1 is told to reload its services. Nothing is removed when
the daemon is not built; a stale `service_search` line then starts nothing.

## Status

- **Search program:** `progs/search.c` is in the tree. On the shared Wayland
  display (`LOCACHY_WINDOW_FLOATING=1` set by `init/apps.c`), `wayland-backend.c`
  opens it as a centered, always-on-top `wlr-layer-shell` overlay
  (`LAYER_OVERLAY`, `760×480` logical pixels, exclusive keyboard focus, no title
  bar; falls back to a client-side undecorated `xdg_toplevel` if layer-shell is
  absent), and pressing `key_search` while it is open sends `close` over
  `LOCACHY_REQUEST_FD` to toggle it closed. On the host framebuffer, it renders
  as a centered floating card over a dimmed backdrop and dismisses on outside
  click or `Esc`. Its device-free tests pass on the build host
  (`test-search-ui`, `test-search-index`, `test-docscan`); its UI on a real
  target is not verified yet. make.sh and `build-check.sh` build `search` (and
  `search-window`) from it when the file exists.
- **Shared display:** `link/labwc-rc.xml` binds `SEARCH-KEY` to
  `locachy-seat --window search`; seat accepts the program `search`, and
  settings' labwc writer (`labwc_line`) puts `key_search` in place of
  `SEARCH-KEY`. seat's own fallback, used only when `settings --apply` fails,
  binds `SEARCH-KEY` to its default `W-space` (Super+Space).
- **Host framebuffer:** process 1 handles `K_ACT_SEARCH` (`init/init.c`): it
  opens search on the focused screen, unless the session is locked, shared, or
  the focused task is holding the screen. `search.c` itself acts on the same
  key. No other framebuffer program dispatches it.
- **Settings:** the Settings program has rows for the global search key (an
  edit that checks conflicts, and a delete), the search index switch and
  document content indexing (`search_docs`). `search_roots` has no row: change it
  in `rc.conf`.
- **Build check:** `tools/build-check.sh` builds the search candidates under
  `-Werror` on the build host.


## Tests

- `tools/test-search-index.c`: the portable engine, covering the pragmas (WAL,
  `auto_vacuum=INCREMENTAL`, `user_version`), foreign and mismatched schemas,
  directory and file moves, recursive subtree deletion, 1–2 and 3+ character
  ASCII and CJK queries, `contentless_delete=1` updates, FTS5 and SQL escaping,
  the 200-result cap, generation sweeps, ghost filtering and stale pruning.
- `tools/test-docscan.c` with `tools/test-docscan.sh`: builds `locachy-docscan`
  with the MuPDF choice above (`--require-pdf` fails without MuPDF) and runs the
  extractor's tests.
