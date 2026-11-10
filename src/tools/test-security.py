#!/usr/bin/env python3
"""locachy-security: queued policy transitions, checked again when they run.

The TPM, the boot image builder, locachy-duress and the job service are
doubles; rc.conf and sudo's PAM file go through the real shared
configuration helper in a private directory.
"""
import contextlib
import importlib.machinery
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
LINK = Path(__file__).resolve().parent.parent / "link"


def load(name, filename):
    loader = importlib.machinery.SourceFileLoader(name, str(LINK / filename))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


security = load("locachy_security", "locachy-security")
config = load("locachy_config", "locachy_config.py")

PAM = "#%PAM-1.0\nauth\t\tinclude\t\tsystem-auth\naccount\t\tinclude\t\tsystem-auth\n"


def pam_with(line):
    return PAM.replace("auth", line + "\nauth", 1)


class FakeTpm:
    def __init__(self):
        self.tokens = {"/dev/root": [], "/dev/swap": []}
        self.pending = None
        self.stuck = False
        self.cleared = []

    def find_luks_targets(self):
        return list(self.tokens)

    def device_tpm_tokens(self, dev):
        return list(enumerate(self.tokens[dev]))

    def pending_request(self):
        return self.pending

    def clear_device_tpm(self, dev):
        self.cleared.append(dev)
        if not self.stuck:
            self.tokens[dev] = []

    def drop_pending(self):
        if not self.stuck:
            self.pending = None


class SecurityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.calls = root / "calls"
        self.calls.write_text("")
        self.tpm = FakeTpm()
        self.jobs = types.SimpleNamespace(execution_mode=lambda: "offline", submit=None)
        names = ("RC_CONF", "SUDO_PAM", "LOCK", "MARKER", "BOOT", "DURESS", "helper")
        saved = {name: getattr(security, name) for name in names}
        self.addCleanup(lambda: [setattr(security, n, v) for n, v in saved.items()])
        env = mock.patch.dict(os.environ, {"DURESS_STATUS": "set luks"})
        env.start()
        self.addCleanup(env.stop)
        security.RC_CONF = root / "rc.conf"
        security.SUDO_PAM = root / "sudo"
        security.LOCK = root / "security.lock"
        security.MARKER = root / "state" / "security-policy.json"
        security.BOOT = self.script(root / "locachy-boot", "boot")
        security.DURESS = self.script(root / "locachy-duress", "duress",
                                      'if [ "$1" = status ]; then echo "$DURESS_STATUS"; fi')
        security.RC_CONF.write_text('keymap="us"\n')
        security.SUDO_PAM.write_text(PAM)
        modules = {"locachy_config": config, "locachy-tpm": self.tpm, "locachy_jobs": self.jobs}
        security.helper = lambda name: modules[name]

    def script(self, path, name, body=""):
        path.write_text(f'#!/bin/sh\necho "{name} $*" >>"{self.calls}"\n{body}\n')
        path.chmod(0o755)
        return path

    def apply(self, kind, value="off"):
        with contextlib.redirect_stdout(io.StringIO()):
            security.apply_policy(kind, value)

    def rc(self):
        return security.read_values()

    def ran(self):
        return [line.strip() for line in self.calls.read_text().splitlines()]

    def test_boot_lock_skip_refused_while_tpm_unlock_or_high(self):
        self.tpm.tokens["/dev/root"] = [{"type": "systemd-tpm2"}]
        with self.assertRaisesRegex(RuntimeError, "cannot be skipped"):
            self.apply("login-skip", "on")
        self.tpm.tokens["/dev/root"] = []
        self.tpm.pending = {"devices": {}}
        with self.assertRaisesRegex(RuntimeError, "cannot be skipped"):
            self.apply("login-skip", "on")
        self.tpm.pending = None
        security.RC_CONF.write_text('lock_mode="high"\n')
        with self.assertRaisesRegex(RuntimeError, "cannot be skipped"):
            self.apply("login-skip", "on")
        self.assertNotIn("login_skip", self.rc())
        self.assertFalse(security.MARKER.exists())

    def test_boot_lock_skip_needs_no_boot_image(self):
        self.apply("login-skip", "on")
        self.assertEqual(self.rc()["login_skip"], "1")
        self.apply("login-skip", "off")
        self.assertNotIn("login_skip", self.rc())
        self.assertEqual(self.ran(), [])
        self.assertFalse(security.MARKER.exists())

    def test_high_clears_tpm_first_then_writes_policy_and_boot_image(self):
        self.tpm.tokens["/dev/root"] = [{"type": "systemd-tpm2"}]
        self.tpm.pending = {"devices": {}}
        security.RC_CONF.write_text('unlock_fingerprint="1"\n')
        security.SUDO_PAM.write_text(pam_with("auth\t\tsufficient\tpam_locachy_any.so locachy-lock-fingerprint"))
        self.apply("lock-mode", "high")
        self.assertEqual(self.tpm.cleared, ["/dev/root", "/dev/swap"])
        self.assertIsNone(self.tpm.pending)
        self.assertEqual(self.rc(), {"unlock_fingerprint": "1", "lock_mode": "high"})
        self.assertNotIn("pam_locachy_any", security.SUDO_PAM.read_text())
        self.assertEqual(self.ran(), ["boot"])
        self.assertFalse(security.MARKER.exists())

    def test_high_is_not_written_while_a_tpm_slot_remains(self):
        self.tpm.tokens["/dev/root"] = [{"type": "systemd-tpm2"}]
        self.tpm.stuck = True
        with self.assertRaisesRegex(RuntimeError, "keys remain"):
            self.apply("lock-mode", "high")
        self.assertNotIn("lock_mode", self.rc())
        self.assertEqual(self.ran(), [])
        # Unfinished: other policy changes and TPM enrollment wait for it.
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            self.apply("duress", "off")
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            security.ensure_tpm_allowed()
        self.tpm.stuck = False
        self.apply("lock-mode", "high")
        self.assertEqual(self.rc()["lock_mode"], "high")
        self.assertFalse(security.MARKER.exists())

    def test_high_refused_while_the_boot_lock_is_skipped(self):
        security.RC_CONF.write_text('login_skip="1"\n')
        with self.assertRaisesRegex(RuntimeError, "turn the boot lock on first"):
            self.apply("lock-mode", "high")
        self.assertEqual(self.rc(), {"login_skip": "1"})
        self.assertEqual(self.ran(), [])
        self.assertFalse(security.MARKER.exists())

    def test_low_keeps_the_boot_lock_choice(self):
        security.RC_CONF.write_text('lock_mode="high"\n')
        self.apply("lock-mode", "low")
        self.assertEqual(self.rc(), {"lock_mode": "low"})
        self.assertEqual(self.ran(), ["boot"])

    def test_tpm_enrollment_checks_the_policy_when_it_runs(self):
        security.ensure_tpm_allowed()
        security.RC_CONF.write_text('login_skip="1"\n')
        with self.assertRaisesRegex(RuntimeError, "boot lock on"):
            security.ensure_tpm_allowed()
        security.RC_CONF.write_text('lock_mode="high"\n')
        with self.assertRaisesRegex(RuntimeError, "high threat level"):
            security.ensure_tpm_allowed()

    def test_biometrics_and_duress_lines_in_sudo_pam(self):
        self.apply("fingerprint", "on")
        self.apply("duress", "on")
        lines = security.SUDO_PAM.read_text().splitlines()
        self.assertEqual(lines[1], "auth\t\tsufficient\tpam_locachy_any.so locachy-lock-fingerprint")
        self.assertEqual(lines[2], "auth\t\toptional\tpam_locachy_duress.so")
        self.assertEqual(lines[3], "auth\t\tinclude\t\tsystem-auth")
        self.assertEqual(self.ran(), ["duress status", "boot"])
        security.RC_CONF.write_text(security.RC_CONF.read_text() + 'lock_mode="high"\n')
        with self.assertRaisesRegex(RuntimeError, "password authentication only"):
            self.apply("face", "on")

    def test_duress_needs_its_password_and_an_encrypted_disk(self):
        os.environ["DURESS_STATUS"] = "not-set luks"
        with self.assertRaisesRegex(RuntimeError, "duress needs"):
            self.apply("duress", "on")
        self.assertNotIn("duress_enabled", self.rc())
        self.assertFalse(security.MARKER.exists())

    def test_remove_duress_turns_it_off_before_the_password_goes(self):
        security.RC_CONF.write_text('duress_enabled="1"\n')
        security.SUDO_PAM.write_text(pam_with("auth\t\toptional\tpam_locachy_duress.so"))
        self.apply("remove-duress")
        self.assertNotIn("duress_enabled", self.rc())
        self.assertNotIn("pam_locachy_duress", security.SUDO_PAM.read_text())
        self.assertEqual(self.ran(), ["duress clear", "boot"])

    def test_invalid_assignment_is_never_guessed(self):
        security.RC_CONF.write_text('lock_mode=high extra\n')
        with self.assertRaisesRegex(RuntimeError, "invalid security policy assignment"):
            self.apply("login-skip", "off")

    def test_queue_mode_submits_the_fixed_job_only(self):
        submitted = []
        self.jobs.execution_mode = lambda: "queue"
        self.jobs.submit = lambda kind, args: submitted.append((kind, args)) or 0
        with mock.patch.object(security.os, "geteuid", lambda: 0):
            self.assertEqual(security.main(["set", "lock-mode", "high"]), 0)
            self.assertEqual(security.main(["remove-duress"]), 0)
        self.assertEqual(submitted, [("security-policy", ["set", "lock-mode", "high"]),
                                     ("security-policy", ["remove-duress"])])
        self.assertNotIn("lock_mode", self.rc())
        self.assertEqual(self.ran(), [])


if __name__ == "__main__":
    unittest.main()
