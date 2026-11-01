#!/usr/bin/env python3
"""Pure fixture checks for the real-image boot driver's input and evidence.
  python3 -B tools/test-root-boot-unit.py SCRATCH_PARENT
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("root_boot", Path(__file__).with_name("test-root-boot.py"))
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class RootBoot(unittest.TestCase):
    def test_fixture_password_and_terminal_command_are_keyboard_representable(self):
        for char in boot.PASSWORD + "echo boot-test-terminal-ok >/dev/kmsg":
            self.assertTrue(boot.keys_for(char))
        self.assertEqual(boot.keys_for("L"), ["shift", "l"])
        self.assertEqual(boot.keys_for(">"), ["shift", "dot"])
        with self.assertRaises(ValueError):
            boot.keys_for("中")

    def test_screenshot_needs_a_complete_rendered_ui(self):
        with tempfile.TemporaryDirectory(prefix="root-shot-", dir=SCRATCH) as temp:
            path = Path(temp) / "screen.ppm"
            path.write_bytes(b"P6\n320 200\n255\n" + b"\0" * 320 * 200 * 3)
            with self.assertRaisesRegex(RuntimeError, "no rendered UI"):
                boot.screenshot_pixels(path)
            path.write_bytes(b"P6\n320 200\n255\n" + (b"\0\0\0\x20\x30\x48\xff\xff\xff" * (320 * 200 // 3 + 1))[:320 * 200 * 3])
            evidence = boot.screenshot_pixels(path)
            self.assertEqual(evidence["width"], 320)
            self.assertGreater(evidence["non_background_pixels"], 1000)
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaisesRegex(RuntimeError, "Incomplete"):
                boot.screenshot_pixels(path)

    def test_boot_image_command_line_from_rc_conf(self):
        """locachy-boot's own lines: settings' Boot rows (rc.conf) each time."""
        script = (Path(__file__).resolve().parents[1] / "link/locachy-boot").read_text()
        block = script.split("rc=/etc/locachy/rc.conf\n", 1)[1].split('mkdir -p "$boot"', 1)[0]
        with tempfile.TemporaryDirectory(prefix="boot-cmdline-", dir=SCRATCH) as temp:
            rc = Path(temp) / "rc.conf"

            def cmdline(text):
                if text is None:
                    rc.unlink(missing_ok=True)
                else:
                    rc.write_text(text)
                run = subprocess.run(["sh", "-euc", 'rc="$1"\n' + block + 'printf "%s\\n" "$cmdline"', "sh", str(rc)],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                return run.stdout.strip(), run.stderr

            quiet = "quiet loglevel=0 vt.global_cursor_default=0 printk.devkmsg=on net.ifnames=0 intel_iommu=on"
            self.assertEqual(cmdline(None), (quiet, ""))
            self.assertEqual(cmdline('hibernate_after="30"\n')[0], quiet)
            self.assertEqual(cmdline('boot_verbose="1"\n')[0], "loglevel=7 printk.devkmsg=on net.ifnames=0 intel_iommu=on")
            self.assertEqual(cmdline('boot_cmdline="i915.enable_psr=0 mitigations=off"\n')[0],
                             quiet + " i915.enable_psr=0 mitigations=off")
            self.assertEqual(cmdline('boot_cmdline="a"\nboot_cmdline="b=1"\n')[0], quiet + " b=1")
            line, err = cmdline('boot_cmdline="x; rm -rf /"\n')
            self.assertEqual(line, quiet)
            self.assertIn("left out", err)
            self.assertEqual(cmdline('boot_cmdline="$(id)"\n')[0], quiet)

    def test_harness_boots_built_esp_without_host_network_or_disks(self):
        text = Path(__file__).with_name("test-root-boot.sh").read_text()
        self.assertNotIn(" -kernel ", text.split("timeout --signal=TERM", 1)[1])
        self.assertIn("::/EFI/BOOT/BOOTX64.EFI", text)
        self.assertIn("ukify build", text)
        self.assertIn("console=ttyS0", text)
        self.assertIn("-nic none", text)
        self.assertEqual(text.count("format=raw,snapshot=on"), 2)
        self.assertIn('test directory', text.lower())
        self.assertIn("exit 77", text)
        self.assertNotIn('kill -USR2 1', text)


if __name__ == "__main__":
    unittest.main()
