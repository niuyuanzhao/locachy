#!/usr/bin/env python3
"""The app presets: a catalog from a repository of their own (locachy-apps),
apart from the system's source. Its files are data, parsed and validated,
never sourced or run; a catalog is taken whole or not at all, and switched in
one step, so that a failure keeps the one there was.

A repository has FORMAT (its format: one line, 1) and its presets in
apps/NAME.conf. Installed, the catalog is APPS_DIR/catalog: NAME.conf, and
CATALOG, where it came from (KEY="VALUE" lines, as rc.conf). The updater
(locachy-pkg) fetches into APPS_DIR/repo.git. A configuration made from a
preset (/etc/locachy/containers/NAME.conf) follows the preset's changes in
what the user left as it was: its base is APPS_DIR/baseline/NAME.conf, the
preset it last followed, else the installed catalog's.

  python3 -I locachy_apps.py validate DIR    DIR, a repository's tree: OK, or why not
  python3 -I locachy_apps.py catalog DIR DEST ORIGIN REPO BRANCH COMMIT
                                             DIR's presets, validated, as a new catalog DEST
  python3 -I locachy_apps.py bootstrap LEGACY
                                             the old system's app presets (LEGACY: its
                                             presets of containers) as the catalog, while
                                             there is none (make.sh)
"""
import ctypes
import glob
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time

APPS_DIR = "/var/lib/locachy/apps"
SOURCE_COMMIT_FILE = "/usr/local/lib/locachy/source-commit"
FORMATS = ("1",) # the repository formats this system reads
MAX_BYTES = 16 * 1024
NAME = re.compile(r"[a-z0-9][a-z0-9._-]{0,62}")
COMMIT = re.compile(r"[0-9a-f]{40}")
LINE = re.compile(r'([a-z0-9_]+)="(.*)"')
# No quote, backslash, $, backquote or control character: the shell reads
# these values too (locachy-app).
VALUE = re.compile(r'[^"\\$`\x00-\x1f\x7f]*')
PACKAGE = re.compile(r"[A-Za-z0-9@_+][A-Za-z0-9@._+-]*") # as pacman takes it (locachy-pkg)
DESKTOP = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]*\.desktop")
META_KEYS = ("format", "origin", "repo", "branch", "commit", "installed", "system_commit")
ORIGINS = ("repository", "medium", "bootstrap")


class CatalogError(ValueError):
    """Not a catalog this system takes; the message says why."""


def _path(value):
    """An absolute path, no .. in it."""
    return value.startswith("/") and len(value) > 1 and ".." not in value.split("/")


# A preset's keys, those of locachy-app's configuration but host (a host
# program is the system's, never an app preset), and what each may hold.
CHECKS = {
    "package": (lambda v: bool(PACKAGE.fullmatch(v)), "one package name"),
    "extras": (lambda v: all(PACKAGE.fullmatch(w) for w in v.split()), "package names separated by spaces"),
    "command": (lambda v: _path(v) and " " not in v, "an absolute path"),
    "arguments": (lambda v: True, ""),
    "network": (lambda v: v in ("0", "1"), "0 or 1"),
    "camera": (lambda v: v in ("0", "1"), "0 or 1"),
    "x11": (lambda v: v in ("0", "1"), "0 or 1"),
    "directories": (lambda v: not v or all(_path(d) for d in v.split(":")),
                    "absolute paths separated by colons"),
    "desktop": (lambda v: all(DESKTOP.fullmatch(d) for d in v.split()), "desktop file names separated by spaces"),
}
REQUIRED = ("package", "command")


def preset_values(name, data):
    """KEY -> VALUE of preset NAME (its bytes), validated: UTF-8, each line
    empty, a comment or KEY="VALUE", each key known and once, each value of
    its kind; package and command there."""
    if len(data) > MAX_BYTES:
        raise CatalogError(f"{name}.conf: more than {MAX_BYTES} bytes")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise CatalogError(f"{name}.conf: not UTF-8") from None
    values = {}
    for number, line in enumerate(text.split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        where = f"{name}.conf: line {number}"
        match = LINE.fullmatch(line)
        if not match:
            raise CatalogError(f'{where}: not KEY="VALUE"')
        key, value = match.groups()
        if key == "host":
            raise CatalogError(f"{where}: host programs are the system's, never an app preset")
        if key not in CHECKS:
            raise CatalogError(f"{where}: unknown key {key}")
        if key in values:
            raise CatalogError(f"{where}: {key} a second time")
        if not VALUE.fullmatch(value):
            raise CatalogError(f"{where}: {key} has a quote, backslash, $, backquote or control character")
        check, kind = CHECKS[key]
        if not check(value):
            raise CatalogError(f"{where}: {key} is not {kind}")
        values[key] = value
    for key in REQUIRED:
        if key not in values:
            raise CatalogError(f"{name}.conf: no {key}")
    return values


def _read(path):
    """The bytes of path: a regular file, not a link, MAX_BYTES at most."""
    name = os.path.basename(path)
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        raise CatalogError(f"no {name}") from None
    if not stat.S_ISREG(st.st_mode):
        raise CatalogError(f"{name}: not a regular file")
    if st.st_size > MAX_BYTES:
        raise CatalogError(f"{name}: more than {MAX_BYTES} bytes")
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW), "rb") as f:
        return f.read(MAX_BYTES + 1)


def _presets(directory, extra=()):
    """NAME -> bytes of the presets in directory, where every entry but extra
    is a valid NAME.conf: the first that is not rejects them all."""
    found = {}
    for entry in sorted(os.listdir(directory)):
        if entry in extra:
            continue
        name = entry[:-5] if entry.endswith(".conf") else ""
        if not NAME.fullmatch(name):
            raise CatalogError(f"{entry}: not a preset's name (NAME.conf; NAME of a-z, 0-9, . _ -)")
        data = _read(os.path.join(directory, entry))
        preset_values(name, data)
        found[name] = data
    return found


def validate_tree(tree):
    """(format, NAME -> bytes) of a repository's tree: a FORMAT this system
    reads, and apps/ of valid presets, at least one. CatalogError says what
    is not."""
    data = _read(os.path.join(tree, "FORMAT")).decode("utf-8", "replace")
    fmt = data[:-1] if data.endswith("\n") else data
    if fmt not in FORMATS:
        raise CatalogError(f"not compatible: format {fmt.strip()[:16]}")
    apps = os.path.join(tree, "apps")
    if os.path.islink(apps) or not os.path.isdir(apps):
        raise CatalogError("no apps directory")
    presets = _presets(apps)
    if not presets:
        raise CatalogError("no presets in apps")
    return fmt, presets


def read_meta(directory):
    """KEY -> VALUE of a catalog's CATALOG; {} when there is none."""
    path = os.path.join(directory, "CATALOG")
    if not os.path.lexists(path):
        return {}
    meta = {}
    for line in _read(path).decode("utf-8", "replace").splitlines():
        match = LINE.fullmatch(line)
        if not match or match[1] not in META_KEYS or not VALUE.fullmatch(match[2]):
            raise CatalogError(f"CATALOG: not a line of it: {line[:60]}")
        meta[match[1]] = match[2]
    if meta.get("format") not in FORMATS or meta.get("origin") not in ORIGINS or \
            (meta.get("commit") and not COMMIT.fullmatch(meta["commit"])):
        raise CatalogError("CATALOG: not a catalog this system reads")
    return meta


def read_catalog(directory):
    """NAME -> path of an installed catalog's presets, validated with its
    CATALOG; {} when there is none."""
    if not os.path.lexists(directory):
        return {}
    if os.path.islink(directory) or not os.path.isdir(directory):
        raise CatalogError(f"{directory}: not a directory")
    presets = _presets(directory, ("CATALOG",))
    if not read_meta(directory):
        raise CatalogError(f"{directory}: no CATALOG")
    return {name: os.path.join(directory, name + ".conf") for name in presets}


def _installed(catalog):
    """NAME -> bytes of the installed catalog, for the bases of the merge;
    none from one that cannot be read."""
    try:
        return {name: _read(path) for name, path in read_catalog(catalog).items()}
    except (CatalogError, OSError):
        return {}


def _directory(path):
    """path, made (0755) when it is not there."""
    if not os.path.isdir(path):
        os.makedirs(path, exist_ok=True)
        os.chmod(path, 0o755)


def _write(path, data, mode=0o644):
    """path replaced by data in one step, on the disk before it is."""
    temporary = path + ".tmp"
    with open(temporary, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(temporary, mode)
    os.replace(temporary, path)


def write_catalog(presets, dest, meta):
    """A new catalog at dest (which must not exist): presets, then CATALOG."""
    for key, value in meta.items():
        if key not in META_KEYS or not VALUE.fullmatch(value):
            raise CatalogError(f"CATALOG: {key} cannot be {value!r}")
    os.mkdir(dest)
    os.chmod(dest, 0o755)
    for name, data in presets.items():
        _write(os.path.join(dest, name + ".conf"), data)
    _write(os.path.join(dest, "CATALOG"),
           "".join(f'{key}="{meta[key]}"\n' for key in META_KEYS if key in meta).encode())


def new_meta(fmt, origin, repo="", branch="", commit="", **more):
    if origin not in ORIGINS or (commit and not COMMIT.fullmatch(commit)):
        raise CatalogError(f"not a catalog's origin and commit: {origin} {commit}")
    return dict(format=fmt, origin=origin, repo=repo, branch=branch, commit=commit,
                installed=time.strftime("%Y-%m-%d %H:%M"), **more)


def _exchange(new, current):
    """new and current change places in one step (renameat2's
    RENAME_EXCHANGE): current is never missing or half written. Without a
    current one, new is renamed."""
    if not os.path.lexists(current):
        os.rename(new, current)
        return
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.renameat2(-100, os.fsencode(new), -100, os.fsencode(current), 2) != 0: # AT_FDCWD, RENAME_EXCHANGE
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), current)


def switch(catalog, presets, meta, before=None):
    """presets and meta as the catalog at path catalog, switched in one step;
    before(), once the new one is complete, before the switch. What an
    interrupted switch left (catalog.new) is removed first."""
    _directory(os.path.dirname(catalog))
    staging = catalog + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    write_catalog(presets, staging, meta)
    if before:
        before()
    _exchange(staging, catalog)
    shutil.rmtree(staging, ignore_errors=True) # the one before, after an exchange


def copy_catalog(src, dest):
    """An installed catalog (the live installer's) as a new system's (dest),
    validated and written whole."""
    presets = {name: _read(path) for name, path in read_catalog(src).items()}
    meta = read_meta(src)
    switch(dest, presets, dict(meta, installed=time.strftime("%Y-%m-%d %H:%M")))


def _git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)


def fetch(apps_dir, url, branch, net=lambda command: command):
    """BRANCH of URL into APPS_DIR/repo.git (shallow, as the source's): its
    commit. CatalogError when it cannot be fetched."""
    repo = os.path.join(apps_dir, "repo.git")
    _directory(repo)
    if not os.path.isfile(os.path.join(repo, "HEAD")):
        subprocess.run(["git", "init", "--bare", "--quiet", repo], check=True)
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    try:
        p = subprocess.run(net(["git", "-C", repo, "fetch", "--depth=1", "--quiet", "--", url, branch]),
                           capture_output=True, text=True, env=env, timeout=300)
        why = "" if p.returncode == 0 else ((p.stderr or "").strip().splitlines() or ["git fetch failed"])[-1]
    except subprocess.TimeoutExpired:
        why = "no answer in 300 s"
    if why:
        raise CatalogError(f"could not fetch {url} ({branch}): {why}".replace("\t", " "))
    commit = _git(repo, "rev-parse", "FETCH_HEAD").stdout.strip()
    if not COMMIT.fullmatch(commit):
        raise CatalogError(f"could not fetch {url} ({branch}): no commit")
    return commit


def fetched(apps_dir, commit):
    """Whether repo.git has commit (a plan's, checked before)."""
    return bool(COMMIT.fullmatch(commit or "")) and \
        _git(os.path.join(apps_dir, "repo.git"), "cat-file", "-e", commit + "^{commit}").returncode == 0


def verify(apps_dir, commit, signers):
    """As the source's (locachy-pkg verify_source_commit): when signers (the
    trusted-signers file) is not empty, commit has a good SSH signature of one."""
    if not os.path.isfile(signers) or os.path.getsize(signers) == 0:
        return
    p = _git(os.path.join(apps_dir, "repo.git"), "-c", "gpg.format=ssh",
             "-c", f"gpg.ssh.allowedSignersFile={signers}", "verify-commit", commit)
    if p.returncode != 0:
        raise CatalogError(f"commit {commit[:12]} failed signature verification against {signers}")


def extract(apps_dir, commit, dest):
    """FORMAT and apps/ of commit (in repo.git) into dest, as files: (format,
    NAME -> bytes), validated."""
    repo = os.path.join(apps_dir, "repo.git")
    with subprocess.Popen(["git", "-C", repo, "archive", "--format=tar", commit, "--", "FORMAT", "apps"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as archive:
        tar = subprocess.run(["tar", "-xf", "-", "--no-same-owner", "-C", dest], stdin=archive.stdout,
                             capture_output=True)
        archive.stdout.close()
        error = archive.stderr.read().decode(errors="replace").strip()
    if archive.returncode != 0 or tar.returncode != 0:
        raise CatalogError(f"{commit[:12]}: not an app presets repository (FORMAT, apps): "
                           + (error.splitlines() or ["tar failed"])[-1])
    return validate_tree(dest)


def conf_values(text):
    """KEY -> VALUE of a configuration as locachy-app reads it: the last
    KEY="VALUE" line of each key."""
    values = {}
    for line in text.splitlines():
        match = re.match(r'\s*([A-Za-z0-9_]+)="([^"]*)"', line)
        if match:
            values[match[1]] = match[2]
    return values


def packages(values):
    return " ".join((values.get("package", "") + " " + values.get("extras", "")).split())


def merged(text, base, new):
    """(text, keys): the configuration text with each key it has as its
    base preset had it following the new preset (taken out where that has
    none); its comments, other keys and the user's values as they were."""
    current = conf_values(text)
    changes = {key: new.get(key) for key in sorted(set(base) | set(new))
               if current.get(key) == base.get(key) and new.get(key) != base.get(key)}
    if not changes:
        return text, []
    lines, done = [], set()
    for line in text.splitlines(keepends=True):
        match = re.match(r"\s*([A-Za-z0-9_]+)=", line)
        key = match[1] if match else None
        if key not in changes:
            lines.append(line)
        elif key not in done:
            done.add(key)
            if changes[key] is not None:
                lines.append(f'{key}="{changes[key]}"\n')
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    lines += [f'{key}="{value}"\n' for key, value in changes.items() if key not in done and value is not None]
    return "".join(lines), sorted(changes)


def merges(conf_dir, apps_dir, old, new):
    """(name, path, text, merged text, keys) of each configuration that
    follows a preset of the new catalog: its base the preset it last followed
    (baseline), else the old catalog's (old: NAME -> bytes). A configuration
    the user made (no base), a host program's, and one whose preset is gone
    are left as they are. new: NAME -> values."""
    found = []
    for path in sorted(glob.glob(os.path.join(glob.escape(conf_dir), "*.conf"))):
        name = os.path.basename(path)[:-5]
        if name not in new or os.path.islink(path) or not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except (OSError, UnicodeDecodeError):
            continue
        if conf_values(text).get("host") == "1":
            continue
        try:
            base = _read(os.path.join(apps_dir, "baseline", name + ".conf"))
        except (CatalogError, OSError):
            base = old.get(name)
        if base is None:
            continue
        result, keys = merged(text, conf_values(base.decode("utf-8", "replace")), new[name])
        found.append((name, path, text, result, keys))
    return found


def merge_preview(conf_dir, apps_dir, old, new):
    """(name, keys, packages before, packages after) of each configuration
    Apply would change."""
    return [(name, keys, packages(conf_values(text)), packages(conf_values(result)))
            for name, _, text, result, keys in merges(conf_dir, apps_dir, old, new) if keys]


def compare(apps_dir, commit, conf_dir, signers):
    """commit (in repo.git) against the installed catalog, for the plan: None
    when it is the one installed; else old (its short commit or origin),
    label, and the configurations that would follow it (merge_preview).
    CatalogError when it fails verification or validation."""
    verify(apps_dir, commit, signers)
    catalog = os.path.join(apps_dir, "catalog")
    try:
        meta = read_meta(catalog)
    except (CatalogError, OSError):
        meta = {}
    if meta.get("commit") == commit:
        return None
    with tempfile.TemporaryDirectory(prefix="locachy-apps-check-") as tmp:
        _, presets = extract(apps_dir, commit, tmp)
    new = {name: preset_values(name, data) for name, data in presets.items()}
    subject = _git(os.path.join(apps_dir, "repo.git"), "log", "-1", "--format=%s", commit).stdout.strip()
    return {"old": meta.get("commit", "")[:7] or meta.get("origin") or "none", "commit": commit,
            "label": f"{commit[:7]} ({subject[:48]})" if subject else commit[:7],
            "merges": merge_preview(conf_dir, apps_dir, _installed(catalog), new)}


def check(apps_dir, url, branch, conf_dir, signers, net=lambda command: command):
    """Fetch BRANCH of URL, then compare (see there)."""
    return compare(apps_dir, fetch(apps_dir, url, branch, net), conf_dir, signers)


def apply(apps_dir, commit, conf_dir, signers, repo, branch):
    """The checked commit (in repo.git: never fetched again) as the catalog,
    verified and validated, switched in one step; then the configurations
    that follow its presets, each written in one step, its baseline the new
    preset. A failure before the switch keeps the old catalog. Returns
    [(name, keys)] of the configurations changed."""
    if not fetched(apps_dir, commit):
        raise CatalogError(f"{(commit or '')[:12] or 'no commit'} is not in {apps_dir}/repo.git: check again")
    verify(apps_dir, commit, signers)
    with tempfile.TemporaryDirectory(prefix="locachy-apps-") as tmp:
        fmt, presets = extract(apps_dir, commit, tmp)
    catalog = os.path.join(apps_dir, "catalog")
    old = _installed(catalog)
    new = {name: preset_values(name, data) for name, data in presets.items()}
    todo = merges(conf_dir, apps_dir, old, new)
    baseline = os.path.join(apps_dir, "baseline")

    def keep_bases():
        # What each configuration followed stays its base, should the
        # merges after the switch not all be done.
        _directory(baseline)
        for name, *_ in todo:
            path = os.path.join(baseline, name + ".conf")
            if name in old and not os.path.exists(path):
                _write(path, old[name])
    switch(catalog, presets, new_meta(fmt, "repository", repo, branch, commit), keep_bases)
    changed = []
    for name, path, _, result, keys in todo:
        if keys:
            _write(path, result.encode(), stat.S_IMODE(os.stat(path).st_mode))
            changed.append((name, keys))
        _write(os.path.join(baseline, name + ".conf"), presets[name])
    return changed


def bootstrap(legacy_dir, apps_dir=APPS_DIR, source_commit_file=SOURCE_COMMIT_FILE):
    """The first system update after the split: the old system's app presets
    (legacy_dir, host programs left out) become the catalog, until one from
    the apps repository replaces it. A catalog there is never touched."""
    catalog = os.path.join(apps_dir, "catalog")
    if os.path.lexists(catalog):
        return "app presets: the catalog is there; left as it is"
    presets = {}
    for path in sorted(glob.glob(os.path.join(glob.escape(legacy_dir), "*.conf"))):
        data = _read(path)
        name = os.path.basename(path)[:-5]
        if conf_values(data.decode("utf-8", "replace")).get("host") == "1":
            continue
        if not NAME.fullmatch(name):
            raise CatalogError(f"{name}.conf: not a preset's name")
        preset_values(name, data)
        presets[name] = data
    if not presets:
        return "app presets: none to keep from before"
    try:
        with open(source_commit_file, encoding="utf-8") as f:
            system = f.read().strip()
    except OSError:
        system = ""
    switch(catalog, presets, new_meta(FORMATS[-1], "bootstrap", system_commit=system if COMMIT.fullmatch(system) else ""))
    return f"app presets: the {len(presets)} of the system before are the catalog until one from the apps repository"


def main(argv):
    try:
        if argv[1:2] == ["validate"] and len(argv) == 3:
            fmt, presets = validate_tree(argv[2])
            print(f"OK {len(presets)} app presets, format {fmt}")
        elif argv[1:2] == ["catalog"] and len(argv) == 8:
            tree, dest, origin, repo, branch, commit = argv[2:]
            fmt, presets = validate_tree(tree)
            write_catalog(presets, dest, new_meta(fmt, origin, repo, branch, commit))
        elif argv[1:2] == ["bootstrap"] and len(argv) == 3:
            print(bootstrap(argv[2]))
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except (CatalogError, OSError) as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
