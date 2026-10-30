#!/usr/bin/env python3
"""locachy-tpm's TPM 2.0 and Secure Boot rules, with the firmware's
variables, the LUKS tools and the TPM played by doubles; nothing reaches a
disk, the firmware or a TPM.
  python3 -B tools/test-tpm.py
"""
import base64
import contextlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("tpm_tool", str(SOURCE / "link/locachy-tpm"))
spec = importlib.util.spec_from_loader(loader.name, loader)
tpm = importlib.util.module_from_spec(spec)
loader.exec_module(tpm)

OURS = b"\x30\x82our-certificate-der"
THEIRS = b"\x30\x82microsoft-certificate-der"
DISKS = {"/dev/root": "11111111-root", "/dev/swap": "22222222-swap"}


def signature_list(certs):
    """EFI_SIGNATURE_LIST of X.509 certificates, one owner GUID each."""
    out = b""
    for cert in certs:
        entry = b"\x11" * 16 + cert
        out += tpm.EFI_CERT_X509 + struct.pack("<III", 28 + len(entry), 0, len(entry)) + entry
    return out


class Tpm(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tpm-test-")
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.vars = {"SetupMode": b"\x00", "SecureBoot": b"\x01", "db": signature_list([THEIRS, OURS])}
        cert = root / "db.pem"
        cert.write_text("-----BEGIN CERTIFICATE-----\n" + base64.b64encode(OURS).decode() +
                        "\n-----END CERTIFICATE-----\n")
        public = root / "tpm2-pcr-public-key.pem"
        public.write_text("public\n")
        self.calls = []
        self.failing = set()      # commands (their first two words) that fail
        self.tokens = {}       # dev -> [token]
        self.pending_slots = set()  # (dev, key text), the extra password slots
        self.absent = set()         # disks blkid cannot find
        for name, value in (("SB_DB_CERT", cert), ("PCR_PUBLIC", public), ("PENDING", root / "pending"),
                            ("LOCACHY_BOOT", root / "no-locachy-boot"), ("SECURITYFS", root / "no-securityfs")):
            self.patch(name, value)
        self.patch("efi_variable", lambda name, guid: self.vars.get(name))
        self.patch("secure_boot", lambda: self.vars.get("SecureBoot") == b"\x01")
        self.patch("find_luks_targets", lambda: list(DISKS))
        self.patch("device_tpm_tokens", lambda dev: list(enumerate(self.tokens.get(dev, []))))
        p = mock.patch.object(tpm.subprocess, "run", self.run_command)
        p.start()
        self.addCleanup(p.stop)

    def patch(self, name, value):
        p = mock.patch.object(tpm, name, value)
        p.start()
        self.addCleanup(p.stop)

    def run_command(self, command, **kwargs):
        env = kwargs.get("env") or {}
        self.calls.append((command, {k: env[k] for k in ("PASSWORD", "NEWPASSWORD", "NEWPIN", "PIN") if k in env}))
        out = ""
        failed = tuple(command[:2]) in self.failing or any(c in self.failing for c in command)
        rc = 1 if failed else 0
        if command[:2] == ["cryptsetup", "luksUUID"]:
            out = DISKS[command[2]] + "\n"
        elif command[:2] == ["blkid", "-U"]:
            dev = {u: d for d, u in DISKS.items()}[command[2]]
            out = "" if dev in self.absent else dev + "\n"
        elif command[:2] == ["cryptsetup", "luksDump"]:
            out = json.dumps({"keyslots": {"0": {}}, "tokens": {}})
        elif command[0] == "systemd-cryptenroll" and "--password" in command and not failed:
            # The key and its association must already be durable when a
            # command can change the header (or die after doing so).
            uuid = DISKS[command[-1]]
            request = tpm.pending_request()
            key = tpm.PENDING / request["devices"][uuid]
            self.assertEqual(key.read_text(), env["NEWPASSWORD"])
            self.pending_slots.add((command[-1], env["NEWPASSWORD"]))
        elif command[:2] == ["cryptsetup", "open"] and "--test-passphrase" in command and not failed:
            key = Path(command[command.index("--key-file") + 1]).read_text()
            rc = 0 if (command[-1], key) in self.pending_slots else 2
        elif command[:2] == ["cryptsetup", "luksRemoveKey"] and not failed:
            self.pending_slots.discard((command[-1], Path(command[-2]).read_text()))
        return types.SimpleNamespace(returncode=rc, stdout=out, stderr="")

    def commands(self, first):
        return [c for c in self.calls if isinstance(c[0], list) and c[0][0] == first]

    def enrolled(self):
        """The TPM enrollments: (device, command, environment)."""
        return [(c[-1], c, env) for c, env in self.commands("systemd-cryptenroll") if "--tpm2-device=auto" in c]

    def quiet(self):
        stack = contextlib.ExitStack()
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
        return stack

    def test_own_keys_are_this_machines_db_certificate_with_secure_boot_on(self):
        self.assertTrue(tpm.secure_boot_own())
        self.assertEqual(tpm.db_certificates(), [THEIRS, OURS])
        self.vars["db"] = signature_list([THEIRS])
        self.assertFalse(tpm.secure_boot_own())
        self.vars["db"] = signature_list([OURS])
        self.vars["SetupMode"] = b"\x01"
        self.assertFalse(tpm.secure_boot_own())
        self.vars["SetupMode"], self.vars["SecureBoot"] = b"\x00", b"\x00"
        self.assertFalse(tpm.secure_boot_own())
        self.vars["db"] = b"\x00" * 10  # cut short: nothing read past it
        self.assertEqual(tpm.db_certificates(), [])

    def test_with_own_keys_pcr_7_and_the_signed_pcr_11_policy_no_pin(self):
        tpm.enroll_tpm2("/dev/root", ("passphrase", "pass phrase"))
        [(dev, cmd, env)] = self.enrolled()
        self.assertEqual(dev, "/dev/root")
        for option in ("--tpm2-pcrs=7", f"--tpm2-public-key={tpm.PCR_PUBLIC}", "--tpm2-public-key-pcrs=11",
                       "--wipe-slot=tpm2"):
            self.assertIn(option, cmd)
        self.assertFalse(any("pin" in option for option in cmd))
        self.assertEqual(env, {"PASSWORD": "pass phrase"})

    def test_without_own_keys_heads_binding_pcrs_0_7_without_the_pcr_key(self):
        """As HEAD binds, also when a PCR key is on the disk (systemd-cryptenroll
        would take it by itself): nothing refused, no PIN."""
        self.vars["db"] = signature_list([THEIRS])
        with mock.patch.object(sys, "argv", ["locachy-tpm", "enroll", "--passphrase-stdin"]), \
                mock.patch.object(sys, "stdin", io.StringIO("pw\n")), \
                mock.patch.object(tpm.os, "geteuid", lambda: 0), \
                mock.patch.object(tpm, "tpm_info", lambda: (2, "/dev/tpm0")), self.quiet():
            self.assertEqual(tpm.main(), 0)
        enrolled = self.enrolled()
        self.assertEqual([d for d, _, _ in enrolled], ["/dev/root", "/dev/swap"])
        for _, cmd, env in enrolled:
            self.assertIn("--tpm2-pcrs=0+7", cmd)
            self.assertIn("--tpm2-public-key=", cmd)
            self.assertNotIn("--tpm2-public-key-pcrs=11", cmd)
            self.assertFalse(any("pin" in option for option in cmd))
            self.assertEqual(env, {"PASSWORD": "pw"})
        # --pcrs is HEAD's literal binding even with own keys on.
        self.calls.clear()
        self.vars["db"] = signature_list([OURS])
        with mock.patch.object(sys, "argv", ["locachy-tpm", "enroll", "--pcrs", "0+2+7", "--passphrase-stdin",
                                             "/dev/root"]), \
                mock.patch.object(sys, "stdin", io.StringIO("pw\n")), \
                mock.patch.object(tpm.os, "geteuid", lambda: 0), \
                mock.patch.object(tpm, "tpm_info", lambda: (2, "/dev/tpm0")), self.quiet():
            self.assertEqual(tpm.main(), 0)
        [(_, cmd, _)] = self.enrolled()
        self.assertIn("--tpm2-pcrs=0+2+7", cmd)

    def test_no_pcr_key_no_enrollment(self):
        tpm.PCR_PUBLIC.unlink()
        with self.assertRaisesRegex(RuntimeError, "no PCR key"):
            tpm.enroll_tpm2("/dev/root", ("passphrase", "pw"))
        self.assertEqual(self.calls, [])

    def test_installer_leaves_key_slots_then_the_first_start_with_own_keys_binds_and_removes_them(self):
        with self.quiet():
            tpm.prepare_pending(list(DISKS), {d: ("passphrase", "pass phrase") for d in DISKS})
        pending = tpm.PENDING
        self.assertEqual(pending.stat().st_mode & 0o777, 0o700)
        request = json.loads((pending / "request.json").read_text())
        self.assertEqual(sorted(request["devices"]), sorted(DISKS.values()))
        added = [(c, env) for c, env in self.commands("systemd-cryptenroll") if "--password" in c]
        self.assertEqual([c[-1] for c, _ in added], ["/dev/root", "/dev/swap"])
        for c, env in added:
            key = pending / request["devices"][DISKS[c[-1]]]
            self.assertEqual(key.stat().st_mode & 0o777, 0o600)
            self.assertEqual(env, {"PASSWORD": "pass phrase", "NEWPASSWORD": key.read_text()})
            self.assertEqual(len(key.read_text()), 64)
        self.calls.clear()

        # A start without Secure Boot on: it waits.
        self.vars["SecureBoot"] = b"\x00"
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 0)
        self.assertTrue(pending.is_dir())
        self.assertEqual(self.enrolled(), [])

        self.vars["SecureBoot"] = b"\x01"
        keys = {d: pending / request["devices"][u] for d, u in DISKS.items()}
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 0)
        enrolled = self.enrolled()
        self.assertEqual(sorted(d for d, _, _ in enrolled), sorted(DISKS))
        for dev, cmd, env in enrolled:
            self.assertIn(f"--unlock-key-file={keys[dev]}", cmd)
            self.assertIn("--tpm2-pcrs=7", cmd)
            self.assertIn("--tpm2-public-key-pcrs=11", cmd)
            self.assertEqual(env, {})
        removed = [c for c, _ in self.commands("cryptsetup") if c[1] == "luksRemoveKey"]
        self.assertEqual(sorted((c[-1], c[-2]) for c in removed), sorted((d, str(keys[d])) for d in DISKS))
        self.assertFalse(pending.exists())

    def test_secure_boot_enroll_signs_first_and_rebinds_what_the_tpm_opened(self):
        made = []
        self.patch("SB_DB_KEY", Path(self.tmp.name) / "db.key")
        self.patch("PCR_PRIVATE", Path(self.tmp.name) / "pcr.key")
        self.patch("rebuild_boot", lambda *a: made.append("boot") or 0)
        self.patch("image_on_esp", lambda: True)
        args = types.SimpleNamespace(enroll=True, microsoft=False, passphrase_stdin=False)
        with self.quiet() as out:
            self.assertEqual(tpm.cmd_secure_boot(args), 1)
        commands = [c[0][:2] for c in self.calls]
        self.assertIn(["sbctl", "create-keys"], commands)
        self.assertIn(["ukify", "genkey"], commands)
        self.assertNotIn(["sbctl", "enroll-keys"], commands)

        # Setup mode, the root bound as HEAD binds: its TPM token adds the
        # key slot of the binding to come (no passphrase), then the keys.
        self.vars["SetupMode"] = b"\x01"
        self.tokens["/dev/root"] = [{"type": "systemd-tpm2", "tpm2-pcrs": [0, 7]}]
        self.calls.clear()
        args.microsoft = True
        with self.quiet():
            self.assertEqual(tpm.cmd_secure_boot(args), 0)
        self.assertEqual(made, ["boot"])
        sequence = [c for c, _ in self.calls if c[0] == "systemd-cryptenroll" or c[:2] == ["sbctl", "enroll-keys"]]
        self.assertEqual(sequence[0][:2], ["systemd-cryptenroll", "--password"])
        self.assertIn("--unlock-tpm2-device=auto", sequence[0])
        self.assertEqual(sequence[0][-1], "/dev/root")
        self.assertEqual(sequence[-1], ["sbctl", "enroll-keys", "--microsoft"])
        self.assertTrue((tpm.PENDING / "request.json").is_file())
        tpm.drop_pending()

        # The TPM does not open it here (bound with the signed policy): the
        # passphrase; sbctl refusing leaves no binding waiting.
        self.calls.clear()
        self.failing = {"--unlock-tpm2-device=auto", "enroll-keys"}
        with self.quiet(), mock.patch.object(tpm.getpass, "getpass", lambda prompt: "typed"):
            self.assertEqual(tpm.cmd_secure_boot(args), 1)
        added = [env for c, env in self.commands("systemd-cryptenroll") if "--password" in c and env.get("PASSWORD")]
        self.assertEqual([env["PASSWORD"] for env in added], ["typed"])
        self.assertFalse(tpm.PENDING.exists())

    def test_keys_are_not_enrolled_when_the_signed_image_is_not_on_the_esp(self):
        self.patch("SB_DB_KEY", Path(self.tmp.name) / "db.key")
        self.patch("PCR_PRIVATE", Path(self.tmp.name) / "pcr.key")
        self.patch("rebuild_boot", lambda *a: 0)
        self.patch("image_on_esp", lambda: False)
        self.vars["SetupMode"] = b"\x01"
        args = types.SimpleNamespace(enroll=True, microsoft=False, passphrase_stdin=False)
        with self.quiet():
            self.assertEqual(tpm.cmd_secure_boot(args), 1)
        self.assertNotIn(["sbctl", "enroll-keys"], [c[0][:2] for c in self.calls])

    def test_a_new_binding_or_clear_gives_up_the_waiting_one(self):
        with self.quiet():
            tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        self.vars["db"] = signature_list([THEIRS])
        with mock.patch.object(sys, "argv", ["locachy-tpm", "enroll", "--passphrase-stdin", "/dev/root"]), \
                mock.patch.object(sys, "stdin", io.StringIO("pw\n")), \
                mock.patch.object(tpm.os, "geteuid", lambda: 0), \
                mock.patch.object(tpm, "tpm_info", lambda: (2, "/dev/tpm0")), self.quiet():
            self.assertEqual(tpm.main(), 0)
        self.assertFalse(tpm.PENDING.exists())
        self.assertIn("luksRemoveKey", [c[1] for c, _ in self.commands("cryptsetup")])

    def test_missing_disk_keeps_its_pending_binding_while_other_disk_finishes(self):
        tpm.prepare_pending(list(DISKS), {d: ("passphrase", "pw") for d in DISKS})
        self.calls.clear()
        self.absent.add("/dev/swap")
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 1)
        request = tpm.pending_request()
        self.assertEqual(list(request["devices"]), [DISKS["/dev/swap"]])
        self.assertTrue((tpm.PENDING / request["devices"][DISKS["/dev/swap"]]).is_file())
        self.assertEqual([d for d, _, _ in self.enrolled()], ["/dev/root"])
        self.absent.clear()
        self.calls.clear()
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 0)
        self.assertEqual([d for d, _, _ in self.enrolled()], ["/dev/swap"])
        self.assertFalse(tpm.PENDING.exists())
        self.assertEqual(self.pending_slots, set())

    def test_failed_pending_slot_removal_keeps_the_key_and_retries_cleanup_only(self):
        tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        request = tpm.pending_request()
        key = tpm.PENDING / request["devices"][DISKS["/dev/root"]]
        text = key.read_text()
        self.failing.add("luksRemoveKey")
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 1)
        self.assertEqual(key.read_text(), text)
        self.assertIn(("/dev/root", text), self.pending_slots)
        self.assertEqual(tpm.pending_request()["enrolled"], [DISKS["/dev/root"]])
        self.calls.clear()
        self.failing.clear()
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 0)
        self.assertEqual(self.enrolled(), [])
        self.assertFalse(tpm.PENDING.exists())

    def test_drop_pending_retains_missing_or_unremovable_slots_and_returns_failure(self):
        tpm.prepare_pending(list(DISKS), {d: ("passphrase", "pw") for d in DISKS})
        self.absent.add("/dev/swap")
        self.failing.add("luksRemoveKey")
        before = {u: (tpm.PENDING / n).read_text() for u, n in tpm.pending_request()["devices"].items()}
        with self.quiet(), self.assertRaisesRegex(RuntimeError, "cleanup incomplete"):
            tpm.drop_pending()
        self.assertEqual(before, {u: (tpm.PENDING / n).read_text()
                                  for u, n in tpm.pending_request()["devices"].items()})
        self.failing.clear()
        self.absent.clear()
        tpm.drop_pending()
        self.assertFalse(tpm.PENDING.exists())
        self.assertEqual(self.pending_slots, set())

    def test_interrupted_prepare_reuses_the_key_and_does_not_add_a_second_slot(self):
        # Stop after the header changed, before systemd-cryptenroll returned.
        real_run = self.run_command
        def interrupted(command, **kwargs):
            result = real_run(command, **kwargs)
            if "--password" in command:
                raise OSError("interrupted after adding the key")
            return result
        with mock.patch.object(tpm.subprocess, "run", interrupted), \
                self.assertRaisesRegex(OSError, "interrupted"):
            tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        request = tpm.pending_request()
        key = tpm.PENDING / request["devices"][DISKS["/dev/root"]]
        text = key.read_text()
        self.calls.clear()
        tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        self.assertEqual(key.read_text(), text)
        self.assertFalse(any("--password" in c for c, _ in self.commands("systemd-cryptenroll")))
        self.assertEqual(self.pending_slots, {("/dev/root", text)})

    def test_failed_atomic_request_save_preserves_the_old_request_and_no_header_changes(self):
        tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        before = (tpm.PENDING / "request.json").read_text()
        real_replace = tpm.os.replace
        def fail_request(src, dst):
            if Path(dst).name == "request.json":
                raise OSError("cannot replace request")
            real_replace(src, dst)
        self.calls.clear()
        with mock.patch.object(tpm.os, "replace", fail_request), self.assertRaisesRegex(OSError, "cannot replace"):
            tpm.prepare_pending(["/dev/swap"], {"/dev/swap": ("passphrase", "pw")})
        self.assertEqual((tpm.PENDING / "request.json").read_text(), before)
        self.assertFalse(any("--password" in c for c, _ in self.commands("systemd-cryptenroll")))
        # The unassociated key was never used; reuse it after the retry.
        key = tpm.PENDING / (DISKS["/dev/swap"] + ".key")
        text = key.read_text()
        tpm.prepare_pending(["/dev/swap"], {"/dev/swap": ("passphrase", "pw")})
        self.assertEqual(key.read_text(), text)
        self.assertIn(("/dev/swap", text), self.pending_slots)

    def test_interrupted_retirement_retries_without_the_already_removed_slot(self):
        tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        request = tpm.pending_request()
        key = tpm.PENDING / request["devices"][DISKS["/dev/root"]]
        real_save = tpm.save_pending_request
        def fail_empty(request):
            if not request["devices"]:
                raise OSError("stopped after removing the slot")
            real_save(request)
        with mock.patch.object(tpm, "save_pending_request", fail_empty), self.quiet():
            self.assertEqual(tpm.cmd_pending(), 1)
        self.assertTrue(key.is_file())
        self.assertEqual(self.pending_slots, set())
        self.calls.clear()
        with self.quiet():
            self.assertEqual(tpm.cmd_pending(), 0)
        self.assertEqual(self.enrolled(), [])
        self.assertFalse(any(c[1] == "luksRemoveKey" for c, _ in self.commands("cryptsetup")))
        self.assertFalse(tpm.PENDING.exists())

    def test_unreadable_key_check_or_corrupt_request_never_discards_recovery_material(self):
        tpm.prepare_pending(["/dev/root"], {"/dev/root": ("passphrase", "pw")})
        request = tpm.pending_request()
        key = tpm.PENDING / request["devices"][DISKS["/dev/root"]]
        self.failing.add("--test-passphrase")
        with self.quiet(), self.assertRaisesRegex(RuntimeError, "cleanup incomplete"):
            tpm.drop_pending()
        self.assertTrue(key.is_file())
        self.assertEqual(tpm.pending_request(), request)
        (tpm.PENDING / "request.json").write_text('{"devices":')
        with self.assertRaisesRegex(RuntimeError, "left untouched"):
            tpm.drop_pending()
        self.assertTrue(key.is_file())

    def test_status_says_which_binding(self):
        self.tokens["/dev/root"] = [{"type": "systemd-tpm2", "tpm2-pcrs": [7], "tpm2_pubkey": "x",
                                     "tpm2_pubkey_pcrs": [11]}]
        self.tokens["/dev/swap"] = [{"type": "systemd-tpm2", "tpm2-pcrs": [0, 7]}]
        with mock.patch.object(tpm, "tpm_info", lambda: (2, "/dev/tpm0")), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            tpm.cmd_status(types.SimpleNamespace(json=True))
        st = json.loads(out.getvalue())
        self.assertEqual(st["enrolled"]["/dev/root"], [{"type": "systemd-tpm2", "pcrs": [7], "signed_pcrs": [11]}])
        self.assertEqual(st["enrolled"]["/dev/swap"], [{"type": "systemd-tpm2", "pcrs": [0, 7], "signed_pcrs": []}])
        self.assertTrue(st["bound_own_keys"] and st["secure_boot_own"])
        self.assertFalse(st["pending"])
        self.assertNotIn("pin", out.getvalue())


if __name__ == "__main__":
    unittest.main()
