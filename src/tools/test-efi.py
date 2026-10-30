#!/usr/bin/env python3
"""UEFI registration safety without firmware writes; real OVMF is separate."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("efi", str(SOURCE / "link/locachy-efi"))
spec = importlib.util.spec_from_loader(loader.name, loader)
efi = importlib.util.module_from_spec(spec)
loader.exec_module(efi)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
GUID = "8129ca5c-784a-47e7-921a-d7a952726599"


def row(number="0003", active=True, guid=GUID, loader=efi.LOADER, part=1):
    return f"Boot{number}{'*' if active else ' '} LOCachy\tHD({part},GPT,{guid},0x800,0x100000)/File({loader})\n"


class Registration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="efi-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.esp = self.root / "esp"
        (self.esp / "EFI/locachy").mkdir(parents=True)
        self.image = self.esp / "EFI/locachy/locachy.efi"
        self.image.write_bytes(b"MZ\x00test")
        self.efi = self.root / "firmware"
        self.variables = self.efi / "efivars"
        self.variables.mkdir(parents=True)
        self.device = "/dev/nvme0n1p1"
        self.disk = "/dev/nvme0n1"
        self.fs = {"target": str(self.esp), "source": self.device, "fstype": "vfat", "options": "rw", "maj:min": "8:1"}
        self.part = {"path": self.device, "type": "part", "pkname": self.disk, "partn": 1,
                     "partuuid": GUID, "parttype": efi.ESP_TYPE, "pttype": "gpt", "maj:min": "8:1"}
        self.order = ["0001", "0002"]
        self.records = []
        self.calls = []
        self.fail = None
        patches = [mock.patch.object(efi, "EFI", self.efi), mock.patch.object(efi, "EFIVARS", self.variables),
                   mock.patch.object(efi, "command", self.command), mock.patch.object(efi.os, "stat", self.stat)]
        self.real_stat = os.stat
        # Capture stat before starting the replacement (the os module is shared).
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def stat(self, path, *args, **kwargs):
        if str(path) == self.device:
            return types.SimpleNamespace(st_mode=stat.S_IFBLK | 0o600, st_rdev=os.makedev(8, 1))
        return self.real_stat(path, *args, **kwargs)

    def text(self):
        return "BootCurrent: 0001\nBootOrder: " + ",".join(self.order) + "\n" + "".join(self.records)

    def command(self, args):
        self.calls.append(args)
        if args[0] == "findmnt":
            target = args[args.index("--mountpoint") + 1]
            fs = self.fs if target == str(self.esp) else {"target": str(self.variables), "fstype": "efivarfs", "options": "rw"}
            return json.dumps({"filesystems": [fs]})
        if args[0] == "lsblk":
            return json.dumps({"blockdevices": [self.part if args[-1] == self.device else {"type": "disk"}]})
        self.assertEqual(args[0], "efibootmgr")
        if args[1:] == ["--verbose"]:
            return self.text()
        if self.fail:
            raise RuntimeError(self.fail)
        if "--create-only" in args:
            self.records.append(row())
        elif "--active" in args:
            number = args[args.index("--bootnum") + 1]
            self.records = [r.replace("Boot" + number + " ", "Boot" + number + "*") for r in self.records]
        elif "--bootorder" in args:
            self.order = args[-1].split(",")
        else:
            self.fail("Unexpected mutation " + repr(args))
        return ""

    def register(self, first=False):
        with contextlib.redirect_stdout(io.StringIO()):
            return efi.register(self.esp, first)

    def mutations(self):
        return [c for c in self.calls if c[0] == "efibootmgr" and c[1:] != ["--verbose"]]

    def test_create_reuses_target_guid_and_partition_and_appends(self):
        self.assertEqual(self.register(), 0)
        self.assertEqual(self.order, ["0001", "0002", "0003"])
        self.assertEqual(self.mutations()[0], ["efibootmgr", "--create-only", "--disk", self.disk, "--part", "1",
                                               "--label", "LOCachy", "--loader", efi.LOADER])
        self.calls.clear()
        self.assertEqual(self.register(), 0)
        self.assertEqual(self.mutations(), [])

    def test_installer_first_preserves_other_entries_and_update_keeps_order(self):
        self.register(True)
        self.assertEqual(self.order, ["0003", "0001", "0002"])
        self.order = ["0002", "0003", "0001"]
        self.calls.clear()
        self.register()
        self.assertEqual(self.order, ["0002", "0003", "0001"])
        self.assertEqual(self.mutations(), [])

    def test_inactive_and_missing_from_order_repaired_without_recreation(self):
        self.records = [row(active=False)]
        self.register()
        self.assertEqual(self.order, ["0001", "0002", "0003"])
        self.assertFalse(any("--create-only" in c for c in self.mutations()))
        self.assertIn(["efibootmgr", "--bootnum", "0003", "--active"], self.mutations())

    def test_same_label_elsewhere_is_not_deleted_or_hijacked(self):
        foreign = row("0009", guid="9129ca5c-784a-47e7-921a-d7a952726599")
        self.records = [foreign]
        self.order.append("0009")
        self.register()
        self.assertIn(foreign, self.records)
        self.assertEqual(self.order, ["0001", "0002", "0009", "0003"])
        self.assertFalse(any("--delete-bootnum" in c for c in self.calls))

    def test_wrong_loader_or_partition_is_not_reused(self):
        self.records = [row("0008", loader=r"\EFI\other.efi"), row("0009", part=2)]
        self.register()
        self.assertTrue(any("--create-only" in c for c in self.mutations()))

    def test_sd_nvme_and_mmc_names_use_metadata_not_string_trimming(self):
        for disk, part in [("/dev/sda", "/dev/sda3"), ("/dev/nvme0n1", "/dev/nvme0n1p3"), ("/dev/mmcblk0", "/dev/mmcblk0p3")]:
            self.device, self.disk = part, disk
            self.fs["source"] = part
            self.part.update(path=part, pkname=disk, partn=3)
            self.assertEqual(efi.esp_target(self.esp), (disk, 3, GUID))

    def test_non_esp_or_identity_mismatch_refused(self):
        for field, bad in [("parttype", "linux"), ("pttype", "dos"), ("type", "crypt"), ("partuuid", "bad"), ("partn", 0), ("maj:min", "8:9")]:
            before = self.part.copy()
            self.part[field] = bad
            with self.assertRaises(RuntimeError):
                self.register()
            self.part = before
        self.assertEqual(self.mutations(), [])

    def test_missing_loader_or_not_a_mount_refused(self):
        self.image.unlink()
        with self.assertRaisesRegex(RuntimeError, "boot image required"):
            self.register()
        self.image.write_text("image")
        self.fs["target"] = str(self.root)
        with self.assertRaisesRegex(RuntimeError, "explicit filesystem mount"):
            self.register()
        self.assertEqual(self.mutations(), [])

    def test_no_uefi_readonly_or_unavailable_variables_do_not_mutate(self):
        with mock.patch.object(efi, "EFI", self.root / "absent"):
            with self.assertRaisesRegex(RuntimeError, "not booted in UEFI"):
                self.register()
        original = self.command
        def readonly(args):
            out = original(args)
            if args[0] == "findmnt" and str(self.variables) in args:
                return out.replace('"rw"', '"ro"')
            return out
        with mock.patch.object(efi, "command", readonly):
            with self.assertRaisesRegex(RuntimeError, "read-only"):
                self.register()
        self.assertEqual(self.mutations(), [])

    def test_command_failure_keeps_boot_image_and_order(self):
        self.fail = "No space left on device"
        with self.assertRaisesRegex(RuntimeError, "No space"):
            self.register()
        self.assertEqual(self.order, ["0001", "0002"])
        self.assertTrue(self.image.is_file())

    def test_partial_firmware_failure_retries_without_another_entry(self):
        original = self.command
        failed = []
        def fail_order(args):
            if "--bootorder" in args and not failed:
                failed.append(True)
                raise RuntimeError("firmware refused BootOrder")
            return original(args)
        with mock.patch.object(efi, "command", fail_order):
            with self.assertRaisesRegex(RuntimeError, "refused BootOrder"):
                self.register()
        self.assertEqual(len(self.records), 1)
        self.calls.clear()
        self.register()
        self.assertFalse(any("--create-only" in c for c in self.mutations()))
        self.assertEqual(self.order, ["0001", "0002", "0003"])

    def test_parse_case_and_full_device_path(self):
        text = "BootOrder: 000a,000B\n" + row("000a", guid=GUID.upper(), loader=efi.LOADER.upper()).replace(
            "HD(", "PciRoot(0x0)/Pci(0x1,0)/HD(")
        order, records = efi.entries(text)
        self.assertEqual(order, ["000A", "000B"])
        self.assertEqual(records[0]["guid"], GUID)
        self.assertEqual(records[0]["loader"], efi.LOADER.lower())

    def test_efivar_bare_file_path_is_recognised(self):
        text = row().replace("File(" + efi.LOADER + ")", efi.LOADER)
        _, records = efi.entries(text)
        self.assertEqual(records[0]["loader"], efi.LOADER.lower())
        self.assertEqual(efi.entries(text.replace(" LOCachy\t", " LOCachy Backup\t"))[1], [])

    def test_normal_update_refuses_a_stale_unmounted_efi_directory(self):
        boot = (SOURCE / "link/locachy-boot").read_text()
        start = boot.index('# An old directory under /efi')
        end = boot.index('[ -d "/usr/lib/modules/$version" ]', start)
        block = boot[start:end]
        tools = self.root / "bin"
        tools.mkdir()
        findmnt = tools / "findmnt"
        findmnt.write_text('#!/bin/sh\nexit 1\n')
        findmnt.chmod(0o755)
        env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
        command = 'esp="$1/EFI"; boot="$1/image"\n' + block + '\nprintf "UNSAFE-WRITE\\n"\n'
        result = subprocess.run(["sh", "-eu", "-c", command, "sh", str(self.esp)], env=env,
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no FAT ESP mounted", result.stdout)
        self.assertNotIn("UNSAFE-WRITE", result.stdout)
        self.assertEqual(self.image.read_bytes(), b"MZ\x00test")
        self.assertLess(boot.index('# An old directory under /efi'), boot.index('cp -p "$esp/locachy/locachy.efi"'))

    def test_boot_repair_is_early_and_install_registers_only_after_images(self):
        boot = (SOURCE / "link/locachy-boot").read_text()
        self.assertLess(boot.index('= --register-only'), boot.index('kernel=$(pacman'))
        self.assertIn('exec /usr/local/bin/locachy-efi --esp /efi', boot)
        self.assertIn('if ! /usr/local/bin/locachy-efi --esp /efi', boot)
        installer = (SOURCE / "link/locachy-install").read_text()
        self.assertLess(installer.index('shutil.copyfile(image, fallback / "BOOTX64.EFI")'),
                        installer.index('registered = run('))
        self.assertIn('"--esp", str(saved), "--first"', installer)
        self.assertLess(installer.index('secure_boot_install(args, root, saved)'), installer.index('registered = run('))
        self.assertLess(installer.index('registered = run('), installer.index('state = root / "var/lib/locachy/install.json"'))


if __name__ == "__main__":
    unittest.main()
