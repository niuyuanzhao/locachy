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

    APPS_URL = "https://example.org/locachy-apps.git"

    def make_catalog(self, presets, commit="c" * 40, dest=None, origin="repository"):
        dest = dest or self.apps
        dest.parent.mkdir(parents=True, exist_ok=True)
        i.APPS.write_catalog({name: text.encode() for name, text in presets.items()}, str(dest),
                             i.APPS.new_meta("1", origin, self.APPS_URL, "next", commit))
        return dest

    def output(self, command):
        if command[0] == "findmnt":
            if "--mountpoint" in command:
                path = command[command.index("--mountpoint") + 1]
                return json.dumps({"filesystems": [m for m in self.mounts if m["target"] == path]})
            return json.dumps({"filesystems": self.mounts})
        if command[0] == "blkid":
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
        self.make_catalog({"firefox": 'package="firefox"\ncommand="/usr/bin/firefox"\n'})
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
                                           {"name": "firefox", "host": False}])
        self.assertEqual((d["defaults"]["apps_repo"], d["defaults"]["apps_branch"]), (self.APPS_URL, "next"))
        self.assertEqual(d["apps"], {"text": f"{'c' * 12} of {self.APPS_URL} (next)", "not_newest": False})
        self.assertEqual(d["machines"][0], {"name": "generic", "description": "Generic PC"})
        self.assertIn("x230-agan", [m["name"] for m in d["machines"]])
        self.assertEqual([x["name"] for x in d["ime"]], ["off", "en", "zh"])
        self.assertEqual(d["timezones"][0], "UTC")

    def test_targets_checked_before_anything_is_written(self):
        with mock.patch.object(i, "output", self.block_output):
            i.check_targets(self.args("--disk", "/dev/sda"))
            for argv, said in ((["--disk", "/dev/sdb"], "live medium"),
                               (["--disk", "/dev/sdc"], "at least 24 GiB"),
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
        install_id = record["install_id"]
        self.assertRegex(install_id, "^[0-9a-f]{32}$")
        self.assertEqual(oct(self.record.stat().st_mode & 0o777), "0o600")
        self.assertEqual(record["update_repo"], "https://example.org/l.git")
        self.assertEqual((record["apps_repo"], record["apps_commit"]), ("https://example.org/a.git", "d" * 40))
        self.assertIn("--features", record["argv"])
        self.assertNotIn("--password-file", record["argv"])  # never a secret, nor where it was
        self.assertEqual(calls[-1][calls[-1].index("--install-id") + 1], install_id)
        self.assertNotIn("--resume", calls[-1])
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
        kept = self.record.with_name("apps-catalog")
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
        # --resume: its options as recorded, the password again; the same ID;
        # the app presets the target has already (pinned), not this run's.
        calls.clear()
        os.environ["LOCACHY_APPS_CATALOG"] = str(self.apps)
        shutil.rmtree(self.apps)
        self.make_catalog({}, commit="e" * 40)  # this run's: no firefox
        pinned = self.make_catalog(firefox, commit="d" * 40, dest=self.root / i.APPS_CATALOG)
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
        self.assertEqual(os.environ["LOCACHY_APPS_CATALOG"], str(pinned))  # the inner run's too
        self.assertEqual(checked[-1], install_id)
        self.assertFalse(self.record.exists())
        self.assertFalse(kept.exists())
        # Nothing to continue now.
        with mock.patch.object(sys, "argv", ["locachy-install", "--resume"]), \
             mock.patch.object(i.os, "geteuid", lambda: 0), contextlib.redirect_stderr(io.StringIO()) as err, \
             self.assertRaises(SystemExit):
            i.main()
        self.assertIn("Nothing to continue", err.getvalue())

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


if __name__ == "__main__":
    unittest.main()
