#!/usr/bin/env python3
"""Focused C/Python/CLI configuration transaction tests.

Usage: test-config-transactions.py [SOURCE [SCRATCH_PARENT]]
"""
from __future__ import annotations

import concurrent.futures
import errno
import fcntl
import importlib.util
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None

spec = importlib.util.spec_from_file_location("locachy_config_test", SOURCE / "link/locachy_config.py")
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class ConfigTransactions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="locachy-config-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / "rc.conf"
        self.containers = self.root / "containers"
        self.containers.mkdir()
        self.path.write_text('# keep\nfoo="old"\nbar="two"\nfoo="duplicate"\ntail="x"\n', encoding="utf-8")
        os.chmod(self.path, 0o640)
        self.helper = self.root / "helper"
        self.cli = self.root / "locachy-config"
        cc = shutil.which("gcc") or "cc"
        common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(SOURCE / "base/include")]
        subprocess.run(common + [str(SOURCE / "base/rc.c"), str(SOURCE / "tools/test-config-helper.c"),
                                 "-o", str(self.helper)], check=True, capture_output=True, text=True)
        say = self.root / "say.c"
        say.write_text("void say(const char *fmt, ...) { (void)fmt; }\n", encoding="utf-8")
        subprocess.run(common + [f'-DCONFIG_RC_PATH="{self.path}"', f'-DCONFIG_CONTAINER_DIR="{self.containers}"',
                                 str(SOURCE / "progs/locachy-config.c"), str(SOURCE / "base/rc.c"), str(say),
                                 "-o", str(self.cli)], check=True, capture_output=True, text=True)
        self.wrap_helper = None
        if sys.platform.startswith("linux"):
            self.wrap_helper = self.root / "wrap-helper"
            subprocess.run(common + ["-DCONFIG_TEST_FAULTS", "-Wl,--wrap=fsync,--wrap=renameat",
                                     str(SOURCE / "base/rc.c"), str(SOURCE / "tools/test-config-helper.c"),
                                     "-o", str(self.wrap_helper)], check=True, capture_output=True, text=True)

    def c(self, *args, check=True, exe=None, env=None):
        return subprocess.run([str(exe or self.helper), *map(str, args)], check=check,
                              capture_output=True, text=True, env=env)

    def run_cli(self, *args, check=True):
        return subprocess.run([str(self.cli), *map(str, args)], check=check, capture_output=True, text=True)

    def test_batch_semantics_comments_final_newline_and_modes_match(self):
        self.path.write_text('# head\n  alpha=one\nbeta="2"\nalpha="dup"\ntail="end"', encoding="utf-8")
        os.chmod(self.path, 0o640)
        self.c("batch", self.path, "alpha", "updated", "beta", "<unset>", "gamma", "added")
        c_result = self.path.read_bytes()
        self.assertEqual(c_result, b'# head\nalpha="updated"\ntail="end"\ngamma="added"\n')
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o640)
        lock = self.path.parent / "rc.conf.lock"
        self.assertTrue(lock.is_file())
        self.assertEqual(stat.S_IMODE(lock.stat().st_mode), 0o600)

        py_path = self.root / "py.conf"
        py_path.write_text('# head\n  alpha=one\nbeta="2"\nalpha="dup"\ntail="end"', encoding="utf-8")
        os.chmod(py_path, 0o640)
        config.update(py_path, [("alpha", "updated"), ("beta", None), ("gamma", "added")])
        self.assertEqual(py_path.read_bytes(), c_result)
        self.assertEqual(stat.S_IMODE(py_path.stat().st_mode), 0o640)

        fresh = self.root / "fresh.conf"
        old_umask = os.umask(0o077)
        try:
            config.update(fresh, {"created": "yes"})
        finally:
            os.umask(old_umask)
        self.assertEqual(stat.S_IMODE(fresh.stat().st_mode), 0o644)

    def test_cli_supports_fixed_targets_and_mixed_batches_only(self):
        self.run_cli("rc", "set", "foo", "cli", "unset", "bar", "set", "added", "1")
        self.assertEqual(self.path.read_text(), '# keep\nfoo="cli"\ntail="x"\nadded="1"\n')
        app = self.containers / "firefox.conf"
        self.run_cli("container", "firefox", "set", "package", "firefox", "set", "network", "1")
        self.assertEqual(app.read_text(), 'package="firefox"\nnetwork="1"\n')
        for bad in (["/etc/passwd", "set", "a", "1"], ["container", "../escape", "set", "a", "1"],
                    ["container", "window-settings-1", "set", "a", "1"], ["rc", "set", "bad-key", "1"],
                    ["rc", "set", "foo", 'bad"quote']):
            self.assertNotEqual(self.run_cli(*bad, check=False).returncode, 0, bad)

    def test_cas_and_unchanged_semantics_in_c_and_python(self):
        old = self.path.read_bytes()
        with self.assertRaises(config.ConflictError):
            config.update(self.path, {"foo": "bad"}, expected=b"wrong")
        with self.assertRaises(config.ConflictError):
            config.compare_exchange(self.path, None, b'foo="absent"\n')
        self.assertEqual(self.c("cas", self.path, "wrong", 'foo="bad"\n', check=False).returncode, 3)
        self.assertEqual(self.path.read_bytes(), old)

        before_ino = self.path.stat().st_ino
        self.assertFalse(config.transaction(self.path, lambda raw: config.UNCHANGED))
        self.assertEqual(self.path.stat().st_ino, before_ino)

        absent = self.root / "absent.conf"
        self.assertEqual(self.c("cas", absent, "<absent>", 'host="0"\n').returncode, 0)
        self.assertEqual(absent.read_bytes(), b'host="0"\n')
        self.assertTrue(config.compare_exchange(absent, b'host="0"\n', b'host="1"\n'))
        self.assertEqual(absent.read_bytes(), b'host="1"\n')

    def test_cross_language_concurrency_has_no_lost_updates_or_partial_reads(self):
        self.path.write_text('c_count="0"\npy_count="0"\ncli_last="0"\n', encoding="utf-8")
        stop = False
        bad_reads = []

        def reader():
            while not stop:
                text = self.path.read_text(encoding="utf-8")
                if not all(k in text for k in ('c_count="', 'py_count="', 'cli_last="')) or not text.endswith("\n"):
                    bad_reads.append(text)

        def py_worker():
            for _ in range(25):
                def step(raw):
                    lines = (raw or b"").decode().splitlines(True)
                    out = []
                    for line in lines:
                        if line.startswith("py_count="):
                            n = int(line.split('"')[1]) + 1
                            out.append(f'py_count="{n}"\n')
                        else:
                            out.append(line)
                    return "".join(out).encode()
                config.transaction(self.path, step)

        def c_worker():
            for i in range(25):
                self.c("set", self.path, "c_step", str(i))

        def cli_worker():
            for i in range(15):
                self.run_cli("rc", "set", "cli_last", str(i))

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            r = pool.submit(reader)
            futures = [pool.submit(py_worker), pool.submit(c_worker), pool.submit(cli_worker)]
            for f in futures:
                f.result()
            stop = True
            r.result()

        self.assertEqual(bad_reads, [])
        final = self.path.read_text()
        self.assertIn('py_count="25"\n', final)
        self.assertIn('cli_last="14"\n', final)
        self.assertIn('c_step="24"\n', final)
        self.assertEqual([p.name for p in self.root.iterdir() if ".tmp." in p.name], [])

    def test_canonical_parent_shares_one_lock_and_rejects_symlinks_or_bad_locks(self):
        alias = self.root / "alias"
        alias.symlink_to(self.root)
        lock = self.root / "rc.conf.lock"
        with open(lock, "wb") as f:
            os.chmod(lock, 0o600)
            fcntl.flock(f.fileno(), fcntl.LOCK_EX)
            proc = subprocess.Popen([str(self.helper), "set", str(alias / "rc.conf"), "foo", "via-alias"])
            self.assertIsNone(proc.poll())
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
            self.assertEqual(proc.wait(timeout=5), 0)
        self.assertIn('foo="via-alias"\n', self.path.read_text())

        sym_target = self.root / "sym.conf"
        sym_target.symlink_to(self.path)
        with self.assertRaises(OSError):
            config.update(sym_target, {"foo": "no"})
        self.assertNotEqual(self.c("set", sym_target, "foo", "no", check=False).returncode, 0)

        bad = self.root / "badlock.conf"
        bad.write_text('a="1"\n', encoding="utf-8")
        (self.root / "badlock.conf.lock").symlink_to(self.path)
        with self.assertRaises(OSError):
            config.update(bad, {"a": "2"})
        self.assertNotEqual(self.c("set", bad, "a", "2", check=False).returncode, 0)

        hard = self.root / "hard.conf"
        hard.write_text('a="1"\n', encoding="utf-8")
        hlock = self.root / "hard.conf.lock"
        hlock.write_text("", encoding="utf-8")
        os.link(hlock, self.root / "hard-extra.lock")
        with self.assertRaises(OSError):
            config.update(hard, {"a": "2"})
        self.assertNotEqual(self.c("set", hard, "a", "2", check=False).returncode, 0)

    def test_uncooperative_external_mutation_is_detected_before_replace(self):
        before = self.path.read_bytes()
        proc = subprocess.Popen([str(self.helper), "pause", str(self.path)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        self.addCleanup(proc.stdin.close)
        self.addCleanup(proc.stdout.close)
        self.assertEqual(proc.stdout.readline().strip(), "READY")
        sneak = self.root / "sneak"
        sneak.write_bytes(b'external="1"\n')
        os.replace(sneak, self.path)
        proc.stdin.write("\n")
        proc.stdin.flush()
        self.assertEqual(proc.wait(timeout=5), 3)
        self.assertEqual(self.path.read_bytes(), b'external="1"\n')
        self.assertNotEqual(self.path.read_bytes(), before)

    def test_staging_rename_and_post_rename_dir_fsync_faults(self):
        before = self.path.read_bytes()
        with mock.patch.object(config.os, "fsync", side_effect=OSError(errno.EIO, "disk fault")):
            with self.assertRaises(OSError):
                config.update(self.path, {"foo": "fault"})
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual([p.name for p in self.root.iterdir() if ".tmp." in p.name], [])

        real_fsync = os.fsync
        calls = 0
        def fail_dir_fsync(fd):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError(errno.EIO, "directory sync fault")
            return real_fsync(fd)
        with mock.patch.object(config.os, "fsync", side_effect=fail_dir_fsync):
            with self.assertRaises(OSError):
                config.update(self.path, {"foo": "after-rename"})
        # After rename succeeded, the new file is complete; failure means durability was not confirmed.
        self.assertIn(b'foo="after-rename"\n', self.path.read_bytes())

        if self.wrap_helper is not None:
            current = self.path.read_bytes()
            env = dict(os.environ, CONFIG_TEST_FSYNC="1")
            self.assertNotEqual(self.c("set", self.path, "foo", "c-fsync-1", check=False,
                                       exe=self.wrap_helper, env=env).returncode, 0)
            self.assertEqual(self.path.read_bytes(), current)
            env = dict(os.environ, CONFIG_TEST_RENAME="1")
            self.assertNotEqual(self.c("set", self.path, "foo", "c-rename", check=False,
                                       exe=self.wrap_helper, env=env).returncode, 0)
            self.assertEqual(self.path.read_bytes(), current)
            env = dict(os.environ, CONFIG_TEST_FSYNC="2")
            self.assertNotEqual(self.c("set", self.path, "foo", "c-dir-fsync", check=False,
                                       exe=self.wrap_helper, env=env).returncode, 0)
            self.assertIn(b'foo="c-dir-fsync"\n', self.path.read_bytes())
            self.assertEqual([p.name for p in self.root.iterdir() if ".tmp." in p.name], [])

    def test_remove_semantics_lock_and_refusals(self):
        self.assertFalse(config.remove(self.root / "missing.conf"))
        target = self.root / "gone.conf"
        target.write_text('a="1"\n', encoding="utf-8")
        with self.assertRaises(config.ConflictError):
            config.remove(target, expected=b'a="2"\n')
        lock_inode = (self.root / "gone.conf.lock").stat().st_ino
        self.assertEqual(target.read_bytes(), b'a="1"\n')
        with self.assertRaises(config.ConflictError):
            config.remove(target, expected=None)
        self.assertTrue(config.remove(target, expected=b'a="1"\n'))
        self.assertFalse(target.exists())
        self.assertFalse(config.remove(target, expected=None))
        with self.assertRaises(config.ConflictError):
            config.remove(target, expected=b"x")
        lock = self.root / "gone.conf.lock"
        self.assertTrue(lock.is_file())
        self.assertEqual(lock.stat().st_ino, lock_inode)  # the same stable lock, never replaced
        self.assertEqual(stat.S_IMODE(lock.stat().st_mode), 0o600)

        linked = self.root / "linked.conf"
        linked.symlink_to(self.path)
        with self.assertRaises(OSError):
            config.remove(linked)
        self.assertTrue(linked.is_symlink())
        self.assertTrue(self.path.exists())

        directory = self.root / "a-dir.conf"
        directory.mkdir()
        with self.assertRaises(OSError):
            config.remove(directory)
        self.assertTrue(directory.is_dir())

        # Transactions refuse a damaged file; removing it does not read it.
        damaged = self.root / "damaged.conf"
        damaged.write_bytes(b'a="1"\n\0junk\n')
        with self.assertRaises(ValueError):
            config.update(damaged, {"a": "2"})
        self.assertTrue(config.remove(damaged))
        self.assertFalse(damaged.exists())

        # An uncooperative replacement after the check, under the lock: the
        # identity/metadata recheck refuses to unlink the other file.
        swapped = self.root / "swapped.conf"
        swapped.write_bytes(b'a="1"\n')
        real_regular_stat = config._regular_stat

        def stat_then_external_replace(path):
            before = real_regular_stat(path)
            replacement = self.root / "replacement"
            replacement.write_bytes(b'a="external"\n')
            os.replace(replacement, path)
            return before

        with mock.patch.object(config, "_regular_stat", stat_then_external_replace):
            with self.assertRaises(config.ConflictError):
                config.remove(swapped)
        self.assertEqual(swapped.read_bytes(), b'a="external"\n')

    def test_remove_reports_directory_sync_failure_after_unlink(self):
        target = self.root / "durable.conf"
        target.write_text('a="1"\n', encoding="utf-8")
        with mock.patch.object(config.os, "fsync", side_effect=OSError(errno.EIO, "directory sync fault")):
            with self.assertRaises(OSError):
                config.remove(target)
        self.assertFalse(target.exists())
        self.assertFalse(config.remove(target))

    def test_remove_and_update_serialize_under_the_lock(self):
        target = self.root / "race.conf"
        target.write_text('n="0"\n', encoding="utf-8")
        errors = []

        def updater():
            for i in range(60):
                try:
                    config.update(target, {"n": str(i)})
                except OSError as e:
                    errors.append(e)

        def remover():
            for _ in range(60):
                try:
                    config.remove(target)
                except OSError as e:
                    errors.append(e)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            for future in [pool.submit(updater), pool.submit(remover)]:
                future.result()
        self.assertEqual(errors, [])
        self.assertEqual([p.name for p in self.root.iterdir() if ".tmp." in p.name], [])
        if target.exists():
            self.assertRegex(target.read_text(), r'^n="\d+"\n$')

    def test_removals_from_an_absent_file_create_nothing(self):
        absent = self.root / "never.conf"
        self.assertFalse(config.update(absent, {"x": None}))
        self.assertFalse(absent.exists())
        self.c("batch", self.root / "never2.conf", "x", "<unset>")
        self.assertFalse((self.root / "never2.conf").exists())
        self.run_cli("container", "ghost", "unset", "network")
        self.assertFalse((self.containers / "ghost.conf").exists())


if __name__ == "__main__":
    unittest.main()
