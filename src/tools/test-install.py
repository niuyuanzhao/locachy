#!/usr/bin/env python3
"""Installer mount/CPU boundaries without root, mounts or package execution.
  python3 -B tools/test-install.py SCRATCH_PARENT
"""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("installer", str(SOURCE / "link/locachy-install"))
spec = importlib.util.spec_from_loader(loader.name, loader)
i = importlib.util.module_from_spec(spec)
loader.exec_module(i)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class Install(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="installer-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve() / "target"
        self.esp = self.root / "efi"
        self.esp.mkdir(parents=True)
        self.root_fs = {"target": str(self.root), "source": "/dev/vdb2", "fstype": "ext4",
                        "options": "rw,relatime", "maj:min": "252:18"}
        self.esp_fs = {"target": str(self.esp), "source": "/dev/vdb1", "fstype": "vfat",
                       "options": "rw,relatime", "maj:min": "252:17"}
        self.running = {"target": "/", "source": "airootfs", "fstype": "overlay", "maj:min": "0:11"}
        self.mounts = [self.running, self.root_fs, self.esp_fs]
        self.label = "locachy-root"
        self.fat = "FAT32"
        self.uuids = {self.root_fs["source"]: "root-uuid", self.esp_fs["source"]: "esp-uuid"}
        patch = mock.patch.object(i, "output", self.output)
        patch.start()
        self.addCleanup(patch.stop)
        real_stat = i.os.stat
        def stat(path, **kwargs):
            if Path(path) == self.root:
                return type("Stat", (), {"st_dev": 99999})()
            return real_stat(path, **kwargs)
        patch = mock.patch.object(i.os, "stat", stat)
        patch.start()
        self.addCleanup(patch.stop)
        self.record = Path(self.tmp.name) / "run/resume.json"
        patch = mock.patch.object(i, "RESUME_RECORD", self.record)
        patch.start()
        self.addCleanup(patch.stop)
        patch = mock.patch.object(i, "RESUME_APPS", self.record.with_name("apps-catalog"))
        patch.start()
        self.addCleanup(patch.stop)
        # This run's app presets (the live entry's LOCACHY_APPS_CATALOG): none
        # until a test makes one.
        self.apps = Path(self.tmp.name) / "run/locachy-apps"
        patch = mock.patch.dict(os.environ, {"LOCACHY_APPS_CATALOG": str(self.apps), "LOCACHY_APPS": ""})
        patch.start()
        self.addCleanup(patch.stop)
        # AUR metadata/key handling has its own tests. These mount/option
        # cases never query the network or execute a package helper.
        patch = mock.patch.object(i, "preflight_keys", return_value=0)
        patch.start()
        self.addCleanup(patch.stop)
        def trust(keys, root=None):
            return i.subprocess.run(["arch-chroot", str(root), "/usr/local/bin/locachy-build", "--trust-key"] + keys).returncode
        patch = mock.patch.object(i, "trust_install_keys", side_effect=trust)
        patch.start()
        self.addCleanup(patch.stop)

    APPS_URL = "https://example.org/locachy-apps.git"

    def test_preflight_unknown_keys_stop_before_partition_and_target_mutations(self):
        argv = ["locachy-install", "--disk=/dev/vdb", "--yes", "--features=howdy"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(i.os, "geteuid", return_value=0), \
             mock.patch.object(i, "check_choices"), mock.patch.object(i, "check_targets"), \
             mock.patch.object(i, "preflight_keys", return_value=3), \
             mock.patch.object(i, "prepare_partitions") as partition, mock.patch.object(i, "run") as run, \
             contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(i.main(), 3)
        partition.assert_not_called()
        run.assert_not_called()
        record = json.loads(self.record.read_text())
        self.assertEqual(record["phase"], "preflight")
        self.assertIn("--disk", record["argv"])
        self.assertNotIn("--password-file", record["argv"])
        self.assertIn("Continue with: locachy-install --resume", out.getvalue())
        # The record has no mounted target yet; a retry checks again, never
        # calls resumed_target or treats it as a partially installed disk.
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume", "--yes"]), \
             mock.patch.object(i.os, "geteuid", return_value=0), mock.patch.object(i, "check_choices"), \
             mock.patch.object(i, "check_targets"), mock.patch.object(i, "preflight_keys", return_value=1), \
             mock.patch.object(i, "resumed_target") as target, mock.patch.object(i, "prepare_partitions") as partition, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(i.main(), 1)
        target.assert_not_called()
        partition.assert_not_called()

    def test_preflight_exception_stops_before_target_write(self):
        argv = ["locachy-install", "--disk=/dev/vdb", "--yes"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(i.os, "geteuid", return_value=0), \
             mock.patch.object(i, "check_choices"), mock.patch.object(i, "check_targets"), \
             mock.patch.object(i, "preflight_keys", side_effect=RuntimeError("network unavailable")), \
             mock.patch.object(i, "prepare_partitions") as partition, mock.patch.object(i, "run") as run, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(i.main(), 1)
        partition.assert_not_called()
        run.assert_not_called()
        self.assertEqual(json.loads(self.record.read_text())["phase"], "preflight")

    def test_resume_never_replaces_an_explicit_empty_catalog_with_the_medium(self):
        args = self.args("--disk", "/dev/vdb")
        record = i.write_resume_record("a" * 32, "", i.safe_arguments(args), args, phase="preflight")
        self.assertIsNone(record["apps_catalog_path"])
        self.assertIsNone(record["apps_catalog_digest"])
        bundled = self.make_catalog({"medium-app": 'package="hello"\ncommand="/usr/bin/hello"\n'})
        with mock.patch.object(i, "MEDIUM_APPS_CATALOG", str(bundled)):
            i.use_resume_catalog(record)
            self.assertIsNone(i.apps_catalog())
            self.assertEqual(i.apps_meta(), {})
            self.assertNotIn("medium-app", i.presets())

    def test_resume_rejects_a_changed_catalog_before_key_check(self):
        self.make_catalog({"demo": 'package="hello"\ncommand="/usr/bin/hello"\n'})
        args = self.args("--disk", "/dev/vdb")
        record = i.write_resume_record("a" * 32, "", i.safe_arguments(args), args, phase="preflight")
        (Path(record["apps_catalog_path"]) / "demo.conf").write_text('package="fish"\ncommand="/usr/bin/fish"\n')
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume"]), \
             mock.patch.object(i.os, "geteuid", return_value=0), \
             mock.patch.object(i, "preflight_keys") as keys, mock.patch.object(i, "prepare_partitions") as partition, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(i.main(), 1)
        self.assertIn("pinned app catalog is missing or changed", err.getvalue())
        keys.assert_not_called()
        partition.assert_not_called()

    def test_operation_lock_brackets_inherited_worker(self):
        with i.installation_lock() as fd:
            with self.assertRaisesRegex(ValueError, "already running"):
                with i.installation_lock():
                    self.fail("a second installer obtained the lock")
            # Exercise the same inherited descriptor check in a fresh process.
            script = """
import importlib.machinery, importlib.util, os, pathlib, sys
loader = importlib.machinery.SourceFileLoader('fixture_backend', sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
backend = importlib.util.module_from_spec(spec)
loader.exec_module(backend)
backend.RESUME_RECORD = pathlib.Path(sys.argv[2])
with backend.installation_lock(private=True) as inherited:
    assert not os.get_inheritable(inherited)
"""
            subprocess.run([sys.executable, "-B", "-c", script, i.__file__, str(self.record)], check=True,
                           pass_fds=(fd,), env=dict(os.environ, LOCACHY_INSTALL_LOCK_FD=str(fd)))
            with self.assertRaisesRegex(ValueError, "already running"):
                with i.installation_lock():
                    self.fail("the worker released the parent's lock")
        with i.installation_lock():
            self.assertTrue(self.record.with_name("operation.lock").is_file())

    def test_resume_refuses_replaced_esp_before_preflight_or_build(self):
        args = self.args("--root", str(self.root), "--esp", str(self.esp))
        install_id = "a" * 32
        i.INSTALL_STATE.write_text(self.root / ".locachy-installer/install-id", install_id + "\n")
        i.write_resume_record(install_id, self.root, i.safe_arguments(args), args,
                              mounts=i.target_identities(self.root_fs, self.esp_fs))
        self.uuids[self.esp_fs["source"]] = "different-esp-uuid"
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume"]), \
             mock.patch.object(i.os, "geteuid", return_value=0), mock.patch.object(i.os.path, "ismount", return_value=True), \
             mock.patch.object(i, "preflight_keys") as keys, mock.patch.object(i, "run") as run, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(i.main(), 1)
        self.assertIn("ESP filesystem changed", err.getvalue())
        self.assertEqual(json.loads(self.record.read_text())["mounts"]["esp"], "esp-uuid")
        keys.assert_not_called()
        run.assert_not_called()
        self.assertFalse(any(self.esp.iterdir()))

    def test_preflight_success_is_read_only_and_trust_works_before_target_exists(self):
        argv = ["locachy-install", "--preflight", "--disk=/dev/vdb", "--yes"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(i.os, "geteuid", return_value=0), \
             mock.patch.object(i, "check_choices"), mock.patch.object(i, "check_targets"), \
             mock.patch.object(i, "prepare_partitions") as partition, mock.patch.object(i, "run") as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(i.main(), 0)
        partition.assert_not_called()
        run.assert_not_called()
        args = self.args("--disk=/dev/vdb")
        i.write_resume_record("a" * 32, "", i.safe_arguments(args), args, phase="preflight")
        key = "A" * 40
        with mock.patch.object(sys, "argv", ["locachy-install", "--trust-key", key]), \
             mock.patch.object(i.os, "geteuid", return_value=0), mock.patch.object(i, "resumed_target") as target, \
             mock.patch.object(i, "trust_install_keys", return_value=0) as trust:
            self.assertEqual(i.main(), 0)
        target.assert_not_called()
        trust.assert_called_once_with([key], None)

    def test_target_resume_record_cannot_smuggle_partition_or_format_options(self):
        args = self.args("--root=" + str(self.root), "--esp=" + str(self.esp))
        i.write_resume_record("a" * 32, self.root, ["--disk", "/dev/vdb"], args)
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume"]), \
             mock.patch.object(i.os, "geteuid", return_value=0), \
             mock.patch.object(i, "resumed_target", return_value=self.root), \
             mock.patch.object(i, "prepare_partitions") as partition, \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            i.main()
        partition.assert_not_called()

    def test_partitioning_resume_refuses_to_repeat_after_formatter_interruption(self):
        args = self.args("--disk=/dev/vdb", "--yes")
        i.write_resume_record("a" * 32, "", i.safe_arguments(args), args, phase="partitioning")
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume"]), \
             mock.patch.object(i.os, "geteuid", return_value=0), \
             mock.patch.object(i, "prepare_partitions") as partition, \
             contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
            i.main()
        partition.assert_not_called()
        self.assertIn("never repeats partitioning", err.getvalue())

    def test_complete_preflight_package_roots_include_modules_ime_and_selected_apps(self):
        self.make_catalog({"demo": 'package="demo-aur"\nextras="another-package"\ncommand="/usr/bin/demo"\n'}, fmt="2")
        args = self.args("--features=howdy", "--ime=zh", "--encrypt", "--containers=demo")
        roots = i.selected_package_roots(args)
        for name in ("howdy-git", "python-dlib", "linux-enable-ir-emitter-git", "cryptsetup",
                     "demo-aur", "another-package", "librime", "rime-frost-git", "noto-fonts-cjk"):
            self.assertIn(name, roots)
        self.assertIn("base", roots)

    def make_catalog(self, presets, commit="c" * 40, dest=None, origin="repository", fmt="1"):
        dest = dest or self.apps
        dest.parent.mkdir(parents=True, exist_ok=True)
        i.APPS.write_catalog({name: text.encode() for name, text in presets.items()}, str(dest),
                             i.APPS.new_meta(fmt, origin, self.APPS_URL, "next", commit))
        return dest

    def output(self, command):
        if command[0] == "findmnt":
            if "--mountpoint" in command:
                path = command[command.index("--mountpoint") + 1]
                return json.dumps({"filesystems": [m for m in self.mounts if m["target"] == path]})
            return json.dumps({"filesystems": self.mounts})
        if command[0] == "blkid":
            if "UUID" in command:
                return self.uuids[command[-1]]
            return self.label if "LABEL" in command else self.fat
        if command[0] == "lsblk":
            return "252:16\n252:18" if command[-1].startswith("/dev/vdb") else "8:0\n8:1"
        if command[0].endswith("ld-linux-x86-64.so.2"):
            return "  x86-64-v4\n  x86-64-v3 (supported, searched)\n  x86-64-v2 (supported, searched)"
        self.fail("Unexpected command: " + repr(command))

    def validate(self):
        return i.validate(str(self.root), str(self.esp))

    def test_continued_target_is_that_installations_only(self):
        work = self.root / ".locachy-installer"
        work.mkdir()
        (work / "install-id").write_text("a" * 32 + "\n")
        (self.root / "usr").mkdir()
        with self.assertRaisesRegex(ValueError, "Target root is not empty"):
            self.validate()  # not continued: empty only
        self.assertEqual(i.validate(str(self.root), str(self.esp), "a" * 32)[:2], (self.root, self.esp))
        with self.assertRaisesRegex(ValueError, "another installation's"):
            i.validate(str(self.root), str(self.esp), "b" * 32)
        # The ESP: what an installation writes there, else nothing.
        (self.esp / "EFI/BOOT").mkdir(parents=True)
        (self.esp / "EFI/BOOT/BOOTX64.EFI").write_text("image")
        i.validate(str(self.root), str(self.esp), "a" * 32)
        (self.esp / "EFI/Microsoft").mkdir()
        with self.assertRaisesRegex(ValueError, "no installation of LOCachy wrote"):
            i.validate(str(self.root), str(self.esp), "a" * 32)
        # No mark yet (it stopped before the build began): empty, as at first.
        (work / "install-id").unlink()
        with self.assertRaisesRegex(ValueError, "Target root is not empty"):
            i.validate(str(self.root), str(self.esp), "a" * 32)

    def test_empty_explicit_target_and_fat32_esp(self):
        (self.root / "lost+found").mkdir()
        self.assertEqual(self.validate()[:2], (self.root, self.esp))

    def test_cpu_auto_uses_supported_features_and_v1_alias(self):
        self.assertEqual(i.cpu_level("auto"), "x86-64-v3")
        self.assertEqual(i.cpu_level("x86-64-v1"), "x86-64")
        self.assertEqual(i.cpu_level("x86-64-v2"), "x86-64-v2")
        with self.assertRaisesRegex(ValueError, "cannot execute"):
            i.cpu_level("x86-64-v4")

    def test_cpu_model_is_gcc_native_else_the_level(self):
        native = "  -march=                     \t\tivybridge\n  -mtune=                     \t\tivybridge\n"
        macros = {"-march=ivybridge": "#define __AVX__ 1\n#define __AES__ 1\n",
                  "-march=skylake": "#define __AVX2__ 1\n#define __SGX__ 1\n",
                  "-march=native": "#define __AVX__ 1\n#define __AES__ 1\n#define __AVX2__ 1\n"}
        def gcc(command):
            return native if "--help=target" in command else macros[command[1]]
        with mock.patch.object(i, "output", gcc):
            self.assertEqual(i.cpu_model("x86-64-v2"), "ivybridge")
        # A name with more than this CPU (a VM's skylake without SGX): native itself.
        native = native.replace("ivybridge", "skylake")
        with mock.patch.object(i, "output", gcc):
            self.assertEqual(i.cpu_model("x86-64-v3"), "native")
        def no_gcc(command):
            raise FileNotFoundError(command[0])
        with mock.patch.object(i, "output", no_gcc):
            self.assertEqual(i.cpu_model("x86-64-v2"), "x86-64-v2")
        self.assertFalse(i.valid_cpu("ivy bridge"))

    def test_tpm_advice_from_locachy_tpm_status(self):
        def status(st):
            return lambda command: json.dumps(st)
        cases = [({"tpm_version": 0}, False, "passphrase", "no TPM"),
                 ({"tpm_version": 1, "tpm_active": False}, False, "passphrase", "TPM 1.2 off in the firmware"),
                 ({"tpm_version": 1, "tpm_active": True}, True, "passphrase", "TPM 1.2: a passphrase recommended"),
                 ({"tpm_version": 2, "tpm_active": True, "setup_mode": True}, True, "tpm",
                  "only for LOCachy's signed boot images"),
                 ({"tpm_version": 2, "tpm_active": True, "secure_boot": False}, True, "passphrase",
                  "binds PCRs 0+7, Secure Boot off, the boot image unsigned"),
                 ({"tpm_version": 2, "tpm_active": True, "secure_boot": True}, True, "passphrase",
                  "binds PCRs 0+7, the boot image unsigned")]
        for st, usable, recommended, said in cases:
            with mock.patch.object(i, "output", status(st)):
                advice = i.tpm_advice()
            self.assertEqual((advice[0], advice[1]), (usable, recommended), st)
            self.assertIn(said, advice[2])
            self.assertNotIn("PIN at", advice[2])
        def broken(command):
            raise subprocess.CalledProcessError(1, command)
        with mock.patch.object(i, "output", broken):
            self.assertFalse(i.tpm_advice()[0])

    BLOCK = {"blockdevices": [
        {"path": "/dev/sda", "maj:min": "8:0", "type": "disk", "size": 256 << 30, "model": "Samsung SSD ",
         "tran": "sata", "rm": False, "ro": False, "fstype": None, "mountpoints": [None], "children": [
             {"path": "/dev/sda1", "maj:min": "8:1", "type": "part", "size": 512 << 20, "fstype": "vfat",
              "label": "EFI", "parttypename": "EFI System", "mountpoints": [None]},
             {"path": "/dev/sda2", "maj:min": "8:2", "type": "part", "size": 8 << 30, "fstype": "apfs",
              "label": None, "parttypename": "Apple APFS", "mountpoints": [None]},
             {"path": "/dev/sda3", "maj:min": "8:3", "type": "part", "size": 200 << 30, "fstype": "ext4",
              "label": "data", "parttypename": "Linux filesystem", "mountpoints": [None]}]},
        {"path": "/dev/sdb", "maj:min": "8:16", "type": "disk", "size": 16 << 30, "model": "USB stick",
         "tran": "usb", "rm": True, "ro": False, "mountpoints": [None], "children": [
             {"path": "/dev/sdb1", "maj:min": "8:17", "type": "part", "size": 1 << 30, "fstype": "iso9660",
              "mountpoints": ["/run/archiso/bootmnt"]}]},
        {"path": "/dev/sdc", "maj:min": "8:32", "type": "disk", "size": 8 << 30, "model": "small",
         "rm": False, "ro": False, "mountpoints": [None]},
        {"path": "/dev/loop0", "maj:min": "7:0", "type": "loop", "size": 1 << 30, "mountpoints": ["/run/archiso/airootfs"]},
        {"path": "/dev/zram0", "maj:min": "252:0", "type": "disk", "size": 4 << 30, "mountpoints": ["[SWAP]"]},
        {"path": "/dev/sr0", "maj:min": "11:0", "type": "rom", "size": 1 << 30, "ro": True, "mountpoints": [None]}]}

    def block_output(self, command):
        if command[0] == "findmnt":
            return json.dumps({"filesystems": [{"target": "/", "source": "airootfs"},
                                               {"target": "/run/archiso/bootmnt", "source": "/dev/sdb1"}]})
        if command[:2] == ["lsblk", "--inverse"]:
            return {"/dev/sdb1": "8:17\n8:16", "/dev/sdb": "8:16", "/dev/sda": "8:0", "/dev/sdc": "8:32"}.get(
                command[-1], command[-1].replace("/dev/sda", "8:") + "\n8:0")
        if command[:2] == ["lsblk", "--json"]:
            def find(nodes, path):
                for n in nodes:
                    if n["path"] == path:
                        return n
                    found = find(n.get("children") or [], path)
                    if found:
                        return found
            if not command[-1].startswith("/dev/"):
                return json.dumps(self.BLOCK)
            node = find(self.BLOCK["blockdevices"], command[-1])
            if not node:
                raise subprocess.CalledProcessError(32, command)
            return json.dumps({"blockdevices": [node]})
        return self.output(command)

    def args(self, *argv):
        return i.make_parser().parse_args(list(argv))

    def test_describe_lists_choices_defaults_and_disks_but_not_the_live_medium(self):
        self.make_catalog({"chatgpt": 'host="1"\npackage="chatgpt-desktop"\ncommand="/usr/bin/chatgpt"\n',
                           "firefox": 'package="firefox"\ncommand="/usr/bin/firefox"\n'}, fmt="2")
        with mock.patch.object(i, "output", self.block_output), \
             mock.patch.object(i, "cpu_model", lambda level: "ivybridge"), \
             mock.patch.object(i, "tpm_status", lambda: {"tpm_version": 1}), \
             mock.patch.object(i, "host_security", lambda: {"id": "HSI:1 (v2.0.16)", "checks": []}), \
             mock.patch.object(i, "tpm_advice", lambda st=None: (True, "passphrase", "TPM 1.2: a passphrase recommended")):
            d = json.loads(json.dumps(i.describe(i.make_parser())))
        self.assertEqual(d["host_security"]["id"], "HSI:1 (v2.0.16)")
        self.assertEqual([x["path"] for x in d["disks"]], ["/dev/sda", "/dev/sdc"])
        sda = d["disks"][0]
        self.assertEqual((sda["model"], sda["tran"], sda["in_use"]), ("Samsung SSD", "sata", ""))
        self.assertEqual([(c["path"], c["fstype"], c["parttypename"]) for c in sda["children"]],
                         [("/dev/sda1", "vfat", "EFI System"), ("/dev/sda2", "apfs", "Apple APFS"),
                          ("/dev/sda3", "ext4", "Linux filesystem")])
        self.assertEqual(d["levels"], {"supported": ["x86-64", "x86-64-v2", "x86-64-v3"],
                                       "auto": "x86-64-v3", "cpu_model": "ivybridge"})
        self.assertEqual(d["defaults"]["user"], "user")
        self.assertEqual(d["defaults"]["swap_gib"], 8)
        self.assertEqual(d["defaults"]["repos"], ["core", "extra", "multilib"])
        self.assertTrue(d["tpm"]["usable"])
        self.assertIn("cjk-fonts", d["module_defaults"]["zh_CN.UTF-8"])
        self.assertNotIn("cjk-fonts", d["module_defaults"]["en_US.UTF-8"])
        # The system's host programs first, then the app presets' catalog.
        self.assertEqual(d["containers"], [{"name": "code", "host": True}, {"name": "foot", "host": True},
                                           {"name": "chatgpt", "host": True}, {"name": "firefox", "host": False}])
        self.assertEqual((d["defaults"]["apps_repo"], d["defaults"]["apps_branch"]), (self.APPS_URL, "next"))
        self.assertEqual(d["apps"], {"text": f"{'c' * 12} of {self.APPS_URL} (next)", "not_newest": False})
        self.assertEqual(d["machines"][0], {"name": "generic", "description": "Generic PC"})
        self.assertIn("x230-agan", [m["name"] for m in d["machines"]])
        self.assertEqual([x["name"] for x in d["ime"]], ["off", "en", "zh"])
        self.assertEqual(d["timezones"][0], "UTC")

    def test_host_packages_come_from_the_selected_catalog_and_its_extras(self):
        chatgpt = 'host="1"\npackage="chatgpt-desktop"\nextras="git ripgrep"\ncommand="/usr/bin/chatgpt"\n'
        self.make_catalog({"chatgpt": chatgpt, "isolated": 'package="hello"\ncommand="/usr/bin/hello"\n'}, fmt="2")
        self.assertEqual(i.host_preset_packages(["chatgpt", "isolated", "foot"]),
                         ["chatgpt-desktop", "git", "ripgrep", "foot"])
        root = self.root / "target-custom"
        (root / "etc/locachy").mkdir(parents=True)
        (root / "etc/locachy/packages").write_text("base\n")
        (root / "etc/locachy/rc.conf").write_text("")
        # Continued installations use their own pinned catalog, not a newer
        # live catalog whose package or access mode may have changed.
        pinned = self.make_catalog({"chatgpt": chatgpt.replace("chatgpt-desktop", "chatgpt-pinned")},
                                   dest=root / i.APPS_CATALOG, fmt="2")
        args = type("Args", (), {"containers": "chatgpt", "features": ""})()
        self.assertTrue(i.apply_customizations(args, root))
        manifest = (root / "etc/locachy/packages").read_text().splitlines()
        self.assertTrue({"chatgpt-pinned", "git", "ripgrep"} <= set(manifest))
        self.assertNotIn("chatgpt-desktop", manifest)
        self.assertIn('host="1"', (root / "etc/locachy/containers/chatgpt.conf").read_text())
        self.assertEqual(i.APPS.read_meta(str(pinned))["format"], "2")

    def test_targets_checked_before_anything_is_written(self):
        with mock.patch.object(i, "output", self.block_output):
            i.check_targets(self.args("--disk", "/dev/sda"))
            for argv, said in ((["--disk", "/dev/sdb"], "live medium"),
                               (["--disk", "/dev/sdc"], "at least 25 GiB"),
                               (["--disk", "/dev/sda1"], "not a whole disk"),
                               (["--disk", "sda"], "not a /dev/"),
                               (["--disk", "/dev/sdz"], "no such block device"),
                               (["--format-esp", "/dev/sda1", "--format-root", "/dev/sda2"], "at least 16 GiB"),
                               (["--format-esp", "/dev/sda1", "--format-root", "/dev/sda1"], "must be different"),
                               (["--format-esp", "/dev/sda", "--format-root", "/dev/sda3"], "not a partition")):
                with self.assertRaisesRegex(ValueError, said, msg=argv):
                    i.check_targets(self.args(*argv))
            i.check_targets(self.args("--format-esp", "/dev/sda1", "--format-root", "/dev/sda3", "--format-swap", "/dev/sda2"))
            self.BLOCK["blockdevices"][0]["children"][2]["mountpoints"] = ["/mnt"]
            try:
                with self.assertRaisesRegex(ValueError, r"/dev/sda3 is in use \(/mnt\)"):
                    i.check_targets(self.args("--disk", "/dev/sda"))
            finally:
                self.BLOCK["blockdevices"][0]["children"][2]["mountpoints"] = [None]

    def test_choices_checked_before_the_build(self):
        keymaps = Path(self.tmp.name) / "keymaps/i386/qwerty"
        keymaps.mkdir(parents=True)
        (keymaps / "us.map.gz").write_bytes(b"")
        (keymaps / "de-latin1.map.gz").write_bytes(b"")
        zones = Path(self.tmp.name) / "zoneinfo/Asia"
        zones.mkdir(parents=True)
        (zones / "Shanghai").write_bytes(b"TZif2")
        secret = Path(self.tmp.name) / "secret"
        with mock.patch.object(i, "KEYMAPS", str(keymaps.parents[1])), \
             mock.patch.object(i, "ZONEINFO", str(zones.parent)):
            self.assertEqual(i.keymaps(), ["de-latin1", "us"])
            i.check_choices(self.args("--keymap", "de-latin1", "--timezone", "Asia/Shanghai", "--machine", "x230-agan"))
            secret.write_text("correct horse\n")
            i.check_choices(self.args("--password-file", str(secret), "--encrypt", "--tpm",
                                      "--luks-passphrase-file", str(secret)))
            # An app is a host program or in the catalog; an unknown one stops
            # the installation before a disk is touched, never a guess.
            with self.assertRaisesRegex(ValueError, "^Unknown app: firefox$"):
                i.check_choices(self.args("--containers", "foot,firefox"))
            self.make_catalog({"firefox": 'package="firefox"\ncommand="/usr/bin/firefox"\n'})
            i.check_choices(self.args("--containers", "foot,firefox"))
            with self.assertRaisesRegex(ValueError, "^Unknown app: qq$"):
                i.check_choices(self.args("--containers", "foot,qq"))
            for argv, said in ((["--apps-repo", "ftp://x"], "App presets repository must be"),
                               (["--apps-branch", "a..b"], "Invalid app presets branch")):
                with self.assertRaisesRegex(ValueError, said):
                    i.apps_source(self.args(*argv))
            for argv, said in ((["--hostname=-bad-"], "hostname"), (["--keymap", "dvorak-x"], "keymap"),
                               (["--timezone", "Mars/Olympus"], "timezone"), (["--features", "desktop,nope"], "module"),
                               (["--containers", "a b"], "container"), (["--machine", "x999"], "machine"),
                               (["--full-name", "A:B"], "colon"), (["--hibernate-after", "-1"], "seconds"),
                               (["--tpm"], "needs --encrypt"),
                               (["--secure-boot-microsoft"], "goes with --secure-boot")):
                with self.assertRaisesRegex(ValueError, said, msg=argv):
                    i.check_choices(self.args(*argv))
            for text in ("", "\n", "two\nlines\n"):
                secret.write_text(text)
                with self.assertRaisesRegex(ValueError, "password-file"):
                    i.check_choices(self.args("--password-file", str(secret)))

    def test_password_file_reaches_the_build_as_a_path_only(self):
        secret = Path(self.tmp.name) / "password"
        secret.write_text("Locachy123\n")
        calls = []
        argv = ["locachy-install", "--root", str(self.root), "--esp", str(self.esp), "--yes",
                "--password-file", str(secret), "--user", "boot-test"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(i.os, "geteuid", lambda: 0), \
             mock.patch.object(i, "validate", lambda r, e, install_id=None: (self.root, self.esp, self.root_fs, self.esp_fs)), \
             mock.patch.object(i, "run", lambda cmd, **kw: calls.append(cmd) or subprocess.CompletedProcess(cmd, 0)), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(i.main(), 0)
        inner = calls[-1]
        self.assertEqual(inner[0], "unshare")
        self.assertEqual(inner[inner.index("--password-file") + 1], str(secret))
        self.assertFalse(any("Locachy123" in a for a in inner))
        self.assertFalse(self.record.exists())  # done: nothing to continue
        # The inner run fails: its own last line said why, nothing is added;
        # stopped by a signal, that is said.
        for status, said in ((1, ""), (-9, "locachy-install: the installation was stopped by signal 9\n")):
            err = io.StringIO()
            with mock.patch.object(sys, "argv", argv), mock.patch.object(i.os, "geteuid", lambda: 0), \
                 mock.patch.object(i, "validate", lambda r, e, install_id=None: (self.root, self.esp, self.root_fs, self.esp_fs)), \
                 mock.patch.object(i, "run", lambda cmd, **kw: subprocess.CompletedProcess(cmd, status)), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                self.assertEqual(i.main(), 1)
            self.assertEqual(err.getvalue(), said)
        # Without --esp: an error, not a traceback.
        with mock.patch.object(sys, "argv", ["locachy-install", "--root", str(self.root), "--yes"]), \
             mock.patch.object(i.os, "geteuid", lambda: 0), self.assertRaises(SystemExit):
            i.main()

    def test_face_module_installs_what_settings_and_the_lock_use(self):
        # The lock's PAM file is written for howdy-git (pam_howdy.so) and
        # linux-enable-ir-emitter-git (7.x: run --config, a TOML file); settings
        # installs those. The installer's module must be the same packages: the
        # releases (howdy 2.x, linux-enable-ir-emitter 6.x on OpenCV 4) are not.
        settings = (SOURCE / "progs/settings.c").read_text(encoding="utf-8")
        face = settings.split('#define FACE_PACKAGES "', 1)[1].split('"', 1)[0].split()
        self.assertEqual(face, ["howdy-git", "linux-enable-ir-emitter-git"])
        self.assertEqual([p for p in i.MODULES["howdy"]["packages"] if p != "python-dlib"], face)
        pam = (SOURCE / "link/pam.d/locachy-lock-face").read_text(encoding="utf-8")
        self.assertIn("pam_howdy.so", pam)
        self.assertIn("linux-enable-ir-emitter run --config", pam)

    def test_repo_sections_in_their_order_and_known_only(self):
        self.assertEqual(i.repo_sections("multilib,core"), ["core", "multilib"])
        self.assertEqual(i.repo_sections(""), [])
        with self.assertRaisesRegex(ValueError, "testing"):
            i.repo_sections("core,testing")

    def test_nonempty_root_or_esp_rejected(self):
        (self.root / "important").write_text("not ours")
        with self.assertRaisesRegex(ValueError, "not empty"):
            self.validate()
        (self.root / "important").unlink()
        (self.esp / "existing.efi").write_text("not ours")
        with self.assertRaisesRegex(ValueError, "ESP must be empty"):
            self.validate()

    def test_nonempty_lost_found_is_not_silently_accepted(self):
        directory = self.root / "lost+found"
        directory.mkdir()
        (directory / "recovered").write_text("not ours")
        with self.assertRaisesRegex(ValueError, "not empty"):
            self.validate()

    def test_wrong_labels_filesystems_or_readonly_rejected(self):
        self.label = "someone-else"
        with self.assertRaisesRegex(ValueError, "locachy-root"):
            self.validate()
        self.label = "locachy-root"
        self.fat = "FAT16"
        with self.assertRaisesRegex(ValueError, "FAT32"):
            self.validate()
        self.fat = "FAT32"
        self.root_fs["options"] = "ro"
        with self.assertRaisesRegex(ValueError, "Writable"):
            self.validate()
        self.root_fs["options"] = "rw"
        self.root_fs["fstype"] = "xfs"
        with self.assertRaisesRegex(ValueError, "ext4"):
            self.validate()

    def test_unmounted_path_and_alias_are_rejected(self):
        self.mounts.remove(self.root_fs)
        with self.assertRaisesRegex(ValueError, "explicit filesystem"):
            self.validate()
        alias = self.root.parent / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            i.validate(str(alias), str(alias / "efi"))

    def test_live_disk_and_duplicate_mount_are_rejected(self):
        self.mounts.append(dict(self.root_fs, target="/other"))
        with self.assertRaisesRegex(ValueError, "also mounted"):
            self.validate()
        self.mounts.pop()
        self.mounts.append({"target": "/run/archiso/bootmnt", "source": "/dev/vdb3",
                            "fstype": "iso9660", "maj:min": "252:19"})
        with self.assertRaisesRegex(ValueError, "live medium"):
            self.validate()

    def test_nested_mounts_and_same_root_esp_device_are_rejected(self):
        self.mounts.append({"target": str(self.root / "home"), "source": "/dev/vdc1",
                            "fstype": "ext4", "maj:min": "253:1"})
        with self.assertRaisesRegex(ValueError, "nested mount"):
            self.validate()
        self.mounts.pop()
        self.esp_fs["maj:min"] = self.root_fs["maj:min"]
        with self.assertRaisesRegex(ValueError, "different filesystems"):
            self.validate()

    def test_update_repo_and_branch(self):
        origin = self.root.parent / "source-origin.json"
        origin.write_text('{"repo": "https://example.org/fork/locachy.git", "branch": "next", "commit": "' + "a" * 40 + '"}\n')
        self.assertEqual(i.detect_source_git(self.root.parent, origin),
                         ("https://example.org/fork/locachy.git", "next", "a" * 40))
        origin.write_text('{"repo": "file:///x", "branch": "--upload-pack=x"}\n')
        self.assertEqual(i.detect_source_git(self.root.parent, origin)[:2], (i.DEFAULT_UPDATE_REPO, "main"))
        args = type("A", (), {"update_repo": "git@example.org:me/locachy.git", "update_branch": "release/1"})()
        self.assertEqual(i.update_source(args)[:2], ("git@example.org:me/locachy.git", "release/1"))
        for repo, branch in (("ftp://x", ""), ("https://x y", ""), ("", "a..b"), ("", "-x"), ("", "x.lock")):
            with self.assertRaises(ValueError):
                i.update_source(type("A", (), {"update_repo": repo, "update_branch": branch})())
        rc = self.root / "etc/locachy/rc.conf"
        rc.parent.mkdir(parents=True)
        rc.write_text('update_repo="https://github.com/niuyuanzhao/locachy.git"\nupdate_branch="old"\n')
        base = {"encrypt": False, "language": "en_US.UTF-8", "features": "", "containers": ""}
        i.apply_customizations(type("A", (), dict(base, update_repo="off", update_branch="dev", apps_repo="off",
                                                  apps_branch="dev"))(), self.root)
        text = rc.read_text()
        self.assertIn('update_repo="off"', text)
        self.assertIn('update_branch="dev"', text)
        self.assertEqual(text.count("update_branch="), 1)
        self.assertIn('apps_repo="off"\napps_branch="dev"\n', text)
        with mock.patch.object(i, "detect_source_git", return_value=(i.DEFAULT_UPDATE_REPO, "main", "")):
            i.apply_customizations(type("A", (), base)(), self.root)
        text = rc.read_text()
        self.assertNotIn("update_branch=", text)
        self.assertNotIn("apps_branch=", text)  # main: not written
        self.assertEqual(text.count("apps_repo="), 1)
        self.assertIn(f'apps_repo="{i.DEFAULT_APPS_REPO}"', text)  # no catalog, no recorded origin

    def test_source_line_says_which_source_and_why(self):
        url, commit = "https://example.org/locachy.git", "0123456789ab" + "c" * 28
        with mock.patch.object(i, "detect_source_git", return_value=(url, "main", commit)):
            for how, said in (("fetched", (f"0123456789ab of {url} (main), the newest, fetched now", False)),
                              ("fetched earlier", (f"0123456789ab of {url} (main), fetched earlier", False)),
                              ("fetched before: could not fetch x (main): fatal: no",
                               (f"0123456789ab of {url} (main), fetched before; could not fetch x (main): fatal: no",
                                True)),
                              ("bundled: updates are off", ("this medium's own, 0123456789ab (updates are off)", False)),
                              ("bundled: could not fetch x (main): fatal: no",
                               ("this medium's own, 0123456789ab; could not fetch x (main): fatal: no", True)),
                              ("", (f"0123456789ab of {url} (main)", False))):
                with mock.patch.dict(os.environ, {"LOCACHY_SOURCE": how}):
                    self.assertEqual(i.source_line(), said, how)
            with mock.patch.dict(os.environ, {"LOCACHY_SOURCE": "bundled: could not fetch x (main): fatal: no"}), \
                 mock.patch.object(i, "output", self.block_output), \
                 mock.patch.object(i, "cpu_model", lambda level: "ivybridge"), \
                 mock.patch.object(i, "tpm_status", lambda: {"tpm_version": 1}), \
                 mock.patch.object(i, "host_security", lambda: None), \
                 mock.patch.object(i, "tpm_advice", lambda st=None: (False, "off", "no TPM")):
                self.assertEqual(i.describe(i.make_parser())["source"],
                                 {"text": "this medium's own, 0123456789ab; could not fetch x (main): fatal: no",
                                  "not_newest": True})

    def test_apps_line_says_which_catalog_and_why(self):
        short = "c" * 12
        with mock.patch.dict(os.environ, {"LOCACHY_APPS": "bundled: could not fetch x (main): fatal: no"}):
            self.assertEqual(i.apps_line(), ("none; could not fetch x (main): fatal: no", True))
        self.make_catalog({"firefox": 'package="firefox"\ncommand="/usr/bin/firefox"\n'})
        for how, said in (("fetched", (f"{short} of {self.APPS_URL} (next), the newest, fetched now", False)),
                          ("fetched earlier", (f"{short} of {self.APPS_URL} (next), fetched earlier", False)),
                          ("fetched before: could not fetch x (next): fatal: no",
                           (f"{short} of {self.APPS_URL} (next), fetched before; could not fetch x (next): fatal: no",
                            True)),
                          ("bundled: updates are off", (f"this medium's own, {short} (updates are off)", False)),
                          ("bundled: x (next): not compatible: format 2",
                           (f"this medium's own, {short}; x (next): not compatible: format 2", True))):
            with mock.patch.dict(os.environ, {"LOCACHY_APPS": how}):
                self.assertEqual(i.apps_line(), said, how)
        with mock.patch.dict(os.environ, {"LOCACHY_APPS": "fetched"}), contextlib.redirect_stdout(io.StringIO()) as out, \
                mock.patch.object(i, "detect_source_git", return_value=("https://example.org/l.git", "main", "")):
            i.print_source()
        self.assertIn(f"App presets: {short} of {self.APPS_URL} (next), the newest, fetched now\n", out.getvalue())
        # No catalog repository recorded: what the medium recorded, else the default.
        origin = Path(self.tmp.name) / "source-origin.json"
        origin.write_text(json.dumps({"apps_repo": "https://example.org/fork-apps.git", "apps_branch": "dev"}))
        with mock.patch.dict(os.environ, {"LOCACHY_APPS_CATALOG": str(Path(self.tmp.name) / "none")}):
            self.assertEqual(i.detect_apps_git(origin)[:2], ("https://example.org/fork-apps.git", "dev"))
            self.assertEqual(i.detect_apps_git(origin.parent / "missing.json")[:2], (i.DEFAULT_APPS_REPO, "main"))

    def test_a_stopped_installation_is_continued_where_it_stopped(self):
        first, again = Path(self.tmp.name) / "password", Path(self.tmp.name) / "password-again"
        first.write_text("Locachy123\n")
        again.write_text("Locachy123\n")
        key = "40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"
        calls, checked = [], []
        def validate(r, e, install_id=None):
            checked.append(install_id)
            return self.root, self.esp, self.root_fs, self.esp_fs
        def stopped(cmd, **kw):
            calls.append(cmd)
            # The build stopped for a key (locachy-build wrote it down).
            keys = self.root / i.UNTRUSTED_KEYS
            keys.parent.mkdir(parents=True, exist_ok=True)
            keys.write_text(f"lyrebird-proxy {key}\n")
            return subprocess.CompletedProcess(cmd, 1)
        out = io.StringIO()
        firefox = {"firefox": 'package="firefox"\ncommand="/usr/bin/firefox"\n'}
        self.make_catalog(firefox, commit="d" * 40)
        argv = ["locachy-install", "--root", str(self.root), "--esp", str(self.esp), "--yes", "--user", "boot-test",
                "--features", "tor", "--password-file", str(first), "--update-repo", "https://example.org/l.git",
                "--apps-repo", "https://example.org/a.git", "--containers", "firefox"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(i.os, "geteuid", lambda: 0), \
             mock.patch.object(i, "validate", validate), mock.patch.object(i, "run", stopped), \
             contextlib.redirect_stdout(out):
            self.assertEqual(i.main(), 1)
        said = out.getvalue()
        self.assertIn(f"Source key to trust: lyrebird-proxy {key}", said)
        self.assertIn(f"Trust them: locachy-install --trust-key {key}\n", said)
        self.assertIn("Continue with: locachy-install --resume\n", said)
        record = json.loads(self.record.read_text())
        self.assertEqual(record["schema"], 2)
        self.assertEqual(record["phase"], "target")
        self.assertIn("source", record)
        install_id = record["install_id"]
        self.assertRegex(install_id, "^[0-9a-f]{32}$")
        self.assertEqual(oct(self.record.stat().st_mode & 0o777), "0o600")
        self.assertEqual(record["update_repo"], "https://example.org/l.git")
        self.assertEqual((record["apps_repo"], record["apps_commit"]), ("https://example.org/a.git", "d" * 40))
        self.assertIn("--features", record["argv"])
        self.assertNotIn("--password-file", record["argv"])  # never a secret, nor where it was
        self.assertEqual(calls[-1][calls[-1].index("--install-id") + 1], install_id)
        self.assertIn("--resume", calls[-1])  # first private build already has a target-local marker
        self.assertEqual((self.root / ".locachy-installer/install-id").read_text().strip(), install_id)
        self.assertEqual(record["mounts"], {"root": "root-uuid", "esp": "esp-uuid"})
        self.assertEqual(checked, [None])
        # --trust-key: the keys, in that target (its locachy-build asks).
        chroot = []
        with mock.patch.object(sys, "argv", ["locachy-install", "--trust-key", key]), \
             mock.patch.object(i.os, "geteuid", lambda: 0), \
             mock.patch.object(i, "resumed_target", lambda record: Path(record["root"])), \
             mock.patch.object(i.subprocess, "run", lambda cmd, **kw: chroot.append(cmd) or subprocess.CompletedProcess(cmd, 0)):
            self.assertEqual(i.main(), 0)
        self.assertEqual(chroot, [["arch-chroot", str(self.root), "/usr/local/bin/locachy-build", "--trust-key", key]])
        # The app presets it started with, kept beside its record.
        kept = self.record.with_name("apps-catalog." + install_id)
        self.assertEqual(i.APPS.read_meta(kept)["commit"], "d" * 40)
        # --resume before its target has a catalog: those kept, not this run's
        # (a newer one, without firefox); stopped again, both stay.
        shutil.rmtree(self.apps)
        self.make_catalog({"gimp": 'package="gimp"\ncommand="/usr/bin/gimp"\n'}, commit="e" * 40)
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume", "--yes", "--password-file", str(again)]), \
             mock.patch.object(i.os, "geteuid", lambda: 0), \
             mock.patch.object(i, "resumed_target", lambda record: Path(record["root"])), \
             mock.patch.object(i, "validate", validate), mock.patch.object(i, "run", stopped), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(i.main(), 1)
        self.assertEqual(os.environ["LOCACHY_APPS_CATALOG"], str(kept))
        self.assertEqual(json.loads(self.record.read_text())["apps_commit"], "d" * 40)
        self.assertEqual(i.APPS.read_meta(kept)["commit"], "d" * 40)
        # --resume: its options as recorded and the password again; the same
        # install ID and app snapshot, even after the target got its own copy.
        calls.clear()
        os.environ["LOCACHY_APPS_CATALOG"] = str(self.apps)
        shutil.rmtree(self.apps)
        self.make_catalog({}, commit="e" * 40)  # this run's: no firefox
        self.make_catalog(firefox, commit="d" * 40, dest=self.root / i.APPS_CATALOG)
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume", "--yes", "--password-file", str(again)]), \
             mock.patch.object(i.os, "geteuid", lambda: 0), \
             mock.patch.object(i, "resumed_target", lambda record: Path(record["root"])), \
             mock.patch.object(i, "validate", validate), \
             mock.patch.object(i, "run", lambda cmd, **kw: calls.append(cmd) or subprocess.CompletedProcess(cmd, 0)), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(i.main(), 0)
        inner = calls[-1]
        self.assertEqual(inner[inner.index("--install-id") + 1], install_id)
        self.assertIn("--resume", inner)
        self.assertEqual(inner[inner.index("--password-file") + 1], str(again))
        self.assertEqual(inner[inner.index("--features") + 1], "tor")
        self.assertEqual(inner[inner.index("--update-repo") + 1], "https://example.org/l.git")
        self.assertEqual(inner[inner.index("--apps-repo") + 1], "https://example.org/a.git")
        self.assertEqual(os.environ["LOCACHY_APPS_CATALOG"], str(kept))  # the recorded snapshot wins
        self.assertEqual(checked[-1], install_id)
        self.assertFalse(self.record.exists())
        self.assertFalse(kept.exists())
        # Nothing to continue now.
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume"]), \
             mock.patch.object(i.os, "geteuid", lambda: 0), contextlib.redirect_stderr(io.StringIO()) as err, \
             self.assertRaises(SystemExit):
            i.main()
        self.assertIn("Nothing to continue", err.getvalue())

    def test_an_older_medium_s_installer_calls_this_one(self):
        """The live entry fetches the newest locachy-install; the full-screen
        installer calling it is the medium's, maybe older. Its calls exactly
        as installer.c of 146a90a makes them (the medium of 2026-10-27), on
        such a medium: no app presets' catalog (it predates them) and no
        LOCACHY_APPS_CATALOG (its live entry fetches none). The installation
        stops for a key; its trust command is this installer's; c (--resume)
        goes on where it stopped, also past an empty catalog an older
        installer kept beside the record."""
        os.environ.pop("LOCACHY_APPS_CATALOG")
        os.environ.pop("LOCACHY_APPS")
        medium = Path(self.tmp.name) / "medium-without-app-presets"
        key = "40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"
        secret = Path(self.tmp.name) / "password"
        secret.write_text("Locachy123\n")
        calls, checked = [], []
        def validate(r, e, install_id=None):
            checked.append(install_id)
            return self.root, self.esp, self.root_fs, self.esp_fs
        def stopped(cmd, **kw):
            calls.append(cmd)
            keys = self.root / i.UNTRUSTED_KEYS
            keys.parent.mkdir(parents=True, exist_ok=True)
            keys.write_text(f"lyrebird-proxy {key}\n")
            return subprocess.CompletedProcess(cmd, 1)
        def call(argv, run, out=None):
            with mock.patch.object(sys, "argv", ["/usr/local/bin/locachy-install"] + argv), \
                 mock.patch.object(i, "MEDIUM_APPS_CATALOG", str(medium)), \
                 mock.patch.object(i.os, "geteuid", lambda: 0), mock.patch.object(i, "validate", validate), \
                 mock.patch.object(i, "resumed_target", lambda record: Path(record["root"])), \
                 mock.patch.object(i, "run", run), contextlib.redirect_stdout(out or io.StringIO()):
                return i.main()
        repo = "--update-repo=https://github.com/niuyuanzhao/locachy.git"
        first = ["--yes", "--root=" + str(self.root), "--esp=" + str(self.esp), "--password-file=" + str(secret),
                 "--user=boot-test", "--hostname=locachy", "--language=en_US.UTF-8", "--keymap=us",
                 "--timezone=auto", "--ime=off", "--copy-network", "--features=desktop,tor", "--containers=foot",
                 "--level=auto", "--cpu=auto", "--repos=core,extra,multilib", repo, "--update-branch=main"]
        out = io.StringIO()
        self.assertEqual(call(first, stopped, out), 1)
        trust = [line for line in out.getvalue().splitlines() if line.startswith("Trust them: ")]
        self.assertEqual(trust[-1], f"Trust them: locachy-install --trust-key {key}")  # what that installer shows
        self.assertFalse(self.record.with_name("apps-catalog").exists())  # none was there to keep
        chroot = []
        with mock.patch.object(i.subprocess, "run", lambda cmd, **kw: chroot.append(cmd) or subprocess.CompletedProcess(cmd, 0)):
            self.assertEqual(call(["--trust-key", key], stopped), 0)
        self.assertEqual(chroot, [["arch-chroot", str(self.root), "/usr/local/bin/locachy-build", "--trust-key", key]])
        # c: as that installer continues. An older installer (930434b) kept an
        # empty catalog beside the record; it is not taken for one.
        kept = self.record.with_name("apps-catalog")
        kept.mkdir()
        (kept / "CATALOG").write_text('installed="2026-11-05 10:00"\n')
        again = ["--resume", "--yes", "--password-file=" + str(secret), repo, "--update-branch=main"]
        calls.clear()
        self.assertEqual(call(again, lambda cmd, **kw: calls.append(cmd) or subprocess.CompletedProcess(cmd, 0)), 0)
        inner = calls[-1]
        self.assertIn("--resume", inner)
        self.assertEqual(inner[inner.index("--features") + 1], "desktop,tor")
        self.assertEqual(inner[inner.index("--containers") + 1], "foot")
        self.assertEqual(checked[-1], inner[inner.index("--install-id") + 1])  # the same installation
        self.assertFalse(self.record.exists())

    def test_host_security_is_fwupds_own_report(self):
        report = ("Host Security ID: \x1b[1mHSI:1 (v2.0.16)\x1b[0m\n\n\x1b[1mHSI-1\x1b[0m\n"
                  "\u2714 BIOS firmware updates:     \x1b[32mEnabled\x1b[0m\n"
                  "\u2718 Secure Boot:                \x1b[31mDisabled\x1b[0m\n"
                  "\nThis system has a low HSI security level.\n")
        seen = []
        def fwupdmgr(command, **kw):
            seen.append(command)
            return subprocess.CompletedProcess(command, 0, report, "")
        with mock.patch.object(i.shutil, "which", lambda name: "/usr/bin/" + name), \
                mock.patch.object(i.subprocess, "run", fwupdmgr):
            self.assertEqual(i.host_security(), {"id": "HSI:1 (v2.0.16)", "checks": [
                "\u2714 BIOS firmware updates:     Enabled", "\u2718 Secure Boot:                Disabled"]})
        self.assertEqual(seen, [["fwupdmgr", "security", "--no-unreported-check", "--no-security-fix",
                                 "--no-reboot-check"]])
        with mock.patch.object(i.shutil, "which", lambda name: None):
            self.assertIsNone(i.host_security())

    def test_secure_boot_keys_go_in_after_the_signed_image_is_on_the_esp(self):
        """Keys made before the image (signed), enrolled once it is on the ESP;
        TPM 2.0 then prepared for the first start with Secure Boot on, or bound
        as without the keys when they did not go in."""
        script = (SOURCE / "link/locachy-install").read_text()
        self.assertLess(script.index('"/usr/local/bin/locachy-tpm", "secure-boot"])'), script.index("boot_cmd = "))
        self.assertLess(script.index('shutil.copyfile(image, fallback / "BOOTX64.EFI")'),
                        script.index("secure_boot_install(args, root, saved)"))
        esp = self.root.parent / "esp"
        esp.mkdir()
        secret = Path(self.tmp.name) / "pw"
        secret.write_text("correct horse")
        for enrolled, status, want in ((0, {"tpm_version": 2}, "prepare"),
                                       (1, {"tpm_version": 2, "own_keys_in_db": False}, "enroll"),
                                       (1, {"tpm_version": 2, "own_keys_in_db": True}, "prepare")):
            calls = []
            def run(command, **kw):
                calls.append((command, kw))
                return subprocess.CompletedProcess(command, enrolled if "--enroll" in command else 0)
            args = self.args("--encrypt", "--tpm", "--secure-boot", "--secure-boot-microsoft",
                             "--luks-passphrase-file", str(secret))
            with mock.patch.object(i, "run", run), mock.patch.object(i, "output", lambda c: json.dumps(status)), \
                    contextlib.redirect_stdout(io.StringIO()):
                i.secure_boot_install(args, self.root, esp)
            commands = [c for c, _ in calls]
            self.assertEqual(commands[0], ["mount", "--bind", str(esp), str(self.root / "efi")])
            self.assertEqual(commands[1], ["arch-chroot", str(self.root), "/usr/local/bin/locachy-tpm", "secure-boot",
                                           "--enroll", "--microsoft"])
            self.assertEqual(commands[2][3:5], [want, "--passphrase-stdin"])
            self.assertEqual(calls[2][1]["input"], "correct horse\n")
            self.assertEqual(commands[-1], ["umount", str(self.root / "efi")])

    def test_running_root_rejected_before_any_build(self):
        with self.assertRaisesRegex(ValueError, "running installation"):
            i.validate("/", "/efi")

    def test_no_automatic_reboot_and_safe_boot_write_order(self):
        script = (SOURCE / "link/locachy-install").read_text()
        self.assertNotIn('run(["reboot', script)
        self.assertIn('"--target-root"', script)
        self.assertIn('"passwd", args.user', script)
        self.assertLess(script.index('"passwd", args.user'), script.index('destination.mkdir'))
        self.assertLess(script.index('if any(saved.iterdir())'), script.index('destination.mkdir'))

    def test_customization_recognizes_multiple_manifest_names_without_duplicates(self):
        conf = self.root / "etc/locachy"
        conf.mkdir(parents=True, exist_ok=True)
        original = "base cryptsetup\npipewire pipewire-alsa\tpipewire-pulse # audio\nwireplumber wiremix sof-firmware\n"
        (conf / "packages").write_text(original)
        (conf / "rc.conf").write_text('hibernate_after="30"\n')
        args = type("Args", (), {"encrypt": True, "features": "audio", "containers": ""})()
        with mock.patch.object(i, "update_source", return_value=("off", "main", "")):
            self.assertFalse(i.apply_customizations(args, self.root))
            self.assertFalse(i.apply_customizations(args, self.root))
        self.assertEqual((conf / "packages").read_text(), original)
        self.assertTrue((conf / "luks").is_file())

    def test_live_network_profiles_are_carried_as_networkmanager_reads_them(self):
        """Keyfiles as the live system's NetworkManager writes them (nmcli,
        nmtui: interface-name of the live device): root's, 0600, their
        secrets kept, bound to no live interface; one whose secret an agent
        kept is said to need it again."""
        live = Path(self.tmp.name) / "live-nm"
        live.mkdir()
        wifi = ("[connection]\nid=Home\nuuid=78b2c58b-d6f5-4885-97bf-7155edd67f06\ntype=wifi\ninterface-name=wlp3s0\n\n"
                "[wifi]\nmode=infrastructure\nssid=Home\n\n[wifi-security]\nkey-mgmt=wpa-psk\npsk=Secret-12345\n\n"
                "[ipv4]\nmethod=auto\n\n[ipv6]\naddr-gen-mode=default\nmethod=auto\n\n[proxy]\n")
        wired = ("[connection]\nid=Wired connection 1\nuuid=0d1c4c2a-5f0e-3f43-9a43-3b1f2f2b9a10\ntype=ethernet\n"
                 "autoconnect-priority=-999\ninterface-name=enp0s25\n\n[ethernet]\n\n[ipv4]\nmethod=auto\n")
        agent = ("[connection]\nid=Cafe\nuuid=5b0f3e0c-1d2a-4c55-8e7a-2f6d9e1b3c44\ntype=wifi\n\n"
                 "[wifi]\nssid=Cafe\n\n[wifi-security]\nkey-mgmt=wpa-psk\npsk-flags=1\n")
        tunnel = ("[connection]\nid=wg0\nuuid=1e2d3c4b-5a69-4788-9a0b-1c2d3e4f5a6b\ntype=wireguard\ninterface-name=wg0\n\n"
                  "[wireguard]\nprivate-key=cGFzc3dvcmQ=\n")
        for name, text in (("Home", wifi), ("Wired connection 1", wired), ("Cafe", agent), ("wg0", tunnel)):
            path = live / (name + ".nmconnection")
            path.write_text(text)
            path.chmod(0o600)
        (live / "notes.txt").write_text("not a profile\n")
        with mock.patch.object(i, "LIVE_NM_CONNECTIONS", live), contextlib.redirect_stdout(io.StringIO()) as out:
            i.copy_network_profiles(self.root)
        target = self.root / "etc/NetworkManager/system-connections"
        self.assertEqual(oct(target.stat().st_mode & 0o777), "0o700")
        self.assertEqual(sorted(p.name for p in target.iterdir()),
                         ["Cafe.nmconnection", "Home.nmconnection", "Wired connection 1.nmconnection", "wg0.nmconnection"])
        for path in target.iterdir():
            self.assertEqual(oct(path.stat().st_mode & 0o777), "0o600", path)  # NetworkManager ignores any other
            self.assertEqual(path.stat().st_uid, os.geteuid())  # root, installing
        self.assertEqual((target / "Home.nmconnection").read_text(), wifi.replace("interface-name=wlp3s0\n", ""))
        self.assertIn("psk=Secret-12345\n", (target / "Home.nmconnection").read_text())
        self.assertEqual((target / "Wired connection 1.nmconnection").read_text(),
                         wired.replace("interface-name=enp0s25\n", ""))
        self.assertEqual((target / "wg0.nmconnection").read_text(), tunnel)  # its own interface, by its name
        self.assertEqual((target / "Cafe.nmconnection").read_text(), agent)
        said = out.getvalue()
        self.assertIn("Saved network Home: copied; not bound to the live system's wlp3s0\n", said)
        self.assertIn("Saved network Wired connection 1: copied; not bound to the live system's enp0s25\n", said)
        self.assertIn("Saved network Cafe: copied; its psk is not saved in it (an agent kept it): "
                      "connect with nmtui to enter it\n", said)
        self.assertIn("Saved network wg0: copied\n", said)
        # Copied again (a continued installation): replaced, still 0600.
        with mock.patch.object(i, "LIVE_NM_CONNECTIONS", live), contextlib.redirect_stdout(io.StringIO()):
            i.copy_network_profiles(self.root)
        self.assertEqual(oct((target / "Home.nmconnection").stat().st_mode & 0o777), "0o600")
        script = (SOURCE / "link/locachy-install").read_text()
        self.assertIn('if getattr(args, "copy_network", False):\n        copy_network_profiles(root)', script)

    def test_partitioning_luks2_and_customizations(self):
        calls = []
        part_target = self.root.parent / "part-target"
        with mock.patch.object(i, "run", lambda cmd, **kw: calls.append((cmd, kw))):
            args = type("Args", (), {
                "disk": "/dev/nvme0n1", "swap_gib": 16, "encrypt": True,
                "root": str(part_target), "esp": str(part_target / "efi"),
            })()
            rdev, edev, sdev = i.prepare_partitions(args, "SecretPass123")
        self.assertEqual((rdev, edev, sdev), ("/dev/mapper/locachy-crypt", "/dev/nvme0n1p1", "/dev/mapper/locachy-swap-crypt"))
        cmds = [c[0] for c in calls]
        self.assertIn(["sfdisk", "--wipe", "always", "/dev/nvme0n1"], cmds)
        self.assertIn(["cryptsetup", "luksFormat", "--batch-mode", "--type", "luks2",
                       "--label", "locachy-crypt", "--key-file", "-", "/dev/nvme0n1p3"], cmds)
        self.assertIn(["cryptsetup", "luksFormat", "--batch-mode", "--type", "luks2",
                       "--label", "locachy-swap-crypt", "--key-file", "-", "/dev/nvme0n1p2"], cmds)
        self.assertIn(["mkfs.ext4", "-F", "-L", "locachy-root", "/dev/mapper/locachy-crypt"], cmds)
        self.assertIn(["mkswap", "-L", "locachy-swap", "/dev/mapper/locachy-swap-crypt"], cmds)
        # A key file is every byte: the key is the passphrase alone, as the
        # boot prompt and locachy-tpm give it (no newline).
        keys = {kw.get("input") for cmd, kw in calls if cmd[0] == "cryptsetup"}
        self.assertEqual(keys, {"SecretPass123"})

        (self.root / "etc/locachy").mkdir(parents=True, exist_ok=True)
        (self.root / "etc/locachy/packages").write_text("base\n")
        (self.root / "etc/locachy/rc.conf").write_text(
            'hibernate_after="30"\nservice_link="x"\nservice_timezone="tz"\nservice_ntp="ntp"\n'
        )
        (self.root / "usr/share/zoneinfo/Asia").mkdir(parents=True, exist_ok=True)
        (self.root / "usr/share/zoneinfo/Asia/Shanghai").write_bytes(b"TZif2")
        presets = self.root / "usr/local/src/locachy/containers"
        presets.mkdir(parents=True, exist_ok=True)
        (presets / "firefox.conf").write_text('package="firefox-of-the-source"\n')  # not a host program: not offered
        (presets / "foot.conf").write_text('host="1"\npackage="foot"\ncommand="/usr/bin/foot"\n')
        firefox = 'package="firefox"\ncommand="/usr/bin/firefox"\n'
        # An app the catalog does not have stops it before anything is written.
        rc_before = (self.root / "etc/locachy/rc.conf").read_text()
        with self.assertRaisesRegex(ValueError, "^Unknown app: firefox$"):
            i.apply_customizations(type("Args", (), {"containers": "foot,firefox"})(), self.root)
        self.assertFalse((self.root / "etc/hostname").exists())
        self.assertEqual((self.root / "etc/locachy/rc.conf").read_text(), rc_before)
        self.make_catalog({"firefox": firefox, "qq": 'package="linuxqq"\ncommand="/usr/bin/linuxqq"\n'})
        cargs = type("Args", (), {
            "encrypt": True, "language": "zh_CN.UTF-8", "login_skip": True,
            "hostname": "x230-box", "keymap": "dvorak", "timezone": "Asia/Shanghai", "no_ntp": True,
            "hibernate_after": 60, "features": "audio,bluetooth,fwupd,flashrom,tailscale,rclone",
            "containers": "firefox,foot,qq",
        })()
        changed = i.apply_customizations(cargs, self.root)
        self.assertTrue(changed)
        self.assertEqual((self.root / "etc/hostname").read_text(), "x230-box\n")
        self.assertIn("127.0.1.1\tx230-box.localdomain x230-box", (self.root / "etc/hosts").read_text())
        self.assertEqual((self.root / "etc/locale.conf").read_text(), "LANG=zh_CN.UTF-8\n")
        self.assertEqual((self.root / "etc/vconsole.conf").read_text(), "KEYMAP=dvorak\n")
        self.assertEqual(i.os.readlink(self.root / "etc/localtime"), "/usr/share/zoneinfo/Asia/Shanghai")
        with self.assertRaisesRegex(ValueError, "Invalid hostname"):
            i.apply_customizations(type("Args", (), {"hostname": "-bad host-"})(), self.root)
        pkgs = (self.root / "etc/locachy/packages").read_text().splitlines()
        self.assertTrue({"cryptsetup", "pipewire", "wiremix", "bluez", "bluetui",
                         "fwupd", "flashrom", "tailscale", "rclone", "fuse3", "foot"} <= set(pkgs))
        self.assertNotIn("texlive-bin", pkgs)
        self.assertNotIn("code", pkgs)
        self.assertNotIn("linuxqq", pkgs)  # an app's packages are its container's, never the host's
        self.assertTrue((self.root / "etc/locachy/luks").is_file())
        rc = (self.root / "etc/locachy/rc.conf").read_text()
        self.assertIn('language="zh_CN.UTF-8"', rc)
        self.assertIn('keymap="dvorak"', rc)
        self.assertNotIn("service_timezone=", rc)
        self.assertNotIn("service_ntp=", rc)
        self.assertIn('login_skip="1"', rc)
        self.assertIn('hibernate_after="60"', rc)
        self.assertIn('service_audio="/usr/local/lib/locachy/audio-client"', rc)
        self.assertIn('program_mixer="/usr/local/bin/term /usr/bin/wiremix"', rc)
        self.assertIn('service_bluetooth="/usr/local/lib/locachy/bluetooth-client"', rc)
        self.assertIn('program_bluetooth="/usr/local/bin/term /usr/bin/bluetui"', rc)
        self.assertIn('service_rclone="/usr/local/lib/locachy/rclone-client"', rc)
        self.assertRegex(rc, r'update_repo="https://github\.com/niuyuanzhao/locachy(?:\.git)?"')
        self.assertEqual(i.SOURCE, SOURCE)
        self.assertEqual((self.root / "etc/locachy/containers/firefox.conf").read_text(), firefox)
        self.assertTrue((self.root / "etc/locachy/containers/foot.conf").is_file())
        self.assertIn('package="linuxqq"', (self.root / "etc/locachy/containers/qq.conf").read_text())
        # The whole catalog, with where it came from, is the installed system's.
        catalog = self.root / i.APPS_CATALOG
        self.assertEqual(sorted(i.APPS.read_catalog(str(catalog))), ["firefox", "qq"])
        self.assertEqual(i.APPS.read_meta(str(catalog))["commit"], "c" * 40)
        self.assertIn(f'apps_repo="{self.APPS_URL}"\napps_branch="next"\n', rc)

        # Minimal install: zero optional features or containers adds nothing beyond core base.
        min_root = self.root.parent / "min-target"
        (min_root / "etc/locachy").mkdir(parents=True, exist_ok=True)
        core_pkgs = [ln.split("#", 1)[0].strip() for ln in (SOURCE / "packages").read_text().splitlines()
                     if ln.split("#", 1)[0].strip()]
        for unwanted in ("bluez", "pipewire", "texlive-bin", "code", "foot", "labwc-locachy", "noto-fonts-cjk"):
            self.assertNotIn(unwanted, core_pkgs)
        (min_root / "etc/locachy/packages").write_text("\n".join(core_pkgs) + "\n")
        (min_root / "etc/locachy/rc.conf").write_text((SOURCE / "rc.conf").read_text())
        margs = type("Args", (), {"encrypt": False, "language": "en_US.UTF-8", "features": "", "containers": ""})()
        self.assertFalse(i.apply_customizations(margs, min_root))
        min_rc = (min_root / "etc/locachy/rc.conf").read_text()
        self.assertNotIn("service_audio=", min_rc)
        self.assertNotIn("service_bluetooth=", min_rc)
        self.assertIn('program_firmware="/usr/local/bin/firmware"', min_rc)
        self.assertEqual(i.effective_ime(type("A", (), {"ime": "auto", "language": "en_US.UTF-8"})()), "off")
        self.assertEqual(i.effective_ime(type("A", (), {"ime": "auto", "language": "zh_CN.UTF-8"})()), "zh")
        self.assertEqual(i.effective_ime(type("A", (), {"ime": "en", "language": "en_US.UTF-8"})()), "en")
        self.assertEqual(i.effective_ime(type("A", (), {"ime": "zh", "no_ime": True, "language": "zh_CN.UTF-8"})()), "off")

    MULTI_BLOCK = {"blockdevices": [
        {"path": "/dev/nvme0n1", "maj:min": "259:0", "type": "disk", "size": 512 << 30, "model": "Primary NVMe",
         "serial": "SER-0", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme0n1p1", "maj:min": "259:1", "type": "part", "size": 512 << 20, "start": 2048,
              "fstype": "vfat", "label": "EFI", "parttypename": "EFI System", "mountpoints": [None]},
             {"path": "/dev/nvme0n1p2", "maj:min": "259:2", "type": "part", "size": 400 << 30, "start": 1050624,
              "fstype": "ext4", "label": "oldroot", "parttypename": "Linux filesystem", "mountpoints": [None]}]},
        # 708.5 GiB of partitions back to back from 1 MiB: 291.5 GiB unallocated after the last one.
        {"path": "/dev/nvme1n1", "maj:min": "259:10", "type": "disk", "size": 1000 << 30, "model": "Data NVMe 1",
         "serial": "SER-1", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme1n1p1", "maj:min": "259:11", "type": "part", "size": 500 << 30, "start": 2048,
              "fstype": "ntfs", "label": "Archive", "parttypename": "Microsoft basic data", "mountpoints": [None]},
             {"path": "/dev/nvme1n1p2", "maj:min": "259:12", "type": "part", "size": 512 << 20, "start": 1048578048,
              "fstype": "vfat", "label": "ESP", "parttypename": "EFI System", "mountpoints": [None]},
             {"path": "/dev/nvme1n1p3", "maj:min": "259:13", "type": "part", "size": 8 << 30, "start": 1049626624,
              "fstype": "swap", "label": "swap", "parttypename": "Linux swap", "mountpoints": [None]},
             {"path": "/dev/nvme1n1p4", "maj:min": "259:14", "type": "part", "size": 200 << 30, "start": 1066403840,
              "fstype": "ext4", "label": "Projects", "parttypename": "Linux filesystem", "mountpoints": [None]}]},
        {"path": "/dev/nvme2n1", "maj:min": "259:20", "type": "disk", "size": 1000 << 30, "model": "Data NVMe 2",
         "serial": "SER-2", "tran": "nvme", "rm": False, "ro": False, "mountpoints": [None]},
        {"path": "/dev/nvme3n1", "maj:min": "259:30", "type": "disk", "size": 1000 << 30, "model": "Data NVMe 3",
         "serial": "SER-3", "tran": "nvme", "rm": False, "ro": False, "mountpoints": [None]},
        {"path": "/dev/nvme4n1", "maj:min": "259:40", "type": "disk", "size": 1000 << 30, "model": "Data NVMe 4",
         "serial": "SER-4", "tran": "nvme", "rm": False, "ro": False, "mountpoints": [None]},
        {"path": "/dev/sdb", "maj:min": "8:16", "type": "disk", "size": 16 << 30, "model": "Live USB",
         "tran": "usb", "rm": True, "ro": False, "mountpoints": [None], "children": [
             {"path": "/dev/sdb1", "maj:min": "8:17", "type": "part", "size": 1 << 30, "fstype": "iso9660",
              "mountpoints": ["/run/archiso/bootmnt"]}]},
    ]}

    def multi_block_output(self, command):
        if command[0] == "findmnt":
            return json.dumps({"filesystems": [{"target": "/", "source": "airootfs"},
                                               {"target": "/run/archiso/bootmnt", "source": "/dev/sdb1"}]})
        if command[:2] == ["lsblk", "--inverse"]:
            anc = {}
            for d in self.MULTI_BLOCK["blockdevices"]:
                anc[d["path"]] = d["maj:min"]
                for c in d.get("children") or []:
                    anc[c["path"]] = f"{c['maj:min']}\n{d['maj:min']}"
            if command[-1] not in anc:
                raise subprocess.CalledProcessError(32, command)
            return anc[command[-1]]
        if command[:2] == ["lsblk", "--json"]:
            def find(nodes, path):
                for n in nodes:
                    if n["path"] == path:
                        return n
                    got = find(n.get("children") or [], path)
                    if got:
                        return got
            if not command[-1].startswith("/dev/"):
                return json.dumps(self.MULTI_BLOCK)
            node = find(self.MULTI_BLOCK["blockdevices"], command[-1])
            if not node:
                raise subprocess.CalledProcessError(32, command)
            return json.dumps({"blockdevices": [node]})
        return self.output(command)

    def test_extra_volumes_parse_and_target_validation(self):
        # Valid format, attach, raid1 array, raid10 with 4 disks, and auto encryption resolution.
        vols = i.parse_extra_volumes([
            "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,encrypt=auto,compute_storage=1",
            "name=archive,mode=attach,dev=/dev/nvme2n1p1,role=data",
            "name=pool,mode=array,devs=/dev/nvme3n1+/dev/nvme4n1,raid=raid1,role=compute,encrypt=none",
        ], encrypt_root=True)
        self.assertEqual(vols[0]["effective_encrypt"], "luks")
        self.assertTrue(vols[0]["compute_storage"])
        self.assertEqual(vols[1]["effective_encrypt"], "none")
        self.assertEqual(vols[2]["raid"], "raid1")
        self.assertEqual(vols[2]["fs"], "btrfs")

        vols_plain = i.parse_extra_volumes([
            "name=data,mode=format,dev=/dev/nvme1n1,fs=btrfs,role=shared,encrypt=auto",
        ], encrypt_root=False)
        self.assertEqual(vols_plain[0]["effective_encrypt"], "none")

        with mock.patch.object(i, "output", self.multi_block_output):
            # Valid format + attach on distinct disks.
            i.check_targets(self.args(
                "--disk", "/dev/nvme0n1",
                "--extra-volume", "name=archive,mode=attach,dev=/dev/nvme1n1p1,role=shared",
                "--extra-volume", "name=projects,mode=attach,dev=/dev/nvme1n1p4,role=data",
                "--extra-volume", "name=pool,mode=array,devs=/dev/nvme2n1+/dev/nvme3n1,raid=raid1,role=compute,compute_storage=1",
            ))
            # Valid raid10 with 4 distinct whole disks.
            i.check_targets(self.args(
                "--disk", "/dev/nvme0n1",
                "--extra-volume", "name=pool10,mode=array,devs=/dev/nvme1n1+/dev/nvme2n1+/dev/nvme3n1+/dev/nvme4n1,raid=raid10,role=compute",
            ))

            # Reject invalid specs and conflicting targets.
            bad_cases = (
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=data,mode=format,dev=/dev/nvme0n1,fs=ext4,role=shared"],
                 "overlaps the primary system disk"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=a,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared",
                  "--extra-volume", "name=b,mode=format,dev=/dev/nvme1n1,fs=btrfs,role=data"],
                 "already used by another volume"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=a,mode=attach,dev=/dev/nvme1n1p1,role=shared",
                  "--extra-volume", "name=b,mode=format,dev=/dev/nvme1n1,fs=ext4,role=data"],
                 "already used by another volume"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=live,mode=format,dev=/dev/sdb,fs=ext4,role=shared"],
                 "live medium"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=badpart,mode=format,dev=/dev/nvme1n1p1,fs=ext4,role=shared"],
                 "not a whole disk"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=esp,mode=attach,dev=/dev/nvme1n1p2,role=shared"],
                 "swap or EFI"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=sw,mode=attach,dev=/dev/nvme1n1p3,role=shared"],
                 "swap or EFI"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=pool,mode=array,devs=/dev/nvme1n1,raid=raid1,role=compute"],
                 "at least 2 disks"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=pool,mode=array,devs=/dev/nvme1n1+/dev/nvme2n1+/dev/nvme3n1,raid=raid10,role=compute"],
                 "at least 4 disks"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=data,compute_storage=1"],
                 "compute_storage=1 requires role"),
                (["--disk", "/dev/nvme0n1",
                  "--extra-volume", "name=a,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,compute_storage=1",
                  "--extra-volume", "name=b,mode=format,dev=/dev/nvme2n1,fs=ext4,role=compute,compute_storage=1"],
                 "At most one --extra-volume"),
                (["--disk", "/dev/nvme0n1", "--extra-volume", "name=Bad Name,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared"],
                 "invalid volume name"),
                (["--disk", "/dev/nvme0n1",
                  "--extra-volume", "name=dup,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared",
                  "--extra-volume", "name=dup,mode=format,dev=/dev/nvme2n1,fs=ext4,role=shared"],
                 "duplicate volume name"),
            )
            for argv, expected in bad_cases:
                with self.assertRaisesRegex(ValueError, expected, msg=argv):
                    parsed = self.args(*argv)
                    i.check_choices(parsed)
                    i.check_targets(parsed)

            # Same-parent-disk array members rejected even when device paths differ (e.g. symlink / alias).
            def same_parent_output(command):
                if command[:2] == ["lsblk", "--inverse"] and command[-1] in ("/dev/nvme1n1", "/dev/disk-alias"):
                    return "259:10"
                if command[:2] == ["lsblk", "--json"] and command[-1] == "/dev/disk-alias":
                    return json.dumps({"blockdevices": [dict(self.MULTI_BLOCK["blockdevices"][1], path="/dev/disk-alias")]})
                return self.multi_block_output(command)
            with mock.patch.object(i, "output", same_parent_output):
                with self.assertRaisesRegex(ValueError, "shares a physical disk"):
                    i.check_targets(self.args(
                        "--disk", "/dev/nvme0n1",
                        "--extra-volume", "name=pool,mode=array,devs=/dev/nvme1n1+/dev/disk-alias,raid=raid1,role=compute",
                    ))

        # describe() advertises storage_roles and storage_raids without dropping existing keys.
        with mock.patch.object(i, "output", self.multi_block_output), \
             mock.patch.object(i, "cpu_model", lambda level: "ivybridge"), \
             mock.patch.object(i, "tpm_status", lambda: {"tpm_version": 0}), \
             mock.patch.object(i, "host_security", lambda: None), \
             mock.patch.object(i, "tpm_advice", lambda st=None: (False, "passphrase", "no TPM")):
            desc = i.describe(i.make_parser())
        self.assertEqual(desc["storage_roles"], ["shared", "data", "compute"])
        self.assertEqual(desc["storage_raids"], ["single", "raid0", "raid1", "raid10"])
        self.assertEqual(desc["disks"][0]["serial"], "SER-0")

    def test_extra_volumes_packages_install_and_resume_idempotency(self):
        # selected_package_roots adds btrfs-progs for btrfs/array and cryptsetup for luks extra volume.
        roots_btrfs = i.selected_package_roots(self.args(
            "--extra-volume", "name=pool,mode=array,devs=/dev/nvme1n1+/dev/nvme2n1,raid=raid1,role=compute,encrypt=luks",
        ))
        self.assertIn("btrfs-progs", roots_btrfs)
        self.assertIn("cryptsetup", roots_btrfs)

        roots_ext4 = i.selected_package_roots(self.args(
            "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,encrypt=none",
        ))
        self.assertNotIn("btrfs-progs", roots_ext4)

        # Full install() path: invokes locachy-storage create --root ... --no-mount ..., writes
        # compute_storage to rc.conf, records extra_volumes in install.json, and skips on --resume.
        run_dir = Path(self.tmp.name) / "run-esp"
        run_dir.mkdir(parents=True)
        pass_file = Path(self.tmp.name) / "luks.pass"
        pass_file.write_text("DiskPass123")
        pw_file = Path(self.tmp.name) / "user.pass"
        pw_file.write_text("UserPass123")

        (self.root / "etc/locachy").mkdir(parents=True, exist_ok=True)
        (self.root / "etc/locachy/packages").write_text("base\n")
        (self.root / "etc/locachy/rc.conf").write_text('hibernate_after="30"\n')
        (self.root / "var/lib/locachy").mkdir(parents=True, exist_ok=True)
        boot_img = self.root / "usr/local/lib/locachy/boot/locachy.efi"
        boot_img.parent.mkdir(parents=True, exist_ok=True)
        boot_img.write_bytes(b"EFI")

        calls = []
        def fake_run(cmd, check=True, **kw):
            calls.append(cmd)
            if cmd[0] == "umount" and Path(cmd[1]).is_dir():
                # The mount point empties when unmounted (its files were the ESP's, not ours).
                for child in Path(cmd[1]).iterdir():
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
                return subprocess.CompletedProcess(cmd, 0)
            if "locachy-storage" in str(cmd):
                # Record created volume into target volumes.json as real locachy-storage does.
                name = cmd[cmd.index("--name") + 1]
                vf = self.root / "etc/locachy/volumes.json"
                data = json.loads(vf.read_text()) if vf.is_file() else {"schema": 1, "volumes": []}
                data["volumes"].append({"name": name})
                vf.write_text(json.dumps(data))
            return subprocess.CompletedProcess(cmd, 0)

        class FakePopen:
            def __init__(self, cmd, **kw):
                calls.append(cmd)
                self.stdout = iter(["make-root: target built and verified\n"])
            def wait(self):
                return 0

        args = self.args(
            "--root", str(self.root), "--esp", str(self.esp), "--yes", "--no-rank-mirrors",
            "--encrypt", "--luks-passphrase-file", str(pass_file), "--password-file", str(pw_file),
            "--extra-volume", "name=archive,mode=attach,dev=/dev/nvme1n1p1,role=data",
            "--extra-volume", "name=pool,mode=array,devs=/dev/nvme2n1+/dev/nvme3n1,raid=raid1,role=compute,encrypt=auto,compute_storage=1",
        )
        esp_dirs = iter([run_dir / "esp-1", run_dir / "esp-2"])

        def fresh_dir(**kw):
            path = next(esp_dirs)
            path.mkdir(parents=True, exist_ok=True)
            return str(path)

        def fake_fs(path):
            return self.esp_fs if Path(path).parent == run_dir else self.root_fs

        def install_once():
            with mock.patch.object(i, "run", fake_run), \
                 mock.patch.object(i.subprocess, "Popen", FakePopen), \
                 mock.patch.object(i.tempfile, "mkdtemp", fresh_dir), \
                 mock.patch.object(i, "filesystem", fake_fs), \
                 mock.patch.object(i, "cpu_model", lambda lvl: "ivybridge"), \
                 mock.patch.object(i, "update_source", lambda a: ("off", "main", "")), \
                 mock.patch.object(i, "apps_source", lambda a: ("off", "main")), \
                 contextlib.redirect_stdout(io.StringIO()):
                i.install(args, self.root, self.esp, self.root_fs, self.esp_fs, "x86-64-v3")

        install_once()

        storage_calls = [c for c in calls if any("locachy-storage" in str(x) for x in c)]
        self.assertEqual(len(storage_calls), 2)
        self.assertEqual(storage_calls[0][2:], [
            str(SOURCE / "link/locachy-storage"), "create", "--root", str(self.root), "--no-mount",
            "--name", "archive", "--mode", "attach", "--role", "data", "--dev", "/dev/nvme1n1p1", "--encrypt", "none",
        ])
        self.assertEqual(storage_calls[1][2:], [
            str(SOURCE / "link/locachy-storage"), "create", "--root", str(self.root), "--no-mount",
            "--name", "pool", "--mode", "array", "--role", "compute",
            "--devs", "/dev/nvme2n1,/dev/nvme3n1", "--raid", "raid1", "--encrypt", "luks",
            "--passphrase-file", str(pass_file),
        ])
        # rc.conf and packages manifest updated.
        rc_text = (self.root / "etc/locachy/rc.conf").read_text()
        self.assertIn('compute_storage="pool"', rc_text)
        pkgs = (self.root / "etc/locachy/packages").read_text().splitlines()
        self.assertIn("btrfs-progs", pkgs)
        self.assertIn("cryptsetup", pkgs)
        # install.json records extra_volumes summary.
        summary = json.loads((self.root / "var/lib/locachy/install.json").read_text())
        self.assertEqual([v["name"] for v in summary["extra_volumes"]], ["archive", "pool"])
        self.assertEqual(summary["extra_volumes"][1]["encrypt"], "luks")
        self.assertTrue(summary["extra_volumes"][1]["compute_storage"])
        # stages.json records extra-volumes success.
        stages_data = json.loads((self.root / ".locachy-installer/stages.json").read_text())
        self.assertTrue(stages_data["stages"]["extra-volumes"]["success"])

        # Simulate --resume: the extra-volumes stage MUST NOT re-invoke locachy-storage.
        calls.clear()
        (self.root / "etc/passwd").write_text("user:x:1000:1000::/home/user:/bin/bash\n")
        args.resume = True
        install_once()
        self.assertEqual([c for c in calls if any("locachy-storage" in str(x) for x in c)], [])

        # Also verify safe_arguments preserves repeatable --extra-volume entries.
        safe = i.safe_arguments(args)
        self.assertEqual(safe.count("--extra-volume"), 2)
        self.assertIn("name=archive,mode=attach,dev=/dev/nvme1n1p1,role=data", safe)

    def test_winmaint_disk_validation_tail_reservation_and_install(self):
        with mock.patch.object(i, "output", self.multi_block_output), \
             mock.patch.object(i, "cpu_model", lambda level: "ivybridge"), \
             mock.patch.object(i, "tpm_status", lambda: {"tpm_version": 0}), \
             mock.patch.object(i, "host_security", lambda: None), \
             mock.patch.object(i, "tpm_advice", lambda st=None: (False, "passphrase", "no TPM")):
            desc = i.describe(i.make_parser())
        self.assertEqual(desc["winmaint"], {"min_gib": 40, "default_gib": 48, "max_gib": 4096})
        # One Maintenance Windows default across the installer, locachy-winmaint and rc.conf.
        winmaint_src = (SOURCE / "link/locachy-winmaint").read_text(encoding="utf-8")
        self.assertEqual(int(re.search(r"^DEFAULT_WIN_C_GIB = (\d+)$", winmaint_src, re.M).group(1)), i.WINMAINT_DEFAULT_GIB)
        rc_text = (SOURCE / "rc.conf").read_text(encoding="utf-8")
        self.assertEqual(int(re.search(r'^winmaint_size_gib="(\d+)"$', rc_text, re.M).group(1)), i.WINMAINT_DEFAULT_GIB)

        roots = i.selected_package_roots(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme0n1"))
        self.assertIn("dosfstools", roots)
        self.assertIn("mtools", roots)
        self.assertNotIn("qemu-desktop", roots)

        with mock.patch.object(i, "output", self.multi_block_output):
            i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme0n1", "--winmaint-gib", "64"))
            i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "128"))
            i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "64"))
            # mode=format + --winmaint-disk on the same secondary disk is allowed (even when old
            # partitions occupy most of the disk, since mode=format wipes and reserves the tail).
            i.check_targets(self.args(
                "--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "48",
                "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared",
            ))
            i.check_targets(self.args(
                "--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "900",
                "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=btrfs,role=shared",
            ))

            for argv, said in (
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme0n1", "--winmaint-gib", "20"], "between 40 and 4096"),
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/sdb", "--winmaint-gib", "64"], "live medium"),
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1p1", "--winmaint-gib", "64"], "not a whole disk"),
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme0n1", "--winmaint-gib", "500"], "at least"),
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "350"], "unallocated"),
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "999",
                  "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared"], "at least"),
                (["--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "64",
                  "--extra-volume", "name=pool,mode=array,devs=/dev/nvme2n1+/dev/nvme3n1,raid=raid1,role=compute"], "cannot share a disk"),
            ):
                with self.assertRaisesRegex(ValueError, said, msg=argv):
                    parsed = self.args(*argv)
                    i.check_choices(parsed)
                    i.check_targets(parsed)

        sfdisk_calls = []
        part_target = self.root.parent / "wm-target"
        with mock.patch.object(i, "run", lambda cmd, **kw: sfdisk_calls.append((cmd, kw))), \
             mock.patch.object(i, "disk_size_bytes", lambda d: 200 << 30):
            wm_args = self.args("--disk", "/dev/nvme0n1", "--swap-gib", "8",
                                "--winmaint-disk", "/dev/nvme0n1", "--winmaint-gib", "64",
                                "--root", str(part_target), "--esp", str(part_target / "efi"))
            i.prepare_partitions(wm_args)
        sfdisk_input = next(kw["input"] for cmd, kw in sfdisk_calls if cmd[0] == "sfdisk")
        self.assertIn(",130301M,L\n", sfdisk_input)
        self.assertNotIn(",+,L\n", sfdisk_input)

        run_dir = Path(self.tmp.name) / "run-wm"
        run_dir.mkdir(parents=True)
        pw_file = Path(self.tmp.name) / "wm-user.pass"
        pw_file.write_text("UserPass123")
        (self.root / "etc/locachy").mkdir(parents=True, exist_ok=True)
        (self.root / "etc/locachy/packages").write_text("base\n")
        (self.root / "etc/locachy/rc.conf").write_text('winmaint_size_gib="48"\n')
        (self.root / "var/lib/locachy").mkdir(parents=True, exist_ok=True)
        boot_img = self.root / "usr/local/lib/locachy/boot/locachy.efi"
        boot_img.parent.mkdir(parents=True, exist_ok=True)
        boot_img.write_bytes(b"EFI")

        calls = []
        def fake_run(cmd, check=True, **kw):
            calls.append(cmd)
            if cmd[0] == "umount" and Path(cmd[1]).is_dir():
                for child in Path(cmd[1]).iterdir():
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
            if "locachy-winmaint" in str(cmd):
                sf = self.root / "var/lib/locachy/winmaint/state.json"
                sf.parent.mkdir(parents=True, exist_ok=True)
                sf.write_text(json.dumps({"schema": 1, "disk": "/dev/nvme0n1",
                                          "partitions": {"c": {"sectors": 96 * 2097152}}}) + "\n")
            return subprocess.CompletedProcess(cmd, 0)

        class FakePopen:
            def __init__(self, cmd, **kw):
                self.stdout = iter(["make-root: target built and verified\n"])
            def wait(self):
                return 0

        esp_dirs = iter([run_dir / "esp-1", run_dir / "esp-2"])
        def fresh_dir(**kw):
            p = next(esp_dirs)
            p.mkdir(parents=True, exist_ok=True)
            return str(p)
        def fake_fs(path):
            return self.esp_fs if Path(path).parent == run_dir else self.root_fs

        args = self.args(
            "--root", str(self.root), "--esp", str(self.esp), "--yes", "--no-rank-mirrors",
            "--password-file", str(pw_file),
            "--winmaint-disk", "/dev/nvme0n1", "--winmaint-gib", "96",
        )
        def run_install():
            with mock.patch.object(i, "run", fake_run), \
                 mock.patch.object(i.subprocess, "Popen", FakePopen), \
                 mock.patch.object(i.tempfile, "mkdtemp", fresh_dir), \
                 mock.patch.object(i, "filesystem", fake_fs), \
                 mock.patch.object(i, "output", lambda cmd: "gpt" if "PTTYPE" in cmd else self.output(cmd)), \
                 mock.patch.object(i, "cpu_model", lambda lvl: "ivybridge"), \
                 mock.patch.object(i, "update_source", lambda a: ("off", "main", "")), \
                 mock.patch.object(i, "apps_source", lambda a: ("off", "main")), \
                 contextlib.redirect_stdout(io.StringIO()):
                i.install(args, self.root, self.esp, self.root_fs, self.esp_fs, "x86-64-v3")

        run_install()
        wm_calls = [c for c in calls if any("locachy-winmaint" in str(x) for x in c)]
        self.assertEqual(len(wm_calls), 1)
        self.assertEqual(wm_calls[0][2:], [
            str(SOURCE / "link/locachy-winmaint"), "create",
            "--root", str(self.root), "--disk", "/dev/nvme0n1", "--size-gib", "96",
        ])
        self.assertIn('winmaint_size_gib="96"', (self.root / "etc/locachy/rc.conf").read_text())
        summary = json.loads((self.root / "var/lib/locachy/install.json").read_text())
        self.assertEqual(summary["maintenance_windows"], {"disk": "/dev/nvme0n1", "gib": 96})

        calls.clear()
        (self.root / "etc/passwd").write_text("user:x:1000:1000::/home/user:/bin/bash\n")
        args.resume = True
        run_install()
        self.assertEqual([c for c in calls if any("locachy-winmaint" in str(x) for x in c)], [])

        # Shared secondary disk: mode=format extra volume + Maintenance Windows passes
        # --reserve-tail-mib <exact MiB> to locachy-storage BEFORE locachy-winmaint create runs.
        shared_root = self.root.parent / "wm-shared-target"
        shared_esp = shared_root / "efi"
        shared_esp.mkdir(parents=True)
        (shared_root / "etc/locachy").mkdir(parents=True, exist_ok=True)
        (shared_root / "etc/locachy/packages").write_text("base\n")
        (shared_root / "etc/locachy/rc.conf").write_text('winmaint_size_gib="48"\n')
        (shared_root / "var/lib/locachy").mkdir(parents=True, exist_ok=True)
        shared_boot = shared_root / "usr/local/lib/locachy/boot/locachy.efi"
        shared_boot.parent.mkdir(parents=True, exist_ok=True)
        shared_boot.write_bytes(b"EFI")
        shared_calls = []

        def shared_fake_run(cmd, check=True, **kw):
            shared_calls.append(cmd)
            if cmd[0] == "umount" and Path(cmd[1]).is_dir():
                for child in Path(cmd[1]).iterdir():
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
            if "locachy-storage" in str(cmd):
                name = cmd[cmd.index("--name") + 1]
                vf = shared_root / "etc/locachy/volumes.json"
                data = json.loads(vf.read_text()) if vf.is_file() else {"schema": 1, "volumes": []}
                data["volumes"].append({"name": name})
                vf.write_text(json.dumps(data))
            if "locachy-winmaint" in str(cmd):
                sf = shared_root / "var/lib/locachy/winmaint/state.json"
                sf.parent.mkdir(parents=True, exist_ok=True)
                sf.write_text(json.dumps({"schema": 1, "disk": "/dev/nvme1n1",
                                          "partitions": {"c": {"sectors": 48 * 2097152}}}) + "\n")
            return subprocess.CompletedProcess(cmd, 0)

        shared_esp_dir = run_dir / "esp-shared"
        def shared_fresh_dir(**kw):
            shared_esp_dir.mkdir(parents=True, exist_ok=True)
            return str(shared_esp_dir)

        shared_args = self.args(
            "--root", str(shared_root), "--esp", str(shared_esp), "--yes", "--no-rank-mirrors",
            "--password-file", str(pw_file),
            "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,encrypt=none",
            "--extra-volume", "name=other,mode=format,dev=/dev/nvme2n1,fs=btrfs,role=data,encrypt=none",
            "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "48",
        )
        expected_tail_mib = str(48 * 1024 + i.WINMAINT_OVERHEAD_MIB)
        with mock.patch.object(i, "run", shared_fake_run), \
             mock.patch.object(i.subprocess, "Popen", FakePopen), \
             mock.patch.object(i.tempfile, "mkdtemp", shared_fresh_dir), \
             mock.patch.object(i, "filesystem", fake_fs), \
             mock.patch.object(i, "output", lambda cmd: "gpt" if "PTTYPE" in cmd else self.multi_block_output(cmd)), \
             mock.patch.object(i, "cpu_model", lambda lvl: "ivybridge"), \
             mock.patch.object(i, "update_source", lambda a: ("off", "main", "")), \
             mock.patch.object(i, "apps_source", lambda a: ("off", "main")), \
             contextlib.redirect_stdout(io.StringIO()):
            i.install(shared_args, shared_root, shared_esp, self.root_fs, self.esp_fs, "x86-64-v3")

        st_indices = [idx for idx, c in enumerate(shared_calls) if any("locachy-storage" in str(x) for x in c)]
        wm_indices = [idx for idx, c in enumerate(shared_calls) if any("locachy-winmaint" in str(x) for x in c)]
        efi_indices = [idx for idx, c in enumerate(shared_calls) if any("locachy-efi" in str(x) for x in c)]
        self.assertEqual(len(st_indices), 2)
        self.assertEqual(len(wm_indices), 1)
        self.assertEqual(len(efi_indices), 1)
        # extra-volumes runs before winmaint, and winmaint runs after the boot image is written and registered.
        self.assertLess(max(st_indices), wm_indices[0])
        self.assertLess(efi_indices[0], wm_indices[0])
        data_cmd = shared_calls[st_indices[0]]
        other_cmd = shared_calls[st_indices[1]]
        self.assertEqual(data_cmd[2:], [
            str(SOURCE / "link/locachy-storage"), "create", "--root", str(shared_root), "--no-mount",
            "--name", "data", "--mode", "format", "--role", "shared",
            "--dev", "/dev/nvme1n1", "--fs", "ext4", "--encrypt", "none",
            "--reserve-tail-mib", expected_tail_mib,
        ])
        self.assertNotIn("--reserve-tail-mib", other_cmd)
        self.assertEqual(shared_calls[wm_indices[0]][2:], [
            str(SOURCE / "link/locachy-winmaint"), "create",
            "--root", str(shared_root), "--disk", "/dev/nvme1n1", "--size-gib", "48",
        ])


    # Maintenance Windows' tail measured as locachy-winmaint create measures it (starts in 512-byte sectors).
    FLOOR = (3 << 20) + (512 << 20) + (8 << 30) + (16 << 30)  # the system disk's least size with the default swap
    GAP_BLOCK = {"blockdevices": [
        {"path": "/dev/nvme0n1", "maj:min": "259:0", "type": "disk", "size": 512 << 30, "model": "Primary NVMe",
         "serial": "SER-0", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": []},
        # 100 GiB from 1 MiB and 180 GiB from 300 GiB: 220 GiB unallocated in between, 20 GiB at the end.
        {"path": "/dev/nvme1n1", "maj:min": "259:10", "type": "disk", "size": 500 << 30, "model": "Gap",
         "serial": "GAP-1", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme1n1p1", "maj:min": "259:11", "type": "part", "size": 100 << 30, "start": 2048,
              "fstype": "ext4", "label": "a", "parttypename": "Linux filesystem", "mountpoints": [None]},
             {"path": "/dev/nvme1n1p2", "maj:min": "259:12", "type": "part", "size": 180 << 30,
              "start": (300 << 30) // 512, "fstype": "ext4", "label": "b", "parttypename": "Linux filesystem",
              "mountpoints": [None]}]},
        # Exactly the tail 48 GiB of C: and 256 MiB need (p1 ends at 2099200, backup GPT takes 33 sectors);
        # one sector less on the next disk.
        {"path": "/dev/nvme2n1", "maj:min": "259:20", "type": "disk",
         "size": (2048 + (1 << 30) // 512 + (48 * 1024 + 256) * 2048 + 33) * 512, "model": "Exact",
         "serial": "EX-2", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme2n1p1", "maj:min": "259:21", "type": "part", "size": 1 << 30, "start": 2048,
              "fstype": "ext4", "label": "a", "parttypename": "Linux filesystem", "mountpoints": [None]}]},
        {"path": "/dev/nvme3n1", "maj:min": "259:30", "type": "disk",
         "size": (2048 + (1 << 30) // 512 + (48 * 1024 + 256) * 2048 + 32) * 512, "model": "Short",
         "serial": "SH-3", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme3n1p1", "maj:min": "259:31", "type": "part", "size": 1 << 30, "start": 2048,
              "fstype": "ext4", "label": "a", "parttypename": "Linux filesystem", "mountpoints": [None]}]},
        {"path": "/dev/nvme4n1", "maj:min": "259:40", "type": "disk", "size": 1000 << 30, "model": "4Kn",
         "serial": "4K-4", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 4096,
         "mountpoints": [None], "children": []},
        {"path": "/dev/nvme5n1", "maj:min": "259:50", "type": "disk", "size": 1000 << 30, "model": "MBR",
         "serial": "MBR-5", "tran": "nvme", "rm": False, "ro": False, "pttype": "dos", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme5n1p1", "maj:min": "259:51", "type": "part", "size": 500 << 30, "start": 2048,
              "fstype": "ext4", "label": "a", "parttypename": "Linux filesystem", "mountpoints": [None]}]},
        {"path": "/dev/nvme6n1", "maj:min": "259:60", "type": "disk", "size": 1000 << 30, "model": "Blank",
         "serial": "BL-6", "tran": "nvme", "rm": False, "ro": False, "pttype": "", "log-sec": 512,
         "mountpoints": [None], "children": []},
        {"path": "/dev/nvme7n1", "maj:min": "259:70", "type": "disk", "size": 1000 << 30, "model": "RawFS",
         "serial": "RAW-7", "tran": "nvme", "rm": False, "ro": False, "pttype": "", "fstype": "ext4",
         "log-sec": 512, "mountpoints": [None], "children": []},
        # A disk that already holds Windows: an ESP, locachy-winmaint's ESP, MSR and C:.
        {"path": "/dev/nvme8n1", "maj:min": "259:80", "type": "disk", "size": 1000 << 30, "model": "Windows",
         "serial": "WIN-8", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": [
             {"path": "/dev/nvme8n1p1", "maj:min": "259:81", "type": "part", "size": 512 << 20, "start": 2048,
              "fstype": "vfat", "label": "EFI", "parttypename": "EFI System", "partlabel": "EFI system partition",
              "mountpoints": [None]},
             {"path": "/dev/nvme8n1p2", "maj:min": "259:82", "type": "part", "size": 128 << 20, "start": 1050624,
              "fstype": "vfat", "label": "WINMAINT_EP", "parttypename": "EFI System", "partlabel": "LOCACHY_WIN_ESP",
              "mountpoints": [None]},
             {"path": "/dev/nvme8n1p3", "maj:min": "259:83", "type": "part", "size": 16 << 20, "start": 1312768,
              "fstype": "", "label": "", "parttypename": "Microsoft reserved", "partlabel": "LOCACHY_WIN_MSR",
              "mountpoints": [None]},
             {"path": "/dev/nvme8n1p4", "maj:min": "259:84", "type": "part", "size": 48 << 30, "start": 1345536,
              "fstype": "ntfs", "label": "WINMAINT_C", "parttypename": "Microsoft basic data",
              "partlabel": "LOCACHY_WIN_C", "mountpoints": [None]}]},
        {"path": "/dev/nvme9n1", "maj:min": "259:90", "type": "disk", "size": FLOOR - 1, "model": "Small",
         "serial": "SM-9", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": []},
        {"path": "/dev/nvme10n1", "maj:min": "259:100", "type": "disk", "size": FLOOR, "model": "Floor",
         "serial": "FL-10", "tran": "nvme", "rm": False, "ro": False, "pttype": "gpt", "log-sec": 512,
         "mountpoints": [None], "children": []},
        {"path": "/dev/sdb", "maj:min": "8:16", "type": "disk", "size": 16 << 30, "model": "Live USB",
         "tran": "usb", "rm": True, "ro": False, "mountpoints": [None], "children": [
             {"path": "/dev/sdb1", "maj:min": "8:17", "type": "part", "size": 1 << 30, "fstype": "iso9660",
              "mountpoints": ["/run/archiso/bootmnt"]}]},
    ]}

    def tree_output(self, tree, command):
        if command[0] == "findmnt":
            return json.dumps({"filesystems": [{"target": "/", "source": "airootfs"},
                                               {"target": "/run/archiso/bootmnt", "source": "/dev/sdb1"}]})
        if command[:2] == ["lsblk", "--inverse"]:
            anc = {}
            for d in tree["blockdevices"]:
                anc[d["path"]] = d["maj:min"]
                for c in d.get("children") or []:
                    anc[c["path"]] = f"{c['maj:min']}\n{d['maj:min']}"
            if command[-1] not in anc:
                raise subprocess.CalledProcessError(32, command)
            return anc[command[-1]]
        if command[:2] == ["lsblk", "--json"]:
            def find(nodes, path):
                for n in nodes:
                    if n["path"] == path:
                        return n
                    got = find(n.get("children") or [], path)
                    if got:
                        return got
            if not command[-1].startswith("/dev/"):
                return json.dumps(tree)
            node = find(tree["blockdevices"], command[-1])
            if not node:
                raise subprocess.CalledProcessError(32, command)
            return json.dumps({"blockdevices": [node]})
        return self.output(command)

    def gap_output(self, command):
        return self.tree_output(self.GAP_BLOCK, command)

    def test_winmaint_tail_is_measured_as_locachy_winmaint_create_measures_it(self):
        with mock.patch.object(i, "output", self.gap_output):
            with self.assertRaisesRegex(ValueError, "unallocated at the end of the disk"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "40"))
            i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "48"))
            with self.assertRaisesRegex(ValueError, "unallocated at the end of the disk"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme3n1", "--winmaint-gib", "48"))
            with self.assertRaisesRegex(ValueError, "MBR"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme5n1", "--winmaint-gib", "48"))
            with self.assertRaisesRegex(ValueError, "512-byte sectors"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme4n1", "--winmaint-gib", "48"))
            with self.assertRaisesRegex(ValueError, "ext4 filesystem"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme7n1", "--winmaint-gib", "48"))
            i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme6n1", "--winmaint-gib", "48"))
            # Installed as its own disk, with no partition table, 4Kn disks are refused the same way.
            with self.assertRaisesRegex(ValueError, "512-byte sectors"):
                i.check_targets(self.args("--winmaint-disk", "/dev/nvme4n1", "--winmaint-gib", "48"))
        with mock.patch.object(i, "output", self.gap_output), \
             mock.patch.object(i, "cpu_model", lambda level: "ivybridge"), \
             mock.patch.object(i, "tpm_status", lambda: {"tpm_version": 0}), \
             mock.patch.object(i, "host_security", lambda: None), \
             mock.patch.object(i, "tpm_advice", lambda st=None: (False, "passphrase", "no TPM")):
            desc = i.describe(i.make_parser())
        disks = {d["path"]: d for d in desc["disks"]}
        self.assertEqual(disks["/dev/nvme1n1"]["free_tail"], (1048575966 - 1006632960 + 1) * 512)
        self.assertEqual(disks["/dev/nvme1n1"]["log_sec"], 512)
        self.assertIsNone(disks["/dev/nvme4n1"]["free_tail"])
        self.assertTrue(disks["/dev/nvme8n1"]["maintenance_windows"])
        self.assertEqual(disks["/dev/nvme8n1"]["children"][1]["maintenance_windows"], True)

    def test_resume_rechecks_changed_winmaint_inputs_and_keeps_a_recorded_one(self):
        def check(*argv):
            with mock.patch.object(i, "output", self.gap_output):
                i.check_targets(self.args("--root", str(self.root), "--resume", *argv))
        stages = self.root / ".locachy-installer"
        stages.mkdir(parents=True, exist_ok=True)
        # The journal says the winmaint stage succeeded with other inputs: the new ones are checked anyway.
        (stages / "stages.json").write_text(json.dumps({"schema": 1, "stages": {"winmaint": {"success": True}}}))
        check("--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "48")
        with self.assertRaisesRegex(ValueError, "unallocated at the end of the disk"):
            check("--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "49")
        state = self.root / "var/lib/locachy/winmaint/state.json"
        state.parent.mkdir(parents=True, exist_ok=True)
        state.write_text(json.dumps({"schema": 1, "disk": "/dev/nvme2n1", "partitions": {"c": {"sectors": 48 * 2097152}}}))
        check("--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "48")
        with self.assertRaisesRegex(ValueError, "is kept"):
            check("--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "96")
        with self.assertRaisesRegex(ValueError, "is kept"):
            check("--winmaint-disk", "/dev/nvme3n1", "--winmaint-gib", "48")
        state.write_text("{}")
        with self.assertRaisesRegex(ValueError, "cannot be read"):
            check("--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "48")
        # Resuming on the already-partitioned system disk: enlarged --winmaint-gib is checked
        # against the real tail left after the system partitions (no blind bypass).
        state.unlink()
        mounted = json.loads(json.dumps(self.GAP_BLOCK))
        mounted["blockdevices"][2]["children"][0]["mountpoints"] = [str(self.root)]
        with mock.patch.object(i, "output", lambda cmd: self.tree_output(mounted, cmd)), \
             mock.patch.object(i, "filesystem", lambda p: {"source": "/dev/nvme2n1p1", "target": str(p)}):
            i.check_targets(self.args("--root", str(self.root), "--esp", str(self.esp), "--resume",
                                      "--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "48"))
            with self.assertRaisesRegex(ValueError, "unallocated at the end of the disk"):
                i.check_targets(self.args("--root", str(self.root), "--esp", str(self.esp), "--resume",
                                          "--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "49"))

    def test_installing_over_maintenance_windows_is_refused_before_any_write(self):
        with mock.patch.object(i, "output", self.gap_output):
            with self.assertRaisesRegex(ValueError, "holds Maintenance Windows partitions; installing here would erase them"):
                i.check_targets(self.args("--disk", "/dev/nvme8n1"))
            with self.assertRaisesRegex(ValueError, "nvme8n1p2: a Maintenance Windows partition"):
                i.check_targets(self.args("--format-esp", "/dev/nvme8n1p2", "--format-root", "/dev/nvme8n1p1"))
            with self.assertRaisesRegex(ValueError, "nvme8n1p4: a Maintenance Windows partition"):
                i.check_targets(self.args("--format-esp", "/dev/nvme8n1p1", "--format-root", "/dev/nvme8n1p4"))
            with self.assertRaisesRegex(ValueError, "holds Maintenance Windows partitions; locachy-storage never formats it"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1",
                                          "--extra-volume", "name=d,mode=format,dev=/dev/nvme8n1,fs=ext4,role=shared"))
            with self.assertRaisesRegex(ValueError, "is a Maintenance Windows partition"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1",
                                          "--extra-volume", "name=w,mode=attach,dev=/dev/nvme8n1p4,role=data"))
            with self.assertRaisesRegex(ValueError, "already holds Maintenance Windows partitions"):
                i.check_targets(self.args("--disk", "/dev/nvme0n1", "--winmaint-disk", "/dev/nvme8n1", "--winmaint-gib", "48"))
            with self.assertRaisesRegex(ValueError, "not a whole disk"):
                i.check_targets(self.args("--disk", "/dev/nvme8n1p1"))

    def test_failed_winmaint_leaves_a_bootable_target_and_resume_rechecks_after_extra_volumes(self):
        # A failure in Maintenance Windows happens after the boot image is published to the ESP.
        run_dir = Path(self.tmp.name) / "run-wm-fail"
        run_dir.mkdir(parents=True)
        pw_file = Path(self.tmp.name) / "wm-fail.pass"
        pw_file.write_text("UserPass123")
        (self.root / "etc/locachy").mkdir(parents=True, exist_ok=True)
        (self.root / "etc/locachy/packages").write_text("base\n")
        (self.root / "etc/locachy/rc.conf").write_text('winmaint_size_gib="48"\n')
        (self.root / "var/lib/locachy").mkdir(parents=True, exist_ok=True)
        boot_img = self.root / "usr/local/lib/locachy/boot/locachy.efi"
        boot_img.parent.mkdir(parents=True, exist_ok=True)
        boot_img.write_bytes(b"EFI")

        fail_wm = [True]
        calls = []
        esp_seen = []

        def fake_run(cmd, check=True, **kw):
            calls.append(cmd)
            if cmd[0] == "umount" and Path(cmd[1]).is_dir():
                for child in Path(cmd[1]).iterdir():
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
            if "locachy-winmaint" in str(cmd):
                esp_seen.append(((run_dir / "esp-1/EFI/locachy/locachy.efi").is_file(),
                                 (run_dir / "esp-1/EFI/BOOT/BOOTX64.EFI").is_file()))
                if fail_wm[0]:
                    raise subprocess.CalledProcessError(1, cmd)
                sf = self.root / "var/lib/locachy/winmaint/state.json"
                sf.parent.mkdir(parents=True, exist_ok=True)
                sf.write_text(json.dumps({"schema": 1, "disk": "/dev/nvme2n1",
                                          "partitions": {"c": {"sectors": 48 * 2097152}}}))
            return subprocess.CompletedProcess(cmd, 0)

        class FakePopen:
            def __init__(self, cmd, **kw):
                self.stdout = iter(["make-root: target built and verified\n"])
            def wait(self):
                return 0

        esp_dir = run_dir / "esp-1"
        def fresh_dir(**kw):
            esp_dir.mkdir(parents=True, exist_ok=True)
            return str(esp_dir)

        args = self.args("--root", str(self.root), "--esp", str(self.esp), "--yes", "--no-rank-mirrors",
                         "--password-file", str(pw_file), "--winmaint-disk", "/dev/nvme2n1", "--winmaint-gib", "48")

        def run_once():
            with mock.patch.object(i, "run", fake_run), \
                 mock.patch.object(i.subprocess, "Popen", FakePopen), \
                 mock.patch.object(i.tempfile, "mkdtemp", fresh_dir), \
                 mock.patch.object(i, "filesystem", lambda p: self.esp_fs if Path(p) == esp_dir else self.root_fs), \
                 mock.patch.object(i, "output", lambda cmd: "gpt" if "PTTYPE" in cmd else self.output(cmd)), \
                 mock.patch.object(i, "cpu_model", lambda lvl: "ivybridge"), \
                 mock.patch.object(i, "update_source", lambda a: ("off", "main", "")), \
                 mock.patch.object(i, "apps_source", lambda a: ("off", "main")), \
                 contextlib.redirect_stdout(io.StringIO()):
                i.install(args, self.root, self.esp, self.root_fs, self.esp_fs, "x86-64-v3")

        with self.assertRaises(subprocess.CalledProcessError):
            run_once()
        # Both EFI/locachy and the fallback were written before locachy-winmaint ran, and boot succeeded.
        self.assertEqual(esp_seen, [(True, True)])
        stages_data = json.loads((self.root / ".locachy-installer/stages.json").read_text())
        self.assertTrue(stages_data["stages"]["boot"]["success"])
        self.assertNotIn("winmaint", stages_data["stages"])

        # The disk helper can fail after saving its GPT state but before
        # writing the Windows container entry. Resume finishes only that file.
        calls.clear()
        fail_wm[0] = False
        sf = self.root / "var/lib/locachy/winmaint/state.json"
        sf.parent.mkdir(parents=True, exist_ok=True)
        sf.write_text(json.dumps({"schema": 1, "disk": "/dev/nvme2n1",
                                  "partitions": {"c": {"sectors": 48 * 2097152}}}))
        (self.root / "etc/passwd").write_text("user:x:1000:1000::/home/user:/bin/bash\n")
        args.resume = True
        helper = i.winmaint_helper()
        with mock.patch.object(i, "winmaint_helper", return_value=helper), \
             mock.patch.object(helper, "parse_gpt_disk", return_value={}) as parse, \
             mock.patch.object(helper, "verify_state_against_gpt") as verify:
            run_once()
        parse.assert_called_once_with("/dev/nvme2n1")
        verify.assert_called_once()
        self.assertFalse(any("locachy-boot" in str(c) or "locachy-winmaint" in str(c) for c in calls))
        self.assertTrue(i.winmaint_container_present(self.root))
        self.assertTrue(i.INSTALL_STATE.Stages(self.root / ".locachy-installer").stages["winmaint"]["success"])

        # Once extra-volumes has completed, a resumed run with a changed --winmaint-gib on that
        # disk checks the real tail on disk, not the pre-format formula (since format won't run again).
        r2 = self.root.parent / "target-after-extra"
        (r2 / ".locachy-installer").mkdir(parents=True)
        (r2 / ".locachy-installer/stages.json").write_text(json.dumps({"schema": 1, "stages": {
            "extra-volumes": {"success": True}}}))
        volume_file = r2 / "etc/locachy/volumes.json"
        volume_file.parent.mkdir(parents=True)
        volume_file.write_text(json.dumps({"schema": 1, "volumes": [{"name": "data"}]}))
        with mock.patch.object(i, "output", self.gap_output):
            with self.assertRaisesRegex(ValueError, "unallocated at the end of the disk"):
                i.check_targets(self.args(
                    "--root", str(r2), "--resume",
                    "--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared",
                    "--winmaint-disk", "/dev/nvme1n1", "--winmaint-gib", "48",
                ))

    def test_unrecorded_extra_volume_failure_is_not_reformatted_on_resume(self):
        args = self.args("--extra-volume", "name=data,mode=format,dev=/dev/nvme1n1")
        with mock.patch.object(i, "run", side_effect=subprocess.CalledProcessError(1, "storage")) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                i.apply_extra_volumes(args, self.root)
            with self.assertRaisesRegex(ValueError, "will not format them again"):
                i.apply_extra_volumes(args, self.root)
        run.assert_called_once()
        self.assertTrue(i.extra_volume_pending(self.root, "data").is_file())

    def test_system_disk_floor_includes_the_partition_slack(self):
        with mock.patch.object(i, "output", self.gap_output):
            with self.assertRaisesRegex(ValueError, "at least 25 GiB"):
                i.check_targets(self.args("--disk", "/dev/nvme9n1"))
            i.check_targets(self.args("--disk", "/dev/nvme10n1"))
        # The floor is what partitioning needs: 1 MiB before the first partition, 2 MiB kept at the end.
        self.assertEqual(i.DISK_SLACK_BYTES + i.ESP_BYTES + (8 << 30) + i.MIN_ROOT_BYTES, self.FLOOR)
        # With Maintenance Windows on the system disk, check_targets and prepare_partitions share the exact floor.
        wm_floor = self.FLOOR + (48 * 1024 + i.WINMAINT_OVERHEAD_MIB) * (1 << 20)
        tree = json.loads(json.dumps(self.GAP_BLOCK))
        tree["blockdevices"][9]["size"] = wm_floor - 1
        tree["blockdevices"][10]["size"] = wm_floor
        with mock.patch.object(i, "output", lambda cmd: self.tree_output(tree, cmd)):
            with self.assertRaisesRegex(ValueError, "with Maintenance Windows"):
                i.check_targets(self.args("--disk", "/dev/nvme9n1", "--winmaint-disk", "/dev/nvme9n1", "--winmaint-gib", "48"))
            i.check_targets(self.args("--disk", "/dev/nvme10n1", "--winmaint-disk", "/dev/nvme10n1", "--winmaint-gib", "48"))
        part_target = self.root.parent / "floor-target"
        with mock.patch.object(i, "run", lambda cmd, **kw: None), \
             mock.patch.object(i, "disk_size_bytes", lambda d: wm_floor):
            i.prepare_partitions(self.args("--disk", "/dev/nvme10n1", "--swap-gib", "8",
                                           "--winmaint-disk", "/dev/nvme10n1", "--winmaint-gib", "48",
                                           "--root", str(part_target), "--esp", str(part_target / "efi")))
        with mock.patch.object(i, "run", lambda cmd, **kw: None), \
             mock.patch.object(i, "disk_size_bytes", lambda d: wm_floor - 1), \
             self.assertRaisesRegex(ValueError, "no room"):
            i.prepare_partitions(self.args("--disk", "/dev/nvme9n1", "--swap-gib", "8",
                                           "--winmaint-disk", "/dev/nvme9n1", "--winmaint-gib", "48",
                                           "--root", str(part_target), "--esp", str(part_target / "efi")))


if __name__ == "__main__":
    unittest.main()
