#!/usr/bin/env python3
"""Pure fixtures for installer ISO entry parsing and production-home navigation.
  python3 -B tools/test-installer-vm-unit.py SCRATCH_PARENT
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("installer_vm", Path(__file__).with_name("test-installer-vm.py"))
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class Match:
    def __init__(self, value):
        self.value = value

    def group(self, index=0):
        return str(self.value).encode()


class FakeGuest:
    def __init__(self, napps):
        self.napps = napps
        self.actions = []
        self.alive = False

    def wait_for(self, expression):
        self.actions.append(("wait", expression))

    def any(self, expression):
        self.actions.append(("match", expression))
        return Match(self.napps)

    def pump(self, seconds):
        pass

    def shot(self, name):
        return {"rendered": True, "name": name}

    def key(self, *keys):
        self.actions.append(("key", keys))

    def type(self, text):
        self.actions.append(("type", text))


class InstallerVM(unittest.TestCase):
    def test_parse_expanded_real_iso_boot_entry_without_kernel_shortcut(self):
        with tempfile.TemporaryDirectory(prefix="iso-entry-", dir=SCRATCH) as temp:
            out = Path(temp)
            config = """title    LOCachy installer (x86_64, UEFI)
sort-key 01
linux    /arch/boot/x86_64/vmlinuz-linux
initrd   /arch/boot/intel-ucode.img
initrd   /arch/boot/x86_64/initramfs-linux.img
options  archisobasedir=arch archisosearchuuid=2026-10-23-00-00-00-00
"""
            calls = []

            def extract(command, **kwargs):
                calls.append(command)
                (out / "iso-loader-entry.conf").write_text(config)
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(vm.subprocess, "run", extract):
                kernel, initrd = vm.iso_commands(Path("/fixture/installer.iso"), out)
            self.assertTrue(kernel.startswith("/arch/boot/x86_64/vmlinuz-linux "))
            self.assertIn("archisosearchuuid=2026-10-23-00-00-00-00", kernel)
            self.assertEqual(initrd, "/arch/boot/intel-ucode.img,/arch/boot/x86_64/initramfs-linux.img")
            self.assertEqual(calls[0][0], "xorriso")
            self.assertEqual(calls[0][calls[0].index("-extract") + 1], "/loader/entries/01-archiso-linux.conf")

    def test_template_or_missing_iso_entries_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="iso-entry-", dir=SCRATCH) as temp:
            out = Path(temp)
            for content in ("linux /%INSTALL_DIR%/boot/x86_64/vmlinuz-linux\ninitrd /arch/initramfs.img\n"
                            "options archisobasedir=%INSTALL_DIR%\n",
                            "title empty\n", "linux /arch/vmlinuz\noptions a=b\n"):
                def extract(command, **kwargs):
                    (out / "iso-loader-entry.conf").write_text(content)
                    return subprocess.CompletedProcess(command, 0)
                with mock.patch.object(vm.subprocess, "run", extract), self.assertRaises(RuntimeError):
                    vm.iso_commands(Path("/fixture/installer.iso"), out)

    def test_keys_use_conservative_press_release_timing(self):
        guest = vm.Guest.__new__(vm.Guest)
        calls, waits = [], []
        guest.command = lambda command, args: calls.append((command, args))
        guest.pump = waits.append
        guest.key("shift", "l")
        self.assertEqual(calls[0][1]["hold-time"], 80)
        self.assertEqual(waits, [0.16])

    def test_production_apps_are_counted_before_builtin_files_and_term(self):
        for count in (0, 2, 5):
            with self.subTest(apps=count), tempfile.TemporaryDirectory(prefix="target-navigation-", dir=SCRATCH) as temp:
                out = Path(temp)
                (out / "installed-boot").mkdir()
                (out / "installed-boot/serial.log").write_text("init: root: /dev/sda2, esp: /dev/sda1\n")
                guest = FakeGuest(count)
                with mock.patch.object(vm, "launch", return_value=(object(), guest, object())), \
                        mock.patch.object(vm, "end_vm") as end:
                    result = vm.target_boot(object(), out / "target.img", out)
                home = guest.actions.index(("key", ("home",)))
                selection = guest.actions[home + 1:home + count + 3]
                self.assertEqual(selection, [("key", ("down",))] * (count + 1) + [("key", ("ret",))])
                self.assertIn("home-returned", result)
                self.assertEqual(guest.actions.count(("type", vm.PASSWORD)), 1)
                end.assert_called_once()
                self.assertTrue(end.call_args.kwargs["completed"])


if __name__ == "__main__":
    unittest.main()
