#!/usr/bin/env python3
"""Atomic updater repair with private files; never a host mutation or network.
  python3 -B tools/test-updater-repair.py SCRATCH_PARENT
"""
import fcntl
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('repair_updater', SOURCE / 'repair-updater.py')
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class Repair(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='updater-repair-', dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.target = self.root / 'usr/local/bin/locachy-pkg'
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(b'#!/usr/bin/env python3\nprint("old updater")\n')
        self.target.chmod(0o755)
        self.old = self.target.read_bytes()
        self.candidate = self.root / 'candidate'
        self.candidate.write_bytes(b'#!/usr/bin/env python3\nprint("new updater")\n')
        self.sha = hashlib.sha256(self.candidate.read_bytes()).hexdigest()
        self.lock = self.root / 'run/locachy/pkg.lock'
        self.lock.parent.mkdir(parents=True)

    def apply(self, sha=None):
        return repair.repair(self.candidate, sha or self.sha, self.root)

    def test_success_keeps_old_inode_and_repeated_repair_is_a_noop(self):
        old_fd = os.open(self.target, os.O_RDONLY)
        try:
            self.apply()
            self.assertEqual(self.target.read_bytes(), self.candidate.read_bytes())
            self.assertEqual(os.read(old_fd, 4096), self.old)
        finally:
            os.close(old_fd)
        backups = list(self.target.parent.glob('*.before-repair'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.old)
        self.assertEqual(self.target.stat().st_mode & 0o777, 0o755)
        inode = self.target.stat().st_ino
        self.apply()
        self.assertEqual(self.target.stat().st_ino, inode)

    def test_hash_or_syntax_failure_keeps_original(self):
        self.assertRaisesRegex(ValueError, 'SHA256', self.apply, '0' * 64)
        self.assertEqual(self.target.read_bytes(), self.old)
        self.candidate.write_bytes(b'#!/usr/bin/env python3\ninvalid python !\n')
        sha = hashlib.sha256(self.candidate.read_bytes()).hexdigest()
        self.assertRaises(SyntaxError, self.apply, sha)
        self.assertEqual(self.target.read_bytes(), self.old)
        self.assertEqual(list(self.target.parent.iterdir()), [self.target])

    def test_busy_domain_and_pacman_lock_are_never_removed(self):
        fd = os.open(self.lock, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertRaisesRegex(RuntimeError, 'job is running', self.apply)
            self.assertEqual(self.target.read_bytes(), self.old)
        finally:
            os.close(fd)
        db_lock = self.root / 'var/lib/pacman/db.lck'
        db_lock.parent.mkdir(parents=True)
        db_lock.write_text('busy')
        self.assertRaisesRegex(RuntimeError, 'pacman database is locked', self.apply)
        self.assertEqual(db_lock.read_text(), 'busy')
        self.assertEqual(self.target.read_bytes(), self.old)

    def test_symlink_target_candidate_or_lock_is_refused(self):
        for which in ('target', 'candidate', 'lock'):
            with self.subTest(which=which):
                path = getattr(self, which)
                saved = path.with_name(path.name + '.real')
                if path.exists():
                    path.rename(saved)
                else:
                    saved.write_text('lock')
                path.symlink_to(saved)
                try:
                    self.assertRaises(OSError, self.apply)
                    old = saved if which == 'target' else self.target
                    self.assertEqual(old.read_bytes(), self.old)
                finally:
                    path.unlink()
                    saved.rename(path)

    def test_failed_publication_never_truncates_installed_file(self):
        with mock.patch.object(repair.os, 'replace', side_effect=OSError('fixture publication failure')):
            self.assertRaisesRegex(OSError, 'publication failure', self.apply)
        self.assertEqual(self.target.read_bytes(), self.old)
        self.assertEqual(list(self.target.parent.glob('.locachy-pkg.repair-*')), [])

    def test_writable_target_or_directory_is_refused(self):
        self.target.chmod(0o777)
        self.assertRaisesRegex(ValueError, 'protected regular', self.apply)
        self.assertEqual(self.target.read_bytes(), self.old)
        self.target.chmod(0o755)
        self.target.parent.chmod(0o777)
        self.assertRaisesRegex(ValueError, 'not writable by others', self.apply)
        self.target.parent.chmod(0o755)
        self.assertEqual(self.target.read_bytes(), self.old)

    def test_cli_has_no_root_override_or_environment_bypass(self):
        # This command only parses an unknown argument and exits, even as root.
        result = subprocess.run([sys.executable, '-I', str(SOURCE / 'repair-updater.py'),
                                 '--root', str(self.root), str(self.candidate), self.sha],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unrecognized arguments', result.stderr)
        self.assertEqual(self.target.read_bytes(), self.old)


if __name__ == '__main__':
    unittest.main()
