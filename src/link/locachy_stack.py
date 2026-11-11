"""locachy_stack: LOCachy's stacked repositories, resolved by one rule.

locachy-repos lists LOCachy's repositories as layers: CachyOS's optimised
builds, then ALHP's, then [cachyos], then Arch's. pacman alone takes a package
from the first repository that has it. So a layer that has not yet rebuilt a
new release keeps the old one, and its sonames, in front of the newer one below
it: ALHP's openssl 3 and the tor built on it, above Arch's openssl 4.

The stack means something else. Of the same release, the optimised build wins;
of different versions, the newer one; once a layer has caught up, it is on top
again. Rebuild layers extend the pkgrel of the release they rebuild (3.6.5-1.1
for Arch's 3.6.5-1). libalpm's own version comparison, with the layer order
breaking only an exact tie, is therefore that rule.

merge() reads the databases as pacman fetched them and writes a second set in
which each repository holds only the packages it wins; pacman's own rule then
picks exactly those. The fetched set stays as it is, so that pacman's next
refresh still compares it with the mirrors. A database that loses nothing is
copied with its signature and its time. One that is rewritten gets none:
SigLevel's DatabaseOptional reads it unsigned, and it was verified when it was
fetched. It is dated at the epoch, so that a refresh over it fetches it whole
(merge).

LOCachy reads the repositories only through such a set:
- the update plan (locachy-pkg), and with it the transaction snapshot and the
  databases published to the host;
- the fresh root (make-root.sh);
- the repository check (locachy-repos).

record() notes the databases LOCachy published to the host. check() tells
locachy-pacman-guard when a pacman -Sy typed by hand has replaced them with the
raw ones, on which pacman resolves the stack first-repository-first.

  python3 -I locachy_stack.py merge --config FILE --from SYNC_DIR --to SYNC_DIR
  python3 -I locachy_stack.py record --sync SYNC_DIR --file RECORD
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
RECORD = "/var/lib/locachy/sync-published.json"
_vercmp = None


def vercmp(a, b):
    """libalpm's alpm_pkg_vercmp, the comparison pacman itself makes: -1, 0 or 1."""
    global _vercmp
    if _vercmp is None:
        try:
            library = ctypes.CDLL(ctypes.util.find_library("alpm") or "libalpm.so")
        except OSError as e:
            raise RuntimeError(f"libalpm cannot be loaded: {e}") from e
        function = library.alpm_pkg_vercmp
        function.restype = ctypes.c_int
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        _vercmp = function
    result = _vercmp(a.encode(), b.encode())
    return (result > 0) - (result < 0)


def repositories(config):
    """The repositories of a pacman.conf, in its order (pacman-conf's list)."""
    out = subprocess.run(["pacman-conf", "--config", config, "--repo-list"],
                         check=True, capture_output=True, text=True).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


def desc_fields(path):
    """%NAME% and %VERSION% of a sync database entry's desc file."""
    fields, key = {}, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if len(line) > 2 and line[0] == "%" and line[-1] == "%":
                key = line[1:-1]
            elif not line:
                key = None
            elif key in ("NAME", "VERSION") and key not in fields:
                fields[key] = line
    return fields


def entries(db, tree):
    """A repository database unpacked into tree: {entry directory: (name, version)}.
    An entry without both fields is left in the tree and never removed.
    The entries' owners are not restored: they mean nothing to pacman, and root
    in a user namespace cannot set the uid a repository recorded (1000)."""
    os.makedirs(tree)
    subprocess.run(["bsdtar", "--no-same-owner", "-xf", db, "-C", tree], check=True)
    found = {}
    for entry in sorted(os.listdir(tree)):
        desc = os.path.join(tree, entry, "desc")
        if os.path.isfile(desc):
            fields = desc_fields(desc)
            if "NAME" in fields and "VERSION" in fields:
                found[entry] = (fields["NAME"], fields["VERSION"])
    return found


def winners(layers):
    """{name: (repository, version)} over [(repository, entries)] in pacman.conf's
    order: the newest version, an exact tie to the higher layer."""
    best = {}
    for repo, found in layers:
        for name, version in found.values():
            held = best.get(name)
            if held is None or vercmp(version, held[1]) > 0:
                best[name] = (repo, version)
    return best


def _place(source, target):
    """Copy source over target by rename, so a reader never sees half a file.
    The copy keeps source's time: pacman's next refresh compares it with the
    mirror's (merge)."""
    temporary = target + ".locachy-new"
    shutil.copyfile(source, temporary)
    os.chmod(temporary, 0o644)
    st = os.stat(source)
    os.utime(temporary, ns=(st.st_atime_ns, st.st_mtime_ns))
    os.replace(temporary, target)


def _unlink(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def merge(source, target, repos):
    """Write the sync directory target from source, as the rule resolves it.
    Each database is replaced by rename. A rewritten database loses its
    signature before it is put in place, and a copied one gets its signature
    after it, so a reader never pairs a database with another's signature.
    Databases of repositories no longer listed are removed.
    Returns {repository: (kept, removed)}."""
    source, target = os.path.abspath(source), os.path.abspath(target)
    os.makedirs(target, mode=0o755, exist_ok=True)
    work = tempfile.mkdtemp(prefix=".locachy-stack-", dir=os.path.dirname(target))
    try:
        layers = []
        for repo in repos:
            db = os.path.join(source, repo + ".db")
            if os.path.isfile(db):
                layers.append((repo, entries(db, os.path.join(work, repo))))
        best = winners(layers)
        summary = {}
        written = set()
        for repo, found in layers:
            db = os.path.join(source, repo + ".db")
            out = os.path.join(target, repo + ".db")
            losing = sorted(e for e, (name, _) in found.items() if best[name][0] != repo)
            summary[repo] = (len(found) - len(losing), len(losing))
            written.update((repo + ".db", repo + ".db.sig"))
            if not losing:
                # Unchanged: the database and its signature go over as they are.
                # In place (source and target the same), there is nothing to copy.
                if os.path.abspath(db) != os.path.abspath(out):
                    _place(db, out)
                    if os.path.isfile(db + ".sig"):
                        _place(db + ".sig", out + ".sig")
                    else:
                        _unlink(out + ".sig")
                continue
            tree = os.path.join(work, repo)
            for entry in losing:
                shutil.rmtree(os.path.join(tree, entry))
            rewritten = os.path.join(work, repo + ".db")
            subprocess.run(["bsdtar", "-czf", rewritten, "-C", tree, "-T", "-"],
                           input="".join(e + "\n" for e in sorted(os.listdir(tree))),
                           text=True, check=True)
            # Time 0. pacman's refresh downloads a database only when the
            # mirror's is newer than the file, and for one that is not newer
            # still fetches a signature the file lacks: the original's, which
            # this rewritten database never matches, and every later pacman
            # command fails on it. Older than any mirror, a plain pacman -Sy
            # downloads it again whole, with its signature (and the record
            # then tells locachy-pacman-guard it was replaced).
            os.utime(rewritten, (0, 0))
            _unlink(out + ".sig")
            _place(rewritten, out)
        for name in os.listdir(target):
            # A copy an interrupted merge left, and databases of repositories
            # no longer listed.
            if name.endswith(".locachy-new") or (
                    (name.endswith(".db") or name.endswith(".db.sig")) and name not in written):
                _unlink(os.path.join(target, name))
        return summary
    finally:
        shutil.rmtree(work, ignore_errors=True)


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def record(sync, path=RECORD):
    """Note the sha256 of each repository database in sync, as LOCachy published it."""
    data = {name: digest(os.path.join(sync, name)) for name in sorted(os.listdir(sync))
            if name.endswith(".db") and os.path.isfile(os.path.join(sync, name))}
    os.makedirs(os.path.dirname(path), mode=0o755, exist_ok=True)
    temporary = path + ".locachy-new"
    with open(temporary, "w", encoding="utf-8") as f:
        json.dump(data, f, sort_keys=True, indent=1)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.chmod(temporary, 0o644)
    os.replace(temporary, path)


def check(sync, repos, path=RECORD):
    """None when each listed repository's database in sync is the one LOCachy
    published, else what differs."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return "LOCachy has not published its merged repository databases here yet"
    except (OSError, ValueError) as e:
        return f"the record of published repository databases is unreadable ({e})"
    for repo in repos:
        name = repo + ".db"
        db = os.path.join(sync, name)
        if os.path.isfile(db) and data.get(name) != digest(db):
            return f"{name} is not the database LOCachy published"
    return None


def main(argv):
    parser = argparse.ArgumentParser(prog="locachy_stack.py")
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("merge", help="write a sync directory resolved newest-first")
    m.add_argument("--config", required=True)
    m.add_argument("--from", dest="source", required=True)
    m.add_argument("--to", dest="target", required=True)
    r = sub.add_parser("record", help="note the databases of a published sync directory")
    r.add_argument("--sync", required=True)
    r.add_argument("--file", default=RECORD)
    args = parser.parse_args(argv[1:])
    try:
        if args.command == "merge":
            summary = merge(args.source, args.target, repositories(args.config))
            for repo, (kept, removed) in summary.items():
                print(f"locachy-stack: {repo}: {kept} packages, {removed} left to another layer")
        else:
            record(args.sync, args.file)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
        print(f"locachy-stack: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
