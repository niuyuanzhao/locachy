#!/usr/bin/env python3
"""Drive archive-worker (progs/archive-worker.c) through archive-proto.h on
real archives in a scratch directory: listing, Ark's extraction rules and
questions, what a hostile archive cannot do, passwords, writing, adding,
removing, volumes, testing and hashes; with 7-Zip when a worker built with
-DARCHIVE_7Z_TEST_PATH is given too.
  python3 -I tools/test-archive-worker.py WORKER SCRATCH_PARENT [WORKER_7Z]
"""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

WORKER = sys.argv.pop(1)
SCRATCH = sys.argv.pop(1)
WORKER_7Z = sys.argv.pop(1) if len(sys.argv) > 1 else None


def unescape(field):
    out, i = [], 0
    while i < len(field):
        c = field[i]
        if c == "\\" and i + 1 < len(field):
            n = field[i + 1]
            if n == "t":
                out.append("\t")
            elif n == "n":
                out.append("\n")
            elif n == "x":
                out.append(chr(int(field[i + 2:i + 4], 16)))
                i += 2
            else:
                out.append(n)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def run(args, answers=(), worker=None, home=None, first_line=None):
    """The worker's messages (type, fields), its status, and the questions;
    first_line: what it reads first without asking (create -P's password)."""
    env = {"PATH": "/usr/bin", "LANG": "C.UTF-8", "HOME": home or os.environ.get("HOME", "/")}
    p = subprocess.Popen([worker or WORKER] + [str(a) for a in args], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    answers = list(answers)
    msgs, asked = [], []
    if first_line is not None:
        p.stdin.write(first_line + "\n")
        p.stdin.flush()
    for line in p.stdout:
        f = line.rstrip("\n").split("\t")
        msg = (f[0], [unescape(x) for x in f[1:]])
        msgs.append(msg)
        if f[0] == "Q":
            asked.append(msg[1])
            p.stdin.write((answers.pop(0) if answers else "c") + "\n")
            p.stdin.flush()
    p.stdin.close()
    p.wait(timeout=60)
    return msgs, p.returncode, asked


def entries(msgs):
    return {m[1][0]: m[1] for m in msgs if m[0] == "E"}


def first(msgs, kind):
    return next((m[1] for m in msgs if m[0] == kind), None)


class Worker(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="archive-", dir=SCRATCH))
        self.src = self.tmp / "src"
        (self.src / "docs/deep").mkdir(parents=True)
        (self.src / "docs/a.txt").write_text("alpha\n")
        (self.src / "docs/deep/b.txt").write_text("beta\n")
        (self.src / "top.txt").write_text("top\n")
        os.symlink("docs/a.txt", self.src / "link")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def tar(self, name, members, mode="w:gz"):
        path = self.tmp / name
        with tarfile.open(path, mode) as t:
            for m in members:
                t.add(self.src / m, arcname=m)
        return path

    def test_list_reports_entries_and_format(self):
        arc = self.tar("multi.tar.gz", ["docs", "top.txt", "link"])
        msgs, rc, _ = run(["list", arc])
        self.assertEqual(rc, 0, msgs)
        e = entries(msgs)
        self.assertEqual(e["docs"][1], "d")
        self.assertEqual(e["docs/deep/b.txt"][1:3], ["f", "5"])
        self.assertEqual(e["link"][1], "l")
        self.assertEqual(e["link"][7], "docs/a.txt")
        fmt = first(msgs, "F")
        self.assertIn("pax", fmt[0].lower())
        self.assertEqual(fmt[1], "gzip")
        self.assertEqual(fmt[2:6], ["libarchive", "0", "0", "1"])

    def test_extract_here_follows_arks_subfolder_rules(self):
        multi = self.tar("multi.tar.gz", ["docs", "top.txt"])
        single = self.tar("single.tar.gz", ["docs"])
        one = self.tar("one.tar.gz", ["top.txt"])
        dest = self.tmp / "out"
        for arc in (multi, single, one):
            msgs, rc, _ = run(["extract", "-p", "-s", arc, dest])
            self.assertEqual(rc, 0, msgs)
        self.assertTrue((dest / "multi/docs/deep/b.txt").is_file())
        self.assertTrue((dest / "multi/top.txt").is_file())
        self.assertTrue((dest / "docs/a.txt").is_file())
        self.assertTrue((dest / "top.txt").is_file())
        # The folder is there now: into "docs (1)", as KIO names it.
        msgs, rc, _ = run(["extract", "-p", "-s", single, dest])
        self.assertEqual(first(msgs, "D")[0], str(dest / "docs (1)"))
        self.assertTrue((dest / "docs (1)/docs/deep/b.txt").is_file())
        msgs, rc, _ = run(["extract", "-p", "-s", multi, dest])
        self.assertTrue((dest / "multi (1)/top.txt").is_file())

    def test_selected_entries_paths_and_root_node(self):
        arc = self.tar("multi.tar.gz", ["docs", "top.txt"])
        dest = self.tmp / "sel"
        msgs, rc, _ = run(["extract", "-p", "-b", "docs", arc, dest, "docs/deep"])
        self.assertEqual(rc, 0, msgs)
        self.assertEqual(sorted(str(p.relative_to(dest)) for p in dest.rglob("*")), ["deep", "deep/b.txt"])
        flat = self.tmp / "flat"
        run(["extract", arc, flat, "docs"])
        self.assertEqual(sorted(p.name for p in flat.iterdir()), ["a.txt", "b.txt"])

    def test_overwrite_question(self):
        arc = self.tar("one.tar.gz", ["top.txt"])
        dest = self.tmp / "q"
        dest.mkdir()
        (dest / "top.txt").write_text("mine\n")
        msgs, rc, asked = run(["extract", "-p", arc, dest], ["s"])
        self.assertEqual(asked, [["exists", "top.txt", "0"]])
        self.assertEqual((dest / "top.txt").read_text(), "mine\n")
        run(["extract", "-p", arc, dest], ["rcopy.txt"])
        self.assertEqual((dest / "copy.txt").read_text(), "top\n")
        run(["extract", "-p", arc, dest], ["o"])
        self.assertEqual((dest / "top.txt").read_text(), "top\n")
        msgs, rc, _ = run(["extract", "-p", arc, dest], ["c"])
        self.assertEqual(rc, 2)

    def test_hostile_paths_stay_inside(self):
        path = self.tmp / "evil.tar"
        outside = self.tmp / "outside"
        outside.mkdir()
        with tarfile.open(path, "w") as t:
            for name, data in (("../escape.txt", b"x"), (str(self.tmp / "abs.txt"), b"y")):
                info = tarfile.TarInfo(name)
                info.size = len(data)
                t.addfile(info, io.BytesIO(data))
            link = tarfile.TarInfo("ln")
            link.type = tarfile.SYMTYPE
            link.linkname = str(outside)
            t.addfile(link)
            info = tarfile.TarInfo("ln/through.txt")
            info.size = 1
            t.addfile(info, io.BytesIO(b"z"))
        dest = self.tmp / "evil-out"
        msgs, rc, _ = run(["extract", "-p", path, dest])
        self.assertEqual(rc, 1, msgs)
        self.assertFalse((self.tmp / "escape.txt").exists())
        self.assertEqual(list(outside.iterdir()), [])
        # An absolute path is taken as one inside the archive (Ark).
        self.assertTrue((dest / str(self.tmp / "abs.txt").lstrip("/")).is_file())
        self.assertTrue(any(m[0] == "W" and m[1][0] == "ln/through.txt" for m in msgs))

    def test_compressed_file_is_one_entry(self):
        path = self.tmp / "notes.txt.gz"
        subprocess.run(["sh", "-c", 'printf "hello\\n" | gzip > "$1"', "sh", str(path)], check=True)
        msgs, rc, _ = run(["list", path])
        self.assertIn("notes.txt", entries(msgs))
        dest = self.tmp / "raw"
        run(["extract", "-p", path, dest])
        self.assertEqual((dest / "notes.txt").read_text(), "hello\n")

    def test_create_add_delete_and_levels(self):
        for name, level in (("n.tar.zst", 19), ("n.zip", 0), ("n.7z", 9), ("n.tar", -1)):
            out = self.tmp / name
            args = ["create"] + (["-l", level] if level >= 0 else []) + [out, self.src, "docs", "top.txt", "link"]
            msgs, rc, _ = run(args)
            self.assertEqual(rc, 0, (name, msgs))
            got = entries(run(["list", out])[0])
            self.assertIn("docs/deep/b.txt", got, name)
            self.assertEqual(got["link"][1], "l", name)
        self.assertEqual(run(["create", self.tmp / "n.zip", self.src, "top.txt"])[1], 1)
        self.assertEqual(oct((self.tmp / "n.zip").stat().st_mode & 0o777), "0o644")
        self.assertEqual(run(["create", "-l", "30", self.tmp / "x.tar.gz", self.src, "top.txt"])[1], 1)
        arc = self.tmp / "n.tar.zst"
        (self.tmp / "new.txt").write_text("new\n")
        msgs, rc, _ = run(["add", "-i", "docs/deep", arc, self.tmp, "new.txt"])
        self.assertEqual(rc, 0, msgs)
        got = entries(run(["list", arc])[0])
        self.assertIn("docs/deep/new.txt", got)
        self.assertEqual(first(run(["list", arc])[0], "F")[1], "zstd")
        msgs, rc, _ = run(["delete", arc, "docs/deep"])
        self.assertEqual(rc, 0, msgs)
        got = entries(run(["list", arc])[0])
        self.assertNotIn("docs/deep/b.txt", got)
        self.assertIn("docs/a.txt", got)
        self.assertEqual(run(["delete", arc, "nothing"])[1], 1)
        self.assertEqual([p.name for p in self.tmp.iterdir() if p.name.startswith(".")], [])

    def test_encrypted_zip_asks_for_the_password(self):
        arc = self.tmp / "secret.zip"
        msgs, rc, _ = run(["create", "-P", arc, self.src, "top.txt"], first_line="pw-123")
        self.assertEqual(rc, 0, msgs)
        self.assertEqual(entries(run(["list", arc])[0])["top.txt"][8], "1")
        with zipfile.ZipFile(arc) as z:
            self.assertTrue(z.infolist()[0].flag_bits & 1)
        dest = self.tmp / "dec"
        msgs, rc, asked = run(["extract", "-p", arc, dest], ["pwrong", "ppw-123"])
        self.assertEqual(rc, 0, msgs)
        self.assertEqual(asked, [["password"], ["password", "again"]])
        self.assertEqual((dest / "top.txt").read_text(), "top\n")
        msgs, rc, _ = run(["extract", "-p", arc, self.tmp / "dec2"], ["c"])
        self.assertEqual(rc, 3)
        self.assertFalse((self.tmp / "dec2/top.txt").exists())
        self.assertEqual(run(["test", arc], ["ppw-123"])[1], 0)
        # Added files are encrypted with it too.
        (self.tmp / "more.txt").write_text("more\n")
        self.assertEqual(run(["add", arc, self.tmp, "more.txt"], ["ppw-123"])[1], 0)
        self.assertEqual(entries(run(["list", arc])[0])["more.txt"][8], "1")

    def test_volumes_written_and_read(self):
        big = self.src / "big.bin"
        big.write_bytes(os.urandom(300000))
        arc = self.tmp / "vol.zip"
        msgs, rc, _ = run(["create", "-l", "0", "-v", "100000", arc, self.src, "big.bin"])
        self.assertEqual(rc, 0, msgs)
        self.assertEqual(first(msgs, "D")[0], str(arc) + ".001")
        parts = sorted(p.name for p in self.tmp.glob("vol.zip.*"))
        self.assertEqual(parts, ["vol.zip.001", "vol.zip.002", "vol.zip.003", "vol.zip.004"])
        msgs, rc, _ = run(["list", self.tmp / "vol.zip.001"])
        self.assertEqual(first(msgs, "F")[5], "4")
        self.assertEqual(first(msgs, "F")[4], "1")
        dest = self.tmp / "vol-out"
        self.assertEqual(run(["extract", "-p", self.tmp / "vol.zip.001", dest])[1], 0)
        self.assertEqual((dest / "big.bin").read_bytes(), big.read_bytes())

    def test_damage_found_by_test(self):
        arc = self.tmp / "d.zip"
        with zipfile.ZipFile(arc, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("data.txt", "0123456789" * 2000)
        self.assertEqual(run(["test", arc])[1], 0)
        raw = bytearray(arc.read_bytes())
        raw[60] ^= 0xFF
        arc.write_bytes(raw)
        msgs, rc, _ = run(["test", arc])
        self.assertEqual(rc, 1, msgs)
        self.assertTrue(any(m[0] == "W" for m in msgs))

    def test_hash_and_remove(self):
        arc = self.tar("h.tar.gz", ["top.txt"])
        data = arc.read_bytes()
        h = first(run(["hash", arc])[0], "H")
        self.assertEqual(h, [hashlib.md5(data).hexdigest(), hashlib.sha1(data).hexdigest(),
                             hashlib.sha256(data).hexdigest()])
        home = self.tmp / "home"
        preview = home / ".cache/locachy/archive/p1"
        preview.mkdir(parents=True)
        (preview / "f").write_text("x")
        self.assertEqual(run(["remove", self.src], home=str(home))[1], 1)
        self.assertTrue(self.src.exists())
        self.assertEqual(run(["remove", preview], home=str(home))[1], 0)
        self.assertFalse(preview.exists())

    @unittest.skipUnless(WORKER_7Z, "no worker built with a 7-Zip")
    def test_7zip_for_encrypted_7z(self):
        arc = self.tmp / "s.7z"
        msgs, rc, _ = run(["create", "-P", "-H", arc, self.src, "docs", "top.txt"], first_line="pw7",
                           worker=WORKER_7Z)
        self.assertEqual(rc, 0, msgs)
        # Without 7-Zip the encrypted file list cannot be read.
        msgs, rc, _ = run(["list", arc])
        self.assertEqual(rc, 1)
        msgs, rc, asked = run(["list", arc], ["pbad", "ppw7"], worker=WORKER_7Z)
        self.assertEqual(rc, 0, msgs)
        self.assertEqual(asked, [["password"], ["password", "again"]])
        self.assertIn("docs/deep/b.txt", entries(msgs))
        self.assertEqual(first(msgs, "F")[2:5], ["7-Zip", "1", "0"])
        dest = self.tmp / "s-out"
        (dest / "s").mkdir(parents=True)
        msgs, rc, _ = run(["extract", "-p", "-s", arc, dest], ["ppw7"], worker=WORKER_7Z)
        self.assertEqual(rc, 0, msgs)
        self.assertEqual((dest / "s (1)/docs/deep/b.txt").read_text(), "beta\n")
        self.assertEqual([p.name for p in (dest / "s (1)").iterdir() if p.name.startswith(".")], [])
        self.assertEqual(run(["test", arc], ["ppw7"], worker=WORKER_7Z)[1], 0)
        (self.tmp / "add.txt").write_text("added\n")
        msgs, rc, _ = run(["add", "-i", "docs", arc, self.tmp, "add.txt"], ["ppw7"], worker=WORKER_7Z)
        self.assertEqual(rc, 0, msgs)
        msgs, rc, _ = run(["delete", arc, "top.txt"], ["ppw7"], worker=WORKER_7Z)
        self.assertEqual(rc, 0, msgs)
        got = entries(run(["list", arc], ["ppw7"], worker=WORKER_7Z)[0])
        self.assertIn("docs/add.txt", got)
        self.assertNotIn("top.txt", got)
        # Still encrypted: libarchive still cannot read its list.
        self.assertEqual(run(["list", arc])[1], 1)


if __name__ == "__main__":
    unittest.main()
