#!/usr/bin/env python3
"""Host-only doubles for the independent Rescue storage helper.

This test never probes or opens a real block device. It records the standard
utility argv/stdin/env contract and supplies a complete fake lsblk/blkid/
findmnt/udevadm world.
"""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader(
    "rescue_storage", str(SOURCE / "installer/locachy-rescue-storage"))
spec = importlib.util.spec_from_loader(loader.name, loader)
helper = importlib.util.module_from_spec(spec)
loader.exec_module(helper)


class Doubles:
    ROOT_LUKS = "11111111-1111-4111-8111-111111111111"
    SWAP_LUKS = "22222222-2222-4222-8222-222222222222"
    ROOT_FS = "33333333-3333-4333-8333-333333333333"
    SWAP_FS = "44444444-4444-4444-8444-444444444444"
    ESP_FS = "55555555-5555-4555-8555-555555555555"

    def __init__(self, target):
        self.target = Path(target)
        self.calls = []
        self.mappers = set()
        self.mapper_ro = {}
        self.mount = None
        self.esp_mount = None
        self.extra_mounts = []
        self.fail_open = False
        self.bad_mapper = False
        self.lsblk = {"blockdevices": [{
            "path": "/dev/sda", "type": "disk", "size": 100000000000,
            "model": "Fake disk", "tran": "sata", "children": [
                {"path": "/dev/sda1", "type": "part", "size": 512000000,
                 "fstype": "vfat", "label": "LOCACHY_EFI", "uuid": self.ESP_FS,
                 "partuuid": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "maj:min": "8:1",
                 "mountpoints": [None]},
                {"path": "/dev/sda2", "type": "part", "size": 8000000000,
                 "fstype": "crypto_LUKS", "label": "locachy-swap-crypt", "uuid": self.SWAP_LUKS,
                 "partuuid": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "maj:min": "8:2",
                 "mountpoints": [None]},
                {"path": "/dev/sda3", "type": "part", "size": 91500000000,
                 "fstype": "crypto_LUKS", "label": "locachy-crypt", "uuid": self.ROOT_LUKS,
                 "partuuid": "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "maj:min": "8:3",
                 "mountpoints": [None]},
            ]} ]}

    @staticmethod
    def completed(argv, stdout=b"", returncode=0):
        return subprocess.CompletedProcess(argv, returncode, stdout, b"")

    def _text(self, data):
        return data.encode() if isinstance(data, str) else data

    def run(self, argv, **kwargs):
        self.calls.append((list(argv), kwargs))
        cmd = list(argv)
        if cmd[:2] == ["udevadm", "settle"]:
            return self.completed(cmd)
        if cmd[:3] == ["lsblk", "--json", "--bytes"]:
            payload = json.loads(json.dumps(self.lsblk))
            if self.mappers:
                children = payload["blockdevices"][0]["children"]
                for number, name, label, uuid, fstype in (
                        (0, next((x for x in self.mappers if x.endswith("-root")), ""), "locachy-root", self.ROOT_FS, "ext4"),
                        (1, next((x for x in self.mappers if x.endswith("-swap")), ""), "locachy-swap", self.SWAP_FS, "swap")):
                    if name:
                        children.append({"path": "/dev/mapper/" + name, "type": "crypt", "size": 1,
                                         "fstype": fstype, "label": label, "uuid": uuid,
                                         "maj:min": "253:" + str(number), "ro": 1 if self.mapper_ro.get(name, True) else 0,
                                         "mountpoints": [None]})
            return self.completed(cmd, json.dumps(payload).encode())
        if cmd[:3] == ["blkid", "-p", "-o"]:
            path = cmd[-1]
            rows = {
                "/dev/sda3": {"TYPE": "crypto_LUKS", "LABEL": "locachy-crypt", "VERSION": "2",
                              "UUID": self.ROOT_LUKS},
                "/dev/sda2": {"TYPE": "crypto_LUKS", "LABEL": "locachy-swap-crypt", "VERSION": "2",
                              "UUID": self.SWAP_LUKS},
                "/dev/sda1": {"TYPE": "vfat", "LABEL": "LOCACHY_EFI", "UUID": self.ESP_FS},
            }
            if path == "/dev/mapper/fake-root":
                rows[path] = {"TYPE": "ext4", "LABEL": "locachy-root", "UUID": self.ROOT_FS}
            if path == "/dev/mapper/fake-swap":
                rows[path] = {"TYPE": "swap", "LABEL": "locachy-swap", "UUID": self.SWAP_FS}
            return self.completed(cmd, "".join(f"{key}={value}\n" for key, value in rows.get(path, {}).items()).encode())
        if cmd[:2] == ["findmnt", "--json"]:
            mounts = ([self.mount] if self.mount else []) + ([self.esp_mount] if self.esp_mount else []) + self.extra_mounts
            return self.completed(cmd, json.dumps({"filesystems": mounts}).encode())
        if cmd[:2] == ["findmnt", "--fstab"]:
            return self.completed(cmd, b'{"filesystems": []}\n')
        if cmd[:3] == ["udevadm", "info", "--query=property"]:
            path = cmd[-1]
            name = Path(path).name
            resource = next((x for x in ("root", "swap") if name.endswith("-" + x)), None)
            if name not in self.mappers:
                return self.completed(cmd, b"", 1)
            if not resource or name not in self.mappers:
                return self.completed(cmd, b"", 1)
            uuid = self.ROOT_LUKS if resource == "root" else self.SWAP_LUKS
            dm_uuid = ("WRONG" if self.bad_mapper else
                       "CRYPT-LUKS2-" + uuid.replace("-", "") + "-" + name)
            major_minor = "253:0" if resource == "root" else "253:1"
            text = f"DM_NAME={name}\nDM_UUID={dm_uuid}\nMAJOR={major_minor.split(':')[0]}\nMINOR={major_minor.split(':')[1]}\n"
            return self.completed(cmd, text.encode())
        if cmd[:2] == ["cryptsetup", "open"]:
            if self.fail_open:
                return self.completed(cmd, b"", 5)
            name = cmd[-1]
            self.mappers.add(name)
            self.mapper_ro[name] = "--readonly" in cmd
            return self.completed(cmd)
        if cmd[:2] == ["cryptsetup", "close"]:
            self.mappers.discard(cmd[-1])
            self.mapper_ro.pop(cmd[-1], None)
            return self.completed(cmd)
        if cmd[0] == "mount":
            mapper = cmd[-2]
            target = Path(cmd[-1])
            name = Path(mapper).name
            if "vfat" in cmd:
                self.esp_mount = {"id": "43", "target": str(target), "source": mapper, "fstype": "vfat",
                                  "options": "ro" if "ro" in cmd else "rw", "maj:min": "8:1"}
                return self.completed(cmd)
            self.mount = {"id": "42", "target": str(target), "source": mapper, "fstype": "ext4",
                          "options": "ro,noload" if "ro,noload" in cmd else "rw", "maj:min": "253:0"}
            target.mkdir(parents=True, exist_ok=True)
            (target / "etc/locachy").mkdir(parents=True)
            (target / "etc/os-release").write_text('NAME="LOCachy"\nID=locachy\n')
            (target / "etc/locachy/rc.conf").write_text('')
            (target / "efi").mkdir()
            return self.completed(cmd)
        if cmd[:2] == ["umount", "--"]:
            if self.esp_mount and cmd[-1] == self.esp_mount["target"]:
                self.esp_mount = None
                return self.completed(cmd)
            self.mount = None
            self.target = Path(cmd[-1])
            for child in self.target.iterdir():
                if child.is_dir():
                    for item in sorted(child.rglob("*"), reverse=True):
                        if item.is_file() or item.is_symlink():
                            item.unlink()
                        elif item.is_dir():
                            item.rmdir()
                    child.rmdir()
                else:
                    child.unlink()
            return self.completed(cmd)
        raise AssertionError("unexpected command: " + repr(cmd))

    def mounts(self):
        return ([self.mount] if self.mount else []) + ([self.esp_mount] if self.esp_mount else []) + self.extra_mounts

    def probe(self, path):
        values = {
            "/dev/sda3": {"TYPE": "crypto_LUKS", "LABEL": "locachy-crypt", "VERSION": "2", "UUID": self.ROOT_LUKS},
            "/dev/sda2": {"TYPE": "crypto_LUKS", "LABEL": "locachy-swap-crypt", "VERSION": "2", "UUID": self.SWAP_LUKS},
            "/dev/sda1": {"TYPE": "vfat", "LABEL": "LOCACHY_EFI", "UUID": self.ESP_FS},
            "/dev/mapper/fake-root": {"TYPE": "ext4", "LABEL": "locachy-root", "UUID": self.ROOT_FS},
            "/dev/mapper/fake-swap": {"TYPE": "swap", "LABEL": "locachy-swap", "UUID": self.SWAP_FS},
        }
        if str(path).startswith("/dev/mapper/") and str(path).endswith("-root"):
            return values["/dev/mapper/fake-root"]
        if str(path).startswith("/dev/mapper/") and str(path).endswith("-swap"):
            return values["/dev/mapper/fake-swap"]
        return values.get(path, {})


class RescueStorage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="rescue-storage-")
        self.d = Doubles(Path(self.tmp.name) / "target")
        self.runtime = Path(self.tmp.name) / "run"
        self.patches = [
            mock.patch.object(helper, "RUNTIME", self.runtime),
            mock.patch.object(helper.subprocess, "run", self.d.run),
            mock.patch.object(helper, "probe", self.d.probe),
            mock.patch.object(helper, "mounts", self.d.mounts),
            mock.patch.object(helper.os.path, "lexists", lambda path: Path(path).name in self.d.mappers),
        ]
        for patch in self.patches:
            patch.start()
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        for patch in reversed(self.patches):
            patch.stop()
        self.tmp.cleanup()

    def storage(self):
        return helper.Storage(self.runtime)

    def test_scan_uses_standard_lsblk_and_reports_only_locachy_roots(self):
        with self.storage() as storage:
            roots = [helper.public_device(n) for n in helper.root_candidates(helper.inventory())]
        self.assertEqual([root["path"] for root in roots], ["/dev/sda3"])
        self.assertEqual(roots[0]["size"], 91500000000)
        calls = [argv for argv, _ in self.d.calls]
        self.assertEqual(calls[0], ["udevadm", "settle", "--timeout=10"])
        self.assertEqual(calls[1], helper.LSBLK)

    def test_status_also_returns_current_inventory(self):
        output = io.StringIO()
        old_umask = os.umask(0o077)
        try:
            with mock.patch.object(helper.os, "geteuid", return_value=os.getuid()), \
                    mock.patch.object(helper, "read_key", side_effect=AssertionError("status read a key")), \
                    contextlib.redirect_stdout(output):
                # main's root gate is separate from Storage's actual uid check.
                with mock.patch.object(helper.os, "geteuid", return_value=0), \
                        mock.patch.object(helper, "private_directory", lambda p: p.mkdir(mode=0o700, exist_ok=True)), \
                        mock.patch.object(helper, "private_file", lambda p, flags: os.open(p, flags, 0o600)):
                    self.assertEqual(helper.main(["status"]), 0)
        finally:
            os.umask(old_umask)
        result = json.loads(output.getvalue())
        self.assertEqual([d["path"] for d in result["devices"]], ["/dev/sda3"])
        self.assertFalse(result["target"]["cleanup_pending"])

    def test_readonly_unlock_has_exact_argv_raw_stdin_and_no_secret_in_env(self):
        secret = b"raw\x00secret\xff"
        with self.storage() as storage:
            storage.unlock("/dev/sda3", secret)
            self.assertEqual(storage.data["mode"], "readonly")
            self.assertEqual(storage.data["esp_mount"]["target"], str(self.runtime / "target" / "efi"))
            self.assertEqual(storage.data["esp_mount"]["uuid"], self.d.ESP_FS)
            self.assertEqual(storage.data["esp_mount"]["maj:min"], "8:1")
        opens = [(argv, kw) for argv, kw in self.d.calls if argv[:2] == ["cryptsetup", "open"]]
        self.assertEqual(len(opens), 2)  # root and independently verified swap
        root_argv, root_kw = opens[0]
        self.assertEqual(root_argv[:-1], ["cryptsetup", "open", "--readonly", "--disable-external-tokens",
                                         "--key-file=-", "--type", "luks2", "/dev/sda3"])
        self.assertRegex(root_argv[-1], r"^locachy-rescue-[0-9a-f]{32}-root$")
        self.assertEqual(root_kw["input"], secret)
        self.assertNotIn(secret, root_argv)
        self.assertNotIn("secret", root_kw["env"])
        self.assertEqual(opens[1][0][2:6], ["--readonly", "--disable-external-tokens", "--key-file=-", "--type"])
        self.assertTrue(self.d.mount["options"].startswith("ro"))
        self.assertNotIn("swapon", [part for argv, _ in self.d.calls for part in argv])
        self.assertNotIn("--allow-discards", [part for argv, _ in self.d.calls for part in argv])

    def test_wrong_key_cleans_only_journaled_resources(self):
        self.d.fail_open = True
        with self.storage() as storage:
            with self.assertRaisesRegex(helper.RescueError, "cryptsetup failed"):
                storage.unlock("/dev/sda3", b"wrong")
            self.assertIsNone(storage.data)
        self.assertFalse((self.runtime / "storage.json").exists())
        self.assertFalse(any(argv[:2] == ["cryptsetup", "close"] for argv, _ in self.d.calls))

    def test_close_refuses_mapper_with_changed_ownership(self):
        with self.storage() as storage:
            storage.unlock("/dev/sda3", b"first")
            self.d.bad_mapper = True
            with self.assertRaisesRegex(helper.RescueError, "ownership changed"):
                storage.close()
        self.assertFalse(any(argv[:2] == ["cryptsetup", "close"] for argv, _ in self.d.calls))
        # The helper never adopts or closes a mapper whose DM_UUID changed.
        self.assertTrue(self.d.mappers)

    def test_enable_repair_closes_readonly_then_reopens_with_second_secret(self):
        first, second = b"first-pass", b"second-pass"
        with self.storage() as storage:
            storage.unlock("/dev/sda3", first)
            storage.enable_repair(second, True)
            self.assertEqual(storage.data["mode"], "readwrite")
        opens = [(argv, kw) for argv, kw in self.d.calls if argv[:2] == ["cryptsetup", "open"]]
        self.assertEqual([kw["input"] for _, kw in opens], [first, first, second, second])
        self.assertEqual(opens[2][0][2], "--disable-external-tokens")
        self.assertNotIn("--readonly", opens[2][0])
        close_at = [index for index, (argv, _) in enumerate(self.d.calls) if argv[:2] == ["cryptsetup", "close"]]
        self.assertEqual(len(close_at), 2)
        second_open_at = [index for index, (argv, _) in enumerate(self.d.calls) if argv[:2] == ["cryptsetup", "open"]][2]
        self.assertLess(max(close_at), second_open_at)
        unmounts = [argv for argv, _ in self.d.calls if argv[:2] == ["umount", "--"]]
        self.assertEqual(unmounts[0][-1], str(self.runtime / "target" / "efi"))
        self.assertEqual(unmounts[1][-1], str(self.runtime / "target"))
        self.assertEqual(self.d.mount["options"], "rw")
        self.assertEqual(self.d.esp_mount["options"], "rw")
        self.assertTrue(self.runtime.joinpath("storage.json").exists())

    def test_close_is_idempotent_and_does_not_remove_external_mounts(self):
        with self.storage() as storage:
            storage.unlock("/dev/sda3", b"first")
            storage.close()
            storage.close()
        self.assertFalse(self.runtime.joinpath("storage.json").exists())
        self.assertFalse(self.d.mappers)
        self.assertIsNone(self.d.mount)


if __name__ == "__main__":
    unittest.main()
