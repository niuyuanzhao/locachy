#!/usr/bin/env python3
"""locachy_stack without the network: the newest version of each package wins
across the layers, an exact tie goes to the higher layer, libalpm decides the
order, and the merged database is what pacman then resolves.

  python3 -B tools/test-stack-merge.py [SCRATCH_PARENT]
  unshare --user --map-root-user python3 -B tools/test-stack-merge.py [SCRATCH_PARENT]

Fixture databases are built as pacman builds them (a gzipped tar of
NAME-VERSION/desc entries). The pacman checks run only when pacman is here.
Run it as root too (a user namespace's will do): root's extraction is the one
that would try to restore the entries' owners.
"""
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("locachy_stack", str(SOURCE / "link/locachy_stack.py"))
spec = importlib.util.spec_from_loader(loader.name, loader)
stack = importlib.util.module_from_spec(spec)
loader.exec_module(stack)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
HAVE_PACMAN = shutil.which("pacman") is not None and shutil.which("pacman-conf") is not None


def desc(name, version):
    """A sync database entry with the fields a real one has (pacman -Sp needs them)."""
    return (f"%FILENAME%\n{name}-{version}-x86_64.pkg.tar.zst\n\n%NAME%\n{name}\n\n"
            f"%BASE%\n{name}\n\n%VERSION%\n{version}\n\n%DESC%\nfixture\n\n"
            f"%CSIZE%\n1024\n\n%ISIZE%\n4096\n\n%SHA256SUM%\n{'0' * 64}\n\n"
            f"%ARCH%\nx86_64\n\n%BUILDDATE%\n1700000000\n\n%PACKAGER%\nfixture\n\n").encode()


def write_db(path, packages):
    """packages: [(name, version)] as a repository database: NAME-VERSION/desc
    entries, owned by uid 1000 as CachyOS's are (root in a user namespace
    cannot restore that owner; the merge must not try)."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        for name, version in packages:
            data = desc(name, version)
            info = tarfile.TarInfo(f"{name}-{version}/desc")
            info.size = len(data)
            info.uid = info.gid = 1000
            tar.addfile(info, io.BytesIO(data))
    Path(path).write_bytes(buffer.getvalue())


def db_packages(path):
    found = []
    with tarfile.open(path, "r:gz") as tar:
        for member in tar.getmembers():
            if member.name.endswith("/desc"):
                text = tar.extractfile(member).read().decode()
                name = text.split("%NAME%\n", 1)[1].split("\n", 1)[0]
                version = text.split("%VERSION%\n", 1)[1].split("\n", 1)[0]
                found.append((name, version))
    return sorted(found)


class Merge(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix="stack-test-", dir=SCRATCH))
        self.source = self.base / "source"
        self.target = self.base / "target"
        self.source.mkdir()
        self.target.mkdir()

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def layers(self, **repos):
        for repo, packages in repos.items():
            write_db(self.source / f"{repo}.db", packages)
            (self.source / f"{repo}.db.sig").write_bytes(b"signature of " + repo.encode())

    def merged(self, order):
        summary = stack.merge(str(self.source), str(self.target), order)
        return {repo: db_packages(self.target / f"{repo}.db") for repo in order}, summary

    def test_newer_lower_layer_wins(self):
        self.layers(top=[("openssl", "3.6.5-1.1")], bottom=[("openssl", "4.1.0-1")])
        got, _ = self.merged(["top", "bottom"])
        self.assertEqual(got["top"], [])
        self.assertEqual(got["bottom"], [("openssl", "4.1.0-1")])

    def test_exact_tie_goes_to_the_higher_layer(self):
        self.layers(top=[("zlib", "1.3.2-1")], bottom=[("zlib", "1.3.2-1")])
        got, _ = self.merged(["top", "bottom"])
        self.assertEqual(got["top"], [("zlib", "1.3.2-1")])
        self.assertEqual(got["bottom"], [])

    def test_rebuild_suffix_is_newer_than_its_release(self):
        # ALHP's rebuilds carry .1: 3.6.5-1.1 is newer than 3.6.5-1, so the layer above wins.
        self.layers(top=[("coreutils", "9.12-2.1")], bottom=[("coreutils", "9.12-2")])
        got, _ = self.merged(["top", "bottom"])
        self.assertEqual(got["top"], [("coreutils", "9.12-2.1")])
        self.assertEqual(got["bottom"], [])

    def test_older_upper_layer_is_replaced_by_newer_lower_one(self):
        self.layers(top=[("tor", "0.4.9.14-1.1")], bottom=[("tor", "0.4.9.16-1")])
        got, _ = self.merged(["top", "bottom"])
        self.assertEqual(got["top"], [])
        self.assertEqual(got["bottom"], [("tor", "0.4.9.16-1")])

    def test_epoch_beats_a_higher_plain_version(self):
        self.layers(top=[("vim", "9.2-1")], bottom=[("vim", "1:9.1-1")])
        got, _ = self.merged(["top", "bottom"])
        self.assertEqual(got["bottom"], [("vim", "1:9.1-1")])
        self.assertEqual(got["top"], [])

    def test_package_in_one_layer_stays_there(self):
        self.layers(top=[("libfoo", "1-1.1")], bottom=[("libbar", "2-1")])
        got, _ = self.merged(["top", "bottom"])
        self.assertEqual(got["top"], [("libfoo", "1-1.1")])
        self.assertEqual(got["bottom"], [("libbar", "2-1")])

    def test_each_name_is_in_exactly_one_layer_afterwards(self):
        self.layers(a=[("p", "2-1"), ("q", "1-1.1")], b=[("p", "1-1"), ("q", "1-1"), ("r", "5-1")],
                    c=[("p", "2-1"), ("r", "4-1")])
        got, _ = self.merged(["a", "b", "c"])
        held = sorted(name for packages in got.values() for name, _ in packages)
        self.assertEqual(held, ["p", "q", "r"])
        self.assertEqual(dict(got["a"]), {"p": "2-1", "q": "1-1.1"})
        self.assertEqual(got["b"], [("r", "5-1")])
        self.assertEqual(got["c"], [])

    def test_unchanged_database_keeps_its_signature_and_time(self):
        # The mirror's time, as pacman set it: a refresh then finds it up to date.
        self.layers(only=[("x", "1-1")])
        for name in ("only.db", "only.db.sig"):
            os.utime(self.source / name, (1700000000, 1700000000))
        self.merged(["only"])
        self.assertEqual((self.target / "only.db.sig").read_bytes(), b"signature of only")
        for name in ("only.db", "only.db.sig"):
            self.assertEqual((self.target / name).stat().st_mtime, 1700000000)

    def test_rewritten_database_has_no_signature_and_time_0(self):
        # Older than any mirror's copy: a plain pacman -Sy fetches it again
        # whole, never pairing it with the mirror's signature.
        self.layers(top=[("p", "1-1.1")], bottom=[("p", "1-1"), ("keep", "3-1")])
        (self.target / "bottom.db.sig").write_bytes(b"signature of an older bottom")
        self.merged(["top", "bottom"])
        self.assertFalse((self.target / "bottom.db.sig").exists())
        self.assertTrue((self.target / "top.db.sig").exists())
        self.assertEqual((self.target / "bottom.db").stat().st_mtime, 0)
        self.assertNotEqual((self.target / "top.db").stat().st_mtime, 0)

    def test_in_place_merge_keeps_unchanged_signatures_and_drops_losers(self):
        self.layers(top=[("p", "1-1.1")], bottom=[("p", "1-1"), ("keep", "3-1")])
        summary = stack.merge(str(self.source), str(self.source), ["top", "bottom"])
        self.assertEqual(summary["top"], (1, 0))
        self.assertEqual(summary["bottom"], (1, 1))
        self.assertEqual(db_packages(self.source / "bottom.db"), [("keep", "3-1")])
        self.assertEqual((self.source / "top.db.sig").read_bytes(), b"signature of top")

    def test_removed_repository_database_is_dropped_from_target(self):
        self.layers(keep=[("a", "1-1")])
        write_db(self.target / "gone.db", [("z", "1-1")])
        stack.merge(str(self.source), str(self.target), ["keep"])
        self.assertFalse((self.target / "gone.db").exists())

    def test_copy_an_interrupted_merge_left_is_removed(self):
        self.layers(keep=[("a", "1-1")])
        (self.target / "keep.db.locachy-new").write_bytes(b"half written")
        self.merged(["keep"])
        self.assertFalse((self.target / "keep.db.locachy-new").exists())
        self.assertEqual(db_packages(self.target / "keep.db"), [("a", "1-1")])

    def test_record_and_check(self):
        self.layers(keep=[("a", "1-1")])
        self.merged(["keep"])
        record = self.base / "published.json"
        stack.record(str(self.target), str(record))
        self.assertIsNone(stack.check(str(self.target), ["keep"], str(record)))
        write_db(self.target / "keep.db", [("a", "2-1")])
        self.assertIn("keep.db is not the database LOCachy published",
                      stack.check(str(self.target), ["keep"], str(record)))
        self.assertIn("not published", stack.check(str(self.target), ["keep"], str(self.base / "absent.json")))
        json.loads(record.read_text())

    @unittest.skipUnless(HAVE_PACMAN, "pacman is not installed here")
    def test_pacman_resolves_the_merged_databases(self):
        # As LOCachy uses it: the merged set is a dbpath's sync directory, read
        # by pacman directly (no -Sy on it).
        self.layers(top=[("app", "2-1.1")], bottom=[("app", "2-1"), ("lib", "1-1")])
        conf = self.base / "pacman.conf"
        conf.write_text("[options]\nArchitecture = x86_64\nSigLevel = Never\n"
                        "[top]\nServer = file:///nonexistent\n[bottom]\nServer = file:///nonexistent\n")
        dbpath = self.base / "db"
        (dbpath / "local").mkdir(parents=True)
        stack.merge(str(self.source), str(dbpath / "sync"), stack.repositories(str(conf)))
        pacman = ["pacman", "--config", str(conf), "--dbpath", str(dbpath), "--logfile", "/dev/null"]
        listed = subprocess.run(pacman + ["-Sl"], capture_output=True, text=True)
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn("top app 2-1.1", listed.stdout)
        self.assertNotIn("bottom app", listed.stdout)
        self.assertIn("bottom lib 1-1", listed.stdout)
        picked = subprocess.run(pacman + ["-Sp", "--print-format", "%r/%n %v", "app", "lib"],
                                capture_output=True, text=True)
        self.assertEqual(picked.returncode, 0, picked.stderr)
        self.assertEqual(sorted(picked.stdout.split("\n")[:-1]), ["bottom/lib 1-1", "top/app 2-1.1"])



if __name__ == "__main__":
    unittest.main(verbosity=2)
