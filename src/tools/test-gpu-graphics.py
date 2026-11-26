#!/usr/bin/env python3
"""Exercise GPU graphics firmware controls with temporary sysfs fixtures only.

Run with python3 -B src/tools/test-gpu-graphics.py from the repository root.
"""
import contextlib
import errno
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("gpu_graphics", str(SOURCE / "link/locachy-gpu-graphics"))
spec = importlib.util.spec_from_loader(loader.name, loader)
gpu = importlib.util.module_from_spec(spec)
loader.exec_module(gpu)


class GraphicsTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="test-gpu-graphics-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.firmware_class = self.root / "sys/class/firmware-attributes"
        self.drm_class = self.root / "sys/class/drm"
        self.graphics = gpu.Graphics(self.firmware_class, self.drm_class)
        self.attrs = self.root / "sys/devices/virtual/firmware-attributes/asus-armoury/attributes"
        self.mux = self.attrs / "gpu_mux_mode/current_value"
        self.dgpu = self.attrs / "dgpu_disable/current_value"
        self.egpu = self.attrs / "egpu_enable/current_value"
        self.power = self.attrs / "dgpu_power_state/current_value"
        self.pending = self.attrs / "pending_reboot"
        self.firmware_class.mkdir(parents=True)
        self.drm_class.mkdir(parents=True)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(gpu, "Graphics", return_value=self.graphics).start()
        mock.patch.object(gpu, "FIRMWARE_LOCK", self.root / "run/firmware.lock").start()
        self.jobs = mock.patch.object(gpu, "jobs_helper").start().return_value
        self.jobs.execution_mode.return_value = "job"
        self.uid = mock.patch.object(gpu.os, "geteuid", return_value=0).start()
        self.write = mock.patch.object(gpu, "write_value", side_effect=self.firmware_write).start()

    @staticmethod
    def put(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value + "\n", encoding="ascii")

    def setup_firmware(self, mux="1", dgpu="0", pending="0"):
        (self.firmware_class / "asus-armoury").symlink_to(self.attrs.parent, target_is_directory=True)
        for path, value in ((self.mux, mux), (self.dgpu, dgpu), (self.pending, pending)):
            if value is not None:
                self.put(path, value)

    def firmware_write(self, path, value):
        target = path.resolve(strict=True)
        self.assertIn(target, (self.mux, self.dgpu))
        self.put(target, value)
        if target == self.mux:
            self.put(self.pending, "1")

    def panel(self, index, *, vendor="0x8086", device_id="0x9b41", pci="0000:00:02.0",
              pci_class="0x030000", connection="eDP", status="connected"):
        device = self.root / "sys/devices/pci0000:00" / pci
        card = f"card{index}"
        drm = device / "drm" / card
        connector = drm / f"{card}-{connection}-1"
        self.put(device / "vendor", vendor)
        self.put(device / "device", device_id)
        self.put(device / "class", pci_class)
        self.put(connector / "status", status)
        (drm / "device").symlink_to(device, target_is_directory=True)
        (self.drm_class / card).symlink_to(drm, target_is_directory=True)
        (self.drm_class / connector.name).symlink_to(connector, target_is_directory=True)
        return connector / "status"

    def cli(self, *args):
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            rc = gpu.main(args)
        return rc, out.getvalue(), err.getvalue()

    def test_status_uses_only_exact_asus_driver_and_stable_rows(self):
        for foreign in ("asus-armoury-0", "asus_armoury", "hp-bioscfg"):
            fake = self.firmware_class / foreign / "attributes"
            self.put(fake / "gpu_mux_mode/current_value", "1")
            self.put(fake / "dgpu_disable/current_value", "0")
            self.put(fake / "pending_reboot", "0")
        self.uid.return_value = 1000
        self.assertEqual(self.cli("status", "--tsv"), (0, "mux\tunsupported\ndgpu\tunsupported\nreboot\tunknown\n", ""))
        self.jobs.execution_mode.assert_not_called()
        self.write.assert_not_called()

    def test_status_distinguishes_absent_inaccessible_and_invalid(self):
        self.setup_firmware(mux=None, dgpu="unsupported", pending="typo")
        self.assertEqual(self.cli("status"), (0, "mux\tunsupported\ndgpu\tunknown\nreboot\tunknown\n", ""))
        self.put(self.mux, "1")
        original_open = Path.open

        def inaccessible(path, *args, **kwargs):
            if path.resolve() == self.mux:
                raise PermissionError(errno.EACCES, "Permission denied")
            return original_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", inaccessible):
            self.assertIn("mux\tunknown\n", self.cli("status", "--tsv")[1])
        self.dgpu.write_bytes(b"\xff\n")
        self.put(self.pending, "1")
        self.assertEqual(json.loads(self.cli("status", "--json")[1]),
                         {"mux": "hybrid", "dgpu": "unknown", "reboot": "1"})

    def test_non_root_mutation_cannot_submit_or_write(self):
        self.uid.return_value = 1000
        rc, out, err = self.cli("set-mux", "hybrid")
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("requires root", err)
        self.jobs.execution_mode.assert_not_called()
        self.write.assert_not_called()

    def test_cli_rejects_other_values_and_options(self):
        for args in (("set-mux", "0"), ("set-dgpu", "disabled"), ("set-mux", "hybrid", "--sysfs", "/tmp")):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as end:
                gpu.main(args)
            self.assertEqual(end.exception.code, 2)
        self.jobs.execution_mode.assert_not_called()

    def test_online_route_submits_only_fixed_jobs_and_no_inline_fallback(self):
        self.jobs.execution_mode.return_value = "queue"
        self.jobs.submit.return_value = 7
        self.assertEqual(self.cli("set-mux", "discrete")[0], 7)
        self.jobs.submit.assert_called_once_with("gpu-graphics-mux", ["discrete"], wait=True)
        self.jobs.submit.reset_mock(side_effect=True)
        self.jobs.submit.side_effect = OSError(errno.ECONNREFUSED, "Connection refused")
        rc, _, err = self.cli("set-dgpu", "off")
        self.assertEqual(rc, 1)
        self.assertIn("Connection refused", err)
        self.jobs.submit.assert_called_once_with("gpu-graphics-dgpu", ["off"], wait=True)
        self.write.assert_not_called()
        self.assertFalse(gpu.FIRMWARE_LOCK.exists())

    def test_identical_requests_are_noops_even_while_pending(self):
        self.setup_firmware(pending="1")
        self.assertEqual(self.cli("set-mux", "hybrid")[0], 0)
        self.assertEqual(self.cli("set-dgpu", "on")[0], 0)
        self.write.assert_not_called()
        self.assertEqual(self.pending.read_text(), "1\n")

    def test_pending_or_unknown_reboot_blocks_changes(self):
        self.setup_firmware()
        for value in ("1", None, "2"):
            if value is None:
                self.pending.unlink()
            else:
                self.put(self.pending, value)
            for args in (("set-mux", "discrete"), ("set-dgpu", "off")):
                with self.subTest(value=value, args=args):
                    rc, _, err = self.cli(*args)
                    self.assertEqual(rc, 1)
                    self.assertIn("pending reboot", err)
        self.write.assert_not_called()

    def test_mux_writes_only_its_own_control_and_requires_reboot(self):
        self.setup_firmware()
        self.jobs.execution_mode.return_value = "offline"
        for wanted, raw in (("discrete", "0"), ("hybrid", "1")):
            with self.subTest(wanted=wanted):
                rc, out, err = self.cli("set-mux", wanted)
                self.assertEqual((rc, err), (0, ""))
                self.assertIn("reboot required", out)
                self.write.assert_called_once_with(self.graphics.current_path("gpu_mux_mode"), raw)
                self.write.reset_mock()
                self.assertEqual(self.mux.read_text(), raw + "\n")
                self.assertEqual(self.dgpu.read_text(), "0\n")
                self.assertEqual(self.cli("status", "--tsv")[1], f"mux\t{wanted}\ndgpu\ton\nreboot\t1\n")
                self.put(self.pending, "0")  # Simulate reboot for the next request.
        self.jobs.submit.assert_not_called()
        self.assertTrue(gpu.FIRMWARE_LOCK.is_file())

    def test_discrete_mux_rejects_disabled_or_unreadable_dgpu(self):
        self.setup_firmware()
        for value, error in (("1", "dGPU is disabled"), ("3", "cannot confirm the dGPU is enabled")):
            with self.subTest(value=value):
                self.put(self.dgpu, value)
                rc, _, err = self.cli("set-mux", "discrete")
                self.assertEqual(rc, 1)
                self.assertIn(error, err)
        self.write.assert_not_called()

    def test_egpu_interlocks_respect_enable_not_physical_presence(self):
        self.setup_firmware()
        for value in ("1", "2", "3", "4", "absent"):
            self.put(self.egpu, value)
            for args in (("set-mux", "discrete"), ("set-dgpu", "off")):
                with self.subTest(value=value, args=args):
                    rc, _, err = self.cli(*args)
                    self.assertEqual(rc, 1)
                    self.assertIn("eGPU", err)
        self.write.assert_not_called()
        self.put(self.attrs / "egpu_connected/current_value", "1")
        self.put(self.egpu, "0")
        self.assertEqual(self.cli("set-mux", "discrete")[0], 0)

    def test_disable_requires_confirmed_hybrid_mode(self):
        self.setup_firmware(mux=None)
        self.panel(1)
        for value in (None, "0", "2"):
            if value is not None:
                self.put(self.mux, value)
            with self.subTest(value=value):
                rc, _, err = self.cli("set-dgpu", "off")
                self.assertEqual(rc, 1)
                self.assertIn("confirmed hybrid", err)
        self.write.assert_not_called()

    def test_disable_does_not_accept_external_discrete_or_disconnected_panel(self):
        self.setup_firmware()
        self.panel(0, connection="HDMI-A")
        self.panel(1, status="disconnected", vendor="0x1002", device_id="0x15bf", pci="0000:05:00.0")
        self.panel(2, vendor="0x10de", device_id="0x2820", pci="0000:01:00.0")
        self.panel(3, vendor="0x1002", device_id="0x73df", pci="0000:02:00.0")
        self.panel(4, device_id="0x5690", pci="0000:00:01.0/0000:03:00.0")
        self.panel(5, vendor="0x1002", device_id="0x1638", pci="0000:04:00.1", pci_class="0x040300")
        rc, _, err = self.cli("set-dgpu", "off")
        self.assertEqual(rc, 1)
        self.assertIn("connected internal display on the integrated GPU", err)
        self.write.assert_not_called()

    def test_dgpu_on_and_off_write_only_dgpu_with_confirmed_integrated_panel(self):
        self.setup_firmware(mux="1")
        intel = self.panel(5)
        self.put(self.power, "0")
        for wanted, raw in (("off", "1"), ("on", "0")):
            with self.subTest(wanted=wanted):
                self.assertEqual(self.cli("set-dgpu", wanted), (0, f"dgpu configured as {wanted}\n", ""))
                self.write.assert_called_once_with(self.graphics.current_path("dgpu_disable"), raw)
                self.write.reset_mock()
                self.assertEqual(self.mux.read_text(), "1\n")
                self.assertEqual(self.pending.read_text(), "0\n")
        self.put(intel, "disconnected")
        self.panel(9, vendor="0x1002", device_id="0x15bf", pci="0000:01:00.0", connection="LVDS")
        self.assertEqual(self.cli("set-dgpu", "off")[0], 0)
        self.assertEqual(self.dgpu.read_text(), "1\n")

    def test_disable_respects_optional_dgpu_power_state(self):
        self.setup_firmware()
        self.panel(2)
        for value, error in (("1", "dGPU is active"), ("2", "cannot confirm the dGPU is idle")):
            self.put(self.power, value)
            rc, _, err = self.cli("set-dgpu", "off")
            self.assertEqual(rc, 1)
            self.assertIn(error, err)
        self.write.assert_not_called()

    def test_kernel_rejection_or_failed_readback_is_not_success(self):
        self.setup_firmware()
        self.write.side_effect = OSError(errno.EBUSY, "Device or resource busy")
        rc, out, err = self.cli("set-mux", "discrete")
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("Device or resource busy", err)
        self.write.assert_called_once()
        self.write.reset_mock(side_effect=True)
        self.panel(4)
        for args in (("set-mux", "discrete"), ("set-dgpu", "off")):
            rc, out, err = self.cli(*args)
            self.assertEqual((rc, out), (1, ""))
            self.assertIn("cannot verify", err)
        self.assertEqual(self.write.call_count, 2)

    def test_mux_acceptance_needs_pending_reboot_readback(self):
        self.setup_firmware()
        self.write.side_effect = self.put
        rc, out, err = self.cli("set-mux", "discrete")
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("pending reboot was not confirmed", err)
        self.assertEqual(self.mux.read_text(), "0\n")
        self.assertEqual(self.dgpu.read_text(), "0\n")
        self.write.assert_called_once()  # No silent retry, rollback or second control.


if __name__ == "__main__":
    unittest.main()
