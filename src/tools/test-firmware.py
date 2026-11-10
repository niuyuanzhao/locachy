#!/usr/bin/env python3
"""Unit and contract tests for locachy-firmware (fwupd, BIOS settings, flashrom).
  python3 -B tools/test-firmware.py [SCRATCH_PARENT]
"""
import contextlib
import hashlib
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
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("locachy_firmware", str(SOURCE / "link/locachy-firmware"))
spec = importlib.util.spec_from_loader(loader.name, loader)
fw = importlib.util.module_from_spec(spec)
loader.exec_module(fw)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class FirmwareHelperTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="test-fw-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        # Offline unless a test says otherwise; the fixed paths, injected here.
        for name, value in (("route_mutation", mock.Mock(return_value=(None, "offline"))),
                            ("FIRMWARE_LOCK_PATH", str(self.root / "firmware.lock")),
                            ("SNAPSHOT_DIR", str(self.root / "snapshots")),
                            ("JOB_DIR", str(self.root / "jobs")),
                            ("SPOOL_OWNER", os.getuid())):
            p = mock.patch.object(fw, name, value)
            p.start()
            self.addCleanup(p.stop)

    def make_script(self, name: str, body: str) -> Path:
        p = self.root / name
        p.write_text("#!/usr/bin/env python3\n" + body, encoding="utf-8")
        p.chmod(p.stat().st_mode | stat.S_IXUSR)
        return p

    def test_fwupd_devices_and_releases(self):
        mock_fwupd = self.make_script("fwupdmgr", """
import json, sys
args = sys.argv[1:]
if "get-devices" in args:
    print(json.dumps({"Devices": [{
        "DeviceId": "dev-sys-01",
        "Name": "System Firmware",
        "Vendor": "LENOVO",
        "Version": "0.1.27",
        "Flags": ["updatable", "can-verify"],
        "Children": [{
            "DeviceId": "dev-nvme-02",
            "Name": "NVMe SSD",
            "Vendor": "Samsung",
            "Version": "2B2QEXM7",
            "Flags": ["updatable", "locked"]
        }]
    }]}))
elif "get-updates" in args:
    print(json.dumps({"Devices": [{
        "DeviceId": "dev-sys-01",
        "Releases": [{"Version": "0.1.29"}]
    }]}))
elif "get-releases" in args:
    print(json.dumps({"Releases": [
        {"Version": "0.1.29", "RemoteId": "lvfs", "Locations": ["https://example.org/029.cab"], "Description": "<p>Fix S3 resume</p>"},
        {"Version": "0.1.27", "RemoteId": "lvfs", "Locations": ["https://example.org/027.cab"], "Description": "<p>Current</p>"},
        {"Version": "0.1.25", "RemoteId": "lvfs", "Locations": ["https://example.org/025.cab"], "Description": "<p>Older</p>"}
    ]}))
""")
        with mock.patch.object(fw, "FWUPD_BIN_PATH", str(mock_fwupd)):
            buf = io.StringIO()
            with mock.patch("sys.stdout", buf):
                self.assertEqual(fw.cmd_fwupd_devices(None), 0)
            lines = buf.getvalue().strip().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertIn("dev-sys-01\tSystem Firmware\tLENOVO\t0.1.27\t0.1.29\tupdate:0.1.29,verifiable", lines[0])
            self.assertIn("dev-nvme-02\tNVMe SSD\tSamsung\t2B2QEXM7\t\tupdatable,locked", lines[1])

            rbuf = io.StringIO()
            rargs = type("A", (), {"device_id": "dev-sys-01", "current_version": "0.1.27"})()
            with mock.patch("sys.stdout", rbuf):
                self.assertEqual(fw.cmd_fwupd_releases(rargs), 0)
            rlines = rbuf.getvalue().strip().splitlines()
            self.assertEqual([l.split("\t")[1] for l in rlines], ["upgrade", "reinstall", "downgrade"])
            self.assertIn("Fix S3 resume", rlines[0])

    def test_network_actions_run_in_locachy_net(self):
        """A system job runs in the root namespace (lo alone): fwupd's
        downloads go into locachy-net, with the rest of its action; what
        needs no network (and the daemon, the devices) stays as it is."""
        calls = self.root / "calls"
        stand_in = self.make_script("fwupdmgr", f"""
import sys
open({str(calls)!r}, "a").write(" ".join(sys.argv[1:]) + "\\n")
print("{{}}")
""")
        netns = self.root / "netns"  # stands in for /run/netns/locachy-net: another namespace
        netns.write_text("")

        class Exec(Exception):
            pass

        def execvp(file, argv):
            raise Exec(file, argv)

        def action(name):
            args = type("A", (), {"action": name, "target": "dev-sys-01", "extra": "https://example.org/029.cab"})()
            with mock.patch("sys.stdout", io.StringIO()):
                return fw.cmd_fwupd_action(args)

        argv = ["/usr/local/bin/locachy-firmware", "fwupd-action", "refresh"]
        with mock.patch.object(fw, "FWUPD_BIN_PATH", str(stand_in)), \
                mock.patch.object(fw.os, "execvp", execvp), mock.patch.object(fw.sys, "argv", argv):
            with mock.patch.object(fw, "NETNS", str(netns)):
                for name in ("refresh", "update", "install-release"):
                    with self.assertRaises(Exec) as e:
                        action(name)
                    self.assertEqual(e.exception.args, ("ip", ["ip", "netns", "exec", "locachy-net", sys.executable,
                                                               "-I", os.path.abspath(fw.__file__)] + argv[1:]), name)
                self.assertFalse(calls.exists(), "fwupdmgr ran outside locachy-net")
                for name in ("verify", "unlock"):
                    self.assertEqual(action(name), 0)
                self.assertEqual(calls.read_text(), "verify dev-sys-01\nunlock dev-sys-01 --assume-yes\n")
            calls.unlink()
            # In locachy-net already (the re-executed one): fwupdmgr itself.
            with mock.patch.object(fw, "NETNS", "/proc/self/ns/net"):
                self.assertEqual(action("refresh"), 0)
            # No locachy-net (no network service yet): as before, nothing else.
            with mock.patch.object(fw, "NETNS", str(self.root / "missing")):
                self.assertEqual(action("update"), 0)
            self.assertEqual(calls.read_text(), "get-remotes --json\nrefresh --force\n"
                                                "update --assume-yes --no-reboot-check\n")

    def test_bios_attributes_read_and_write(self):
        attrs = self.root / "attributes"
        sc = attrs / "SecurityChip"
        sc.mkdir(parents=True)
        (sc / "current_value").write_text("Inactive\n")
        (sc / "possible_values").write_text("Active;Inactive;Disable\n")
        (sc / "type").write_text("enumeration\n")
        (sc / "display_name").write_text("Security Chip (TPM)\n")
        (attrs / "save_settings").write_text("single\n")
        (attrs / "pending_reboot").write_text("1\n")

        with mock.patch.object(fw, "BIOS_ATTRS_OVERRIDE", str(attrs)):
            buf = io.StringIO()
            with mock.patch("sys.stdout", buf):
                self.assertEqual(fw.cmd_bios_list(None), 0)
            self.assertEqual(
                buf.getvalue().strip(),
                "SecurityChip\tInactive\tActive;Inactive;Disable\tenumeration\tSecurity Chip (TPM)"
            )
            out = io.StringIO()
            with mock.patch("sys.stdout", out):
                rc = fw.cmd_bios_set(type("A", (), {"name": "SecurityChip", "value": "Active"})())
            self.assertEqual(rc, 0)
            self.assertEqual((sc / "current_value").read_text().strip(), "Active")
            self.assertIn("next reboot", out.getvalue())

    def test_spi_programmers_probe_3x_backup_and_write_guards(self):
        usb = self.root / "usb/1-2"
        usb.mkdir(parents=True)
        (usb / "idVendor").write_text("1a86\n")
        (usb / "idProduct").write_text("5512\n")

        state_file = self.root / "flaky"
        mock_fr = self.make_script("flashrom", f"""
import os, sys
from pathlib import Path
args = sys.argv[1:]
if "-r" in args:
    out = Path(args[args.index("-r") + 1])
    flaky = Path({str(state_file)!r})
    if flaky.exists():
        n = int(flaky.read_text() or "0") + 1
        flaky.write_text(str(n))
        out.write_bytes((b"A" * 4095) + bytes([n]))
    else:
        out.write_bytes(b"CONSISTENT_ROM" * 512)
    sys.exit(0)
if "-w" in args:
    sys.exit(0)
print('FREG0: Flash Descriptor region (0x00000000-0x00000fff) is read-only.')
print('FREG1: BIOS region (0x00500000-0x00bfffff) is read-write.')
print('FREG2: Management Engine region (0x00003000-0x004fffff) is locked.')
print('FREG3: Gigabit Ethernet region (0x00001000-0x00002fff) is read-write.')
print('Found Winbond flash chip "W25Q64.V" (8192 kB, SPI) on ch341a_spi.')
""")
        ac_file = self.root / "ac_online"
        ac_file.write_text("0\n")
        seams = {"FLASHROM_BIN_PATH": str(mock_fr), "USB_SYSFS_DIR": str(self.root / "usb"),
                 "ITCO_GUARD": False, "AC_ONLINE_FILE": str(ac_file)}
        with contextlib.ExitStack() as stack:
            for name, value in seams.items():
                stack.enter_context(mock.patch.object(fw, name, value))
            pbuf = io.StringIO()
            with mock.patch("sys.stdout", pbuf):
                fw.cmd_spi_programmers(None)
            self.assertIn("ch341a_spi\t1\tWCH CH341A", pbuf.getvalue())

            prbuf = io.StringIO()
            with mock.patch("sys.stdout", prbuf):
                rc = fw.cmd_spi_probe(type("A", (), {"programmer": "ch341a_spi", "chip": ""})())
            self.assertEqual(rc, 0)
            self.assertIn("CHIP\tW25Q64.V\t8388608\tfd,bios,gbe", prbuf.getvalue())

            # 1) Flaky reads MUST fail and leave zero backup files behind
            bdir = self.root / "backups"
            state_file.write_text("0")
            rc = fw.cmd_spi_backup(type("A", (), {
                "programmer": "ch341a_spi", "out_dir": str(bdir), "chip": "", "regions": ""
            })())
            self.assertEqual(rc, 2)
            self.assertEqual(list(bdir.glob("*.bin")), [])

            # 2) Consistent 3x reads save verified .bin and .sha256
            state_file.unlink()
            rc = fw.cmd_spi_backup(type("A", (), {
                "programmer": "ch341a_spi", "out_dir": str(bdir), "chip": "", "regions": "fd,gbe,bios"
            })())
            self.assertEqual(rc, 0)
            bins = list(bdir.glob("*.bin"))
            self.assertEqual(len(bins), 1)
            expect_sha = hashlib.sha256(b"CONSISTENT_ROM" * 512).hexdigest()
            self.assertIn(expect_sha, Path(str(bins[0]) + ".sha256").read_text())

            # 3) Write guards: offline AC rejected on internal; wrong size rejected
            img = bins[0]
            self.assertEqual(fw.cmd_spi_write(type("A", (), {
                "programmer": "internal", "image": str(img), "chip": "", "regions": "", "expected_size": 0
            })()), 3)
            ac_file.write_text("1\n")
            self.assertEqual(fw.cmd_spi_write(type("A", (), {
                "programmer": "ch341a_spi", "image": str(img), "chip": "", "regions": "", "expected_size": 8388608
            })()), 4)
            self.assertEqual(fw.cmd_spi_write(type("A", (), {
                "programmer": "ch341a_spi", "image": str(img), "chip": "", "regions": "",
                "expected_size": img.stat().st_size
            })()), 0)
    def test_spi_write_consumes_a_private_copy_not_the_path_it_was_given(self):
        """A writer that replaces the given image while the request runs cannot
        change what is flashed: flashrom reads the job's own copy."""
        original = self.root / "image.bin"
        original.write_bytes(b"ORIGINAL" * 1024)
        seen = self.root / "seen.bin"
        mock_fr = self.make_script("flashrom", f"""
import sys
from pathlib import Path
args = sys.argv[1:]
target = Path(args[args.index("-w") + 1])
Path({str(original)!r}).write_bytes(b"REPLACED" * 1024)
Path({str(seen)!r}).write_bytes(target.read_bytes())
sys.exit(0)
""")
        seams = {"FLASHROM_BIN_PATH": str(mock_fr), "ITCO_GUARD": False}
        with contextlib.ExitStack() as stack:
            for name, value in seams.items():
                stack.enter_context(mock.patch.object(fw, name, value))
            with mock.patch("sys.stdout", io.StringIO()):
                rc = fw.cmd_spi_write(type("A", (), {"programmer": "ch341a_spi", "image": str(original), "chip": "",
                                                      "regions": "", "expected_size": 0})())
        self.assertEqual(rc, 0)
        self.assertEqual(seen.read_bytes(), b"ORIGINAL" * 1024)
        self.assertEqual(original.read_bytes(), b"REPLACED" * 1024)
        self.assertEqual(list((self.root / "snapshots").glob("firmware-input-*")), [])  # removed after use

    def test_spi_backup_options_pass_to_the_job_in_their_checked_form(self):
        args = type("A", (), {"programmer": "ch341a_spi", "out_dir": "/var/x", "chip": "W25Q64", "regions": "bios"})()
        self.assertEqual(fw.spi_job_args(args, [args.programmer, args.out_dir]),
                         ["ch341a_spi", "/var/x", "--chip", "W25Q64", "--regions", "bios"])
        bare = type("A", (), {"programmer": "internal"})()
        self.assertEqual(fw.spi_job_args(bare, ["internal"]), ["internal"])

    def test_production_backends_take_no_lock_flasher_or_guard_from_the_environment(self):
        self.assertNotIn("os.environ", (SOURCE / "link/locachy-firmware").read_text())
        efi = (SOURCE / "link/locachy-efi").read_text()
        self.assertNotIn("LOCACHY_EFI_LOCK", efi)
        self.assertIn('EFI_LOCK_PATH = "/run/locachy/efi.lock"', efi)

    def job_write(self, image):
        """cmd_spi_write as job 7's runner runs it: its status, what flashrom read."""
        seen = self.root / "seen.bin"
        seen.unlink(missing_ok=True)
        mock_fr = self.make_script("flashrom", f"""
import sys
from pathlib import Path
args = sys.argv[1:]
Path({str(seen)!r}).write_bytes(Path(args[args.index("-w") + 1]).read_bytes())
""")
        with mock.patch.object(fw, "route_mutation", return_value=(None, "job")), \
                mock.patch.object(fw, "job_number", lambda: "7"), \
                mock.patch.object(fw, "FLASHROM_BIN_PATH", str(mock_fr)), mock.patch.object(fw, "ITCO_GUARD", False), \
                mock.patch("sys.stdout", io.StringIO()), mock.patch("sys.stderr", io.StringIO()):
            rc = fw.cmd_spi_write(type("A", (), {"programmer": "ch341a_spi", "image": str(image), "chip": "",
                                                  "regions": "", "expected_size": 0})())
        return rc, (seen.read_bytes() if seen.exists() else None)

    def accept(self, content, number="7", digest=None, size=None):
        """What the jobs service leaves when it accepts job NUMBER: its copy
        (JOB_DIR/NUMBER.input) and its record (JOB_DIR/NUMBER.state)."""
        jobs = self.root / "jobs"
        jobs.mkdir(exist_ok=True)
        copy = jobs / f"{number}.input"
        copy.write_bytes(content)
        copy.chmod(0o600)
        (jobs / f"{number}.state").write_text(
            "kind=spi-write\n"
            f"image_sha256={digest or hashlib.sha256(content).hexdigest()}\n"
            f"image_size={len(content) if size is None else size}\n")
        return copy

    def test_a_job_flashes_only_its_accepted_copy_with_the_recorded_digest_and_size(self):
        content = b"ACCEPTED" * 512
        copy = self.accept(content)
        self.assertEqual(self.job_write(copy), (0, content))
        # Refused, nothing flashed: another digest or size recorded, no record,
        # the same bytes elsewhere, another job's copy, a link to the copy, a
        # copy others may write.
        self.accept(content, digest="0" * 64)
        self.assertEqual(self.job_write(copy), (1, None))
        self.accept(content, size=1)
        self.assertEqual(self.job_write(copy), (1, None))
        self.accept(content)
        (self.root / "jobs/7.state").unlink()
        self.assertEqual(self.job_write(copy), (1, None))
        copy = self.accept(content)
        elsewhere = self.root / "image.bin"
        elsewhere.write_bytes(content)
        self.assertEqual(self.job_write(elsewhere), (1, None))
        other = self.accept(content, number="8")
        self.assertEqual(self.job_write(other), (1, None))
        link = self.root / "link.input"
        link.symlink_to(copy)
        self.assertEqual(self.job_write(link), (1, None))
        copy.chmod(0o666)
        self.assertEqual(self.job_write(copy), (1, None))


if __name__ == "__main__":
    unittest.main()
