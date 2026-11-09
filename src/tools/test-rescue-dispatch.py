#!/usr/bin/env python3
"""Exercise the rescue action router without devices, services or root changes."""
import importlib.machinery
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("rescue_dispatch", str(SOURCE / "installer/locachy-rescue"))
spec = importlib.util.spec_from_loader(loader.name, loader)
r = importlib.util.module_from_spec(spec)
loader.exec_module(r)


def storage():
    return {"ok": True, "target": {"state": "closed"}, "devices": [{"path": "/dev/fixture"}],
            "output": "", "error": ""}


def link():
    return {"ok": True, "link": {"active": False, "state": "off"},
            "interfaces": [{"name": "cable0"}], "output": "", "error": ""}


class Dispatch(unittest.TestCase):
    def test_status_merges_independent_backends(self):
        with mock.patch.object(r, "call", side_effect=[storage(), link()]) as call:
            reply = r.dispatch(["status"])
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["devices"], [{"path": "/dev/fixture"}])
        self.assertEqual(reply["interfaces"], [{"name": "cable0"}])
        self.assertEqual(call.call_args_list, [mock.call(r.STORAGE, ["status"]), mock.call(r.LINK, ["interfaces"])])

    def test_secrets_go_only_to_requested_backend(self):
        secret = b"exact passphrase with spaces"
        calls = []
        def call(program, args, secret=None):
            calls.append((program, args, secret))
            return storage() if program == r.STORAGE else link()
        with mock.patch.object(r, "call", side_effect=call):
            self.assertTrue(r.dispatch(["unlock-readonly", "/dev/fixture"], secret)["ok"])
            self.assertTrue(r.dispatch(["start-link", "cable0"], secret)["ok"])
        self.assertEqual([s for p, a, s in calls if s is not None], [secret, secret])
        self.assertIn((r.STORAGE, ["unlock-readonly", "/dev/fixture"], secret), calls)
        self.assertIn((r.LINK, ["start", "cable0"], secret), calls)
        self.assertTrue(all(secret.decode() not in " ".join(a) for p, a, s in calls))

    def test_cleanup_stops_link_and_closes_target_even_after_failure(self):
        failed = dict(link(), ok=False, error="link cleanup failed")
        with mock.patch.object(r, "call", side_effect=[failed, storage(), storage(), link()]) as call:
            reply = r.dispatch(["cleanup"])
        self.assertFalse(reply["ok"])
        self.assertIn("link cleanup failed", reply["error"])
        self.assertEqual(call.call_args_list, [mock.call(r.LINK, ["stop"]), mock.call(r.STORAGE, ["close"]),
                                              mock.call(r.STORAGE, ["status"]), mock.call(r.LINK, ["interfaces"])])

    def test_unknown_or_malformed_action_only_reads_status(self):
        calls = []
        def call(program, args, secret=None):
            calls.append((program, args, secret))
            return storage() if program == r.STORAGE else link()
        with mock.patch.object(r, "call", side_effect=call):
            for args in (["shell"], ["enable-repair"], ["start-link"], ["status", "extra"], ["close-target", "extra"]):
                self.assertFalse(r.dispatch(args)["ok"])
        self.assertTrue(all(args in (["status"], ["interfaces"]) and secret is None
                            for program, args, secret in calls))

    def test_reply_and_subprocess_failures_stay_failures(self):
        cases = [subprocess.CompletedProcess([], 0, b"not json", b""),
                 subprocess.CompletedProcess([], 1, b'{"ok":true}', b""),
                 subprocess.CompletedProcess([], 0, b'[]', b"")]
        for result in cases:
            with self.subTest(result=result), mock.patch.object(r.subprocess, "run", return_value=result):
                self.assertFalse(r.call("/fixture", ["status"])["ok"])
        for error in (OSError("missing"), subprocess.TimeoutExpired("fixture", 1)):
            with mock.patch.object(r.subprocess, "run", side_effect=error):
                self.assertFalse(r.call("/fixture", ["status"])["ok"])


if __name__ == "__main__":
    unittest.main()
