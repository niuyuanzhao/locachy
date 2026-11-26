#!/usr/bin/env python3
"""Pure fixtures for installer ISO entry parsing and production-home navigation.
  python3 -B tools/test-installer-vm-unit.py SCRATCH_PARENT
"""
import contextlib
from hashlib import sha256
import importlib.util
import io
import json
from pathlib import Path
import shutil
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


class FakeGuest:
    def __init__(self, out):
        self.out = out
        self.actions = []
        self.alive = False

    def wait_for(self, expression):
        self.actions.append(("wait", expression))

    def pump(self, seconds):
        pass

    def shot(self, name):
        (self.out / (name + ".ppm")).write_text(name)
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

    def test_preflight_stop_prints_logs_without_retry_or_approval(self):
        for stops in (False, True):
            with self.subTest(stops=stops):
                guest = mock.Mock(alive=True)
                guest.remaining.return_value = 240

                def shell(command, *, label=None, output=False):
                    if label in ("target package download (after key preflight)",
                                 "installation completion (no automatic source-key approval)"):
                        self.assertIn("timeout -k 2 232", command)
                        raise RuntimeError(f"Guest command failed (3): {label}")
                    if output:
                        self.assertIn("install.log", command)
                        self.assertIn("refresh.log", command)
                        return "Installation source-key preflight failed: fixture failure\n"

                guest.shell.side_effect = shell
                stderr = io.StringIO()
                with mock.patch.object(vm.subprocess, "run"), mock.patch.object(vm, "boot_live"), \
                        mock.patch.object(vm, "launch", return_value=(object(), guest, object())), \
                        mock.patch.object(vm, "tui_provenance"), mock.patch.object(vm, "end_vm") as end, \
                        contextlib.redirect_stderr(stderr), self.assertRaisesRegex(RuntimeError, r"failed \(3\)"):
                    vm.install_tui(mock.Mock(timeout=240), Path("/fixture/iso"), Path("/fixture/run"), "", "", stops)
                self.assertIn("fixture failure", stderr.getvalue())
                self.assertIn("/fixture/run/install/serial.log", stderr.getvalue())
                self.assertEqual(guest.key.call_args_list.count(mock.call("y")), 1)  # erase confirmation only
                self.assertNotIn(mock.call("c"), guest.key.call_args_list)
                guest.command.assert_not_called()
                self.assertFalse(end.call_args.kwargs["completed"])

    def test_exact_bundled_source_and_catalog_survive_resume_selection(self):
        with tempfile.TemporaryDirectory(prefix="installer-provenance-", dir=SCRATCH) as temp:
            root = Path(temp).resolve()
            slots, target, proc = (root / name for name in ("slots", "target", "proc"))
            pid = proc / "123"
            pid.mkdir(parents=True)
            source = slots / "bundled-source.fixture"
            tracked = ("locachy-install", "locachy_install_state.py", "locachy_apps.py", "modules.json")
            (source / "link").mkdir(parents=True)
            for name in tracked:
                shutil.copyfile(vm.SOURCE.parent / "link" / name, source / "link" / name)
            for name in ("make-root.sh", "packages", "rc.conf"):
                (source / name).write_text(f"selected local {name}\n")
            origin = root / "source-origin.json"
            source_commit = "a" * 40
            repo = "https://example.invalid/system.git"
            origin.write_text(json.dumps(dict(repo=repo, branch="main", commit=source_commit, source_changed=True)))
            state_spec = importlib.util.spec_from_file_location("installation_state", source / "link/locachy_install_state.py")
            state = importlib.util.module_from_spec(state_spec)
            state_spec.loader.exec_module(state)
            how = "bundled: local changes over the fetched commit"

            def catalog(path, commit, package):
                path.mkdir(parents=True)
                (path / "CATALOG").write_text(f'format="3"\norigin="repository"\nrepo="https://example.invalid/apps.git"\n'
                                              f'branch="main"\ncommit="{commit}"\ninstalled="fixture"\n')
                (path / "fixture.conf").write_text(f'host="1"\npackage="{package}"\ncommand="/usr/bin/{package}"\n')

            first_catalog, next_catalog = slots / "apps.first/catalog", slots / "apps.next/catalog"
            catalog(first_catalog, "b" * 40, "original-app")
            catalog(next_catalog, "c" * 40, "refreshed-app")

            def select(src, apps, resume=False):
                record = json.dumps([[repo, "main", "https://example.invalid/apps.git", "main"], str(src), str(apps), how, "fetched"]).encode()
                token = sha256(record).hexdigest()
                (slots / (token + ".json")).write_bytes(record)
                (pid / "cmdline").write_bytes(b"\0".join([b"python3", b"-I", str(src / "link/locachy-install").encode(),
                                                         b"--yes", b"--resume" if resume else b"--disk=/dev/sda", b""]))
                (pid / "environ").write_bytes(f"LOCACHY_INSTALL_SNAPSHOT={token}\0LOCACHY_APPS_CATALOG={apps}\0"
                                               f"LOCACHY_SOURCE={how}\0LOCACHY_APPS=fetched\0".encode())

            def provenance(action):
                return subprocess.run([sys.executable, "-IB", "-c", vm.TUI_PROVENANCE,
                                       str(root), action, str(target), str(slots), str(origin), str(proc)],
                                      capture_output=True, text=True, timeout=20)

            select(source, first_catalog)
            result = provenance("capture")
            self.assertEqual(result.returncode, 0, result.stderr)
            refreshed = slots / "bundled-source.refreshed"
            shutil.copytree(source, refreshed)
            (refreshed / "link/modules.json").write_text((refreshed / "link/modules.json").read_text() + "\n")
            select(refreshed, next_catalog, resume=True)
            result = provenance("capture-resume")
            self.assertEqual(result.returncode, 0, result.stderr)

            lib = target / "usr/local/lib/locachy"
            lib.mkdir(parents=True)
            (lib / "source-commit").write_text(source_commit + "\n")
            installed = target / "usr/local/src/locachy"
            shutil.copytree(refreshed, installed)
            (installed / "packages").write_text("materialized packages\n")
            (installed / "rc.conf").write_text("materialized choices\n")
            installed_catalog = target / "var/lib/locachy/apps/catalog"
            shutil.copytree(first_catalog, installed_catalog)
            meta = installed_catalog / "CATALOG"
            meta.write_text(meta.read_text().replace('installed="fixture"', 'installed="on target"'))
            (target / "var/lib/locachy/install.json").write_text(json.dumps({
                "extra_volumes": [], "containers": [], "features": [], "ime": "off", "repos": ["core", "extra"],
                "cpu": "haswell", "cpu_level": "x86-64-v2"}))
            stages = state.Stages(target / ".locachy-installer")
            with mock.patch.dict("os.environ", {"LOCACHY_INSTALLER": "1", "WITH_IME": "0", "WITH_FEATURES": "",
                                                "WITH_EXTRA_PACKAGES": "cryptsetup", "WITH_REPOS": "core,extra"}):
                stages.complete("sources", state.build_input(refreshed, target, "sources", "x86-64-v2", "haswell"))
            result = provenance("verify-resume")
            self.assertEqual(result.returncode, 0, result.stderr)

            shutil.copyfile(next_catalog / "CATALOG", meta)
            self.assertIn("target app presets differ", provenance("verify-resume").stderr)
            shutil.copyfile(first_catalog / "CATALOG", meta)
            shutil.copyfile(source / "link/modules.json", installed / "link/modules.json")
            self.assertIn("installed source bytes differ", provenance("verify-resume").stderr)

    def test_production_apps_are_counted_before_builtin_files_and_term(self):
        for count in (0, 2, 5):
            with self.subTest(apps=count), tempfile.TemporaryDirectory(prefix="target-navigation-", dir=SCRATCH) as temp:
                out = Path(temp)
                boot_out = out / vm.DIAGNOSTIC_BOOT
                boot_out.mkdir()
                (boot_out / "serial.log").write_text(
                    f"home: {count} containers, 4 host programs\ninit: root: /dev/sda2, esp: /dev/sda1\n")
                guest = FakeGuest(boot_out)
                with mock.patch.object(vm, "launch", return_value=(object(), guest, object())), \
                        mock.patch.object(vm, "end_vm") as end:
                    result = vm.target_boot(out / "target.img", out)
                home = guest.actions.index(("key", ("home",)))
                selection = guest.actions[home + 1:home + count + 4]
                self.assertEqual(selection, [
                    ("key", ("right",))
                ] * (count + 2) + [("key", ("ret",))])
                admin = guest.actions.index(("key", ("home",)), home + 1)
                self.assertEqual(guest.actions[admin + 1:admin + count + 5], [
                    ("key", ("right",))
                ] * (count + 3) + [("key", ("ret",))])
                self.assertIn("home-returned", result)
                self.assertEqual(guest.actions.count(("type", vm.PASSWORD)), 1)
                end.assert_called_once()
                self.assertTrue(end.call_args.kwargs["completed"])


if __name__ == "__main__":
    unittest.main()
