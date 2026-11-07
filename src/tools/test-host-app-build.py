#!/usr/bin/env python3
"""The real locachy-app build function, with only its package installer doubled.

No mounts, root, packages or network: the host-access branch must install and
register every declared package before recording a successful build.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]


def function(name):
    text = (SOURCE / "link/locachy-app").read_text()
    start = text.index(name + "() {")
    return text[start:text.index("\n}", start) + 2]


class HostBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="locachy-host-build-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.conf = self.root / "conf"
        self.conf.mkdir()
        self.state = self.root / "state"
        self.state.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.command = self.bin / "example-app"
        (self.conf / "example.conf").write_text(
            'host="1"\npackage="example-package"\nextras="helper-package"\n'
            f'command="{self.command}"\n')
        self.calls = self.root / "calls"
        installer = self.bin / "locachy-pkg"
        installer.write_text('#!/bin/sh\nset -eu\nprintf "%s\\n" "$@" >>"$TEST_CALLS"\n'
                             '[ "${TEST_FAIL:-0}" = 0 ] || exit 17\n'
                             'if [ "${TEST_NO_COMMAND:-0}" = 0 ]; then\n'
                             '    printf "#!/bin/sh\\nexit 0\\n" >"$TEST_COMMAND"\n'
                             '    chmod 755 "$TEST_COMMAND"\nfi\n')
        installer.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TEST_CALLS=str(self.calls), TEST_COMMAND=str(self.command))
        self.script = self.root / "build.sh"
        self.script.write_text(
            'set -euo pipefail\ndie() { printf "locachy-app: %s\\n" "$*" >&2; exit 1; }\n'
            + '\n'.join(function(n) for n in ("conf_value", "is_host", "saved_packages", "do_build"))
            + '\nCONT_CONF=$1\nCONT_BASE=$2\ndo_build example\n')

    @property
    def record(self):
        return self.state / "example/built"

    def build(self, **env):
        return subprocess.run(["bash", str(self.script), str(self.conf), str(self.state)],
                              env=dict(self.env, **env), text=True, capture_output=True, timeout=10)

    def test_store_build_installs_the_package_and_extras_then_records_success(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls.read_text().splitlines(), ["install", "example-package", "helper-package"])
        self.assertIn('packages="example-package helper-package"', self.record.read_text())
        self.assertIn("built: real-system packages installed", result.stdout)
        self.assertFalse(self.record.with_name("built.new").exists())

    def test_failed_install_has_no_success_marker_and_can_be_retried(self):
        self.record.parent.mkdir()
        self.record.write_text('packages="old-package"\n')
        result = self.build(TEST_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("package installation failed; not built", result.stderr)
        self.assertFalse(self.record.exists())
        self.assertFalse(self.command.exists())
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.record.exists())

    def test_successful_installer_without_the_command_is_not_a_successful_build(self):
        result = self.build(TEST_NO_COMMAND="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not installed after its packages; not built", result.stderr)
        self.assertFalse(self.record.exists())

    def test_an_existing_command_does_not_skip_registering_its_packages(self):
        self.command.write_text("#!/bin/sh\nexit 0\n")
        self.command.chmod(0o755)
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls.read_text().splitlines(), ["install", "example-package", "helper-package"])

    def test_no_packages_cannot_be_built(self):
        self.record.parent.mkdir()
        self.record.write_text('packages="old-package"\n')
        (self.conf / "example.conf").write_text(f'host="1"\ncommand="{self.command}"\n')
        result = self.build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no package", result.stderr)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.record.exists())


if __name__ == "__main__":
    # The interaction runner supplies its output directory to Python tests.
    import sys
    if len(sys.argv) == 2 and os.path.isdir(sys.argv[1]):
        sys.argv.pop()
    unittest.main()
