#!/usr/bin/env python3
"""Unit and contract tests for locachy-firmware (fwupd, BIOS settings, flashrom)."""
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


class FirmwareHelperTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="test-fw-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

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
        with mock.patch.dict(os.environ, {"LOCACHY_FWUPD_BIN": str(mock_fwupd)}):
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

        with mock.patch.dict(os.environ, {"LOCACHY_BIOS_ATTRS_DIR": str(attrs)}):
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
        env = {
            "LOCACHY_FLASHROM_BIN": str(mock_fr),
            "LOCACHY_USB_SYSFS_DIR": str(self.root / "usb"),
            "LOCACHY_FLASHROM_NO_MODPROBE": "1",
            "LOCACHY_AC_ONLINE_FILE": str(ac_file),
        }
        with mock.patch.dict(os.environ, env):
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


if __name__ == "__main__":
    unittest.main()
