#!/usr/bin/env python3
"""Exercise real gate dispatch and error reporting without mounts or chroots."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class GateErrors(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="gate-errors-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TEST_LOG=str(self.calls))
        self.gate = SOURCE / "link/local-package-gate"
        text = self.gate.read_text()
        start = text.index("        run_logged() {")
        self.script = self.root / "gate-logger"
        self.script.write_text('set -euo pipefail\nname=labwc-locachy\nresult=$1\nshift\n' +
                               text[start:text.index('\n        }', start) + len('\n        }')] +
                               '\nrun_logged host-clients-build.log "$@"\n')

    def command(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/bash\nset -eu\n" + body + "\n")
        path.chmod(0o755)

    def test_default_gate_is_update_smoke_and_regression_is_explicit(self):
        # Use a normal scratch directory for the regression's cgroup leaf.
        # unshare is a double; no writes take place in the host hierarchy.
        gate = self.root / 'local-package-gate'
        gate.write_text(self.gate.read_text().replace(
            '/sys/fs/cgroup${parent%/}/locachy-regression.XXXXXXXX',
            str(self.root / 'locachy-regression.XXXXXXXX')))
        self.env['TEST_ROOT'] = str(self.root)
        self.command("mktemp", 'd=$(/usr/bin/mktemp "$@"); case "$d" in *locachy-regression.*) : >"$d/cgroup.procs" ;; esac; printf "%s\\n" "$d"')
        self.command("unshare", 'if [ "$1" = --cgroup ]; then '
                     '    grep -qx 0 "$TEST_ROOT"/locachy-regression.*/cgroup.procs; '
                     '    printf "entered private cgroup\\n" >>"$TEST_LOG"; '
                     '    rm "$TEST_ROOT"/locachy-regression.*/cgroup.procs; '
                     'fi; while [ "$1" != bash ]; do shift; done; exec "$@"')
        for tool in ("install", "mount"):
            self.command(tool, 'printf "%s %s\\n" "${0##*/}" "$*" >>"$TEST_LOG"')
        self.command("chroot", 'shift; while [ "$1" != bash ]; do shift; done; shift; '
                               'printf "test %s\\n" "${1##*/}" >>"$TEST_LOG"; '
                               'if [ "$1" = -euc ]; then exit 0; fi; '
                               'printf "args %s\\n" "${*:2}" >>"$TEST_LOG"')
        for name in ("labwc-locachy", "libmupdf", "libpaper", "libinput", "libinput-tools"):
            for mode in ([], ["--regression"]):
                with self.subTest(name=name, mode=mode):
                    self.calls.write_text("")
                    result = subprocess.run(["bash", str(gate), name, str(self.root), str(SOURCE),
                                             str(self.root), *mode], env=self.env, capture_output=True,
                                            text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    calls = self.calls.read_text().splitlines()
                    cgroup = [line for line in calls if line.startswith("mount -t cgroup2 ")]
                    self.assertFalse(list(self.root.glob('locachy-regression.*')))
                    if name == "labwc-locachy" and mode:
                        self.assertEqual(calls[0], "mount -t cgroup2 -o nosuid,noexec,nodev cgroup2 /sys/fs/cgroup")
                        self.assertEqual(calls[1], "entered private cgroup")
                        self.assertEqual(calls[-1], "test -euc")
                        self.assertEqual(len(cgroup), 2)
                    else:
                        self.assertNotIn("entered private cgroup", calls)
                        self.assertIn(f"starting {'regression' if mode else 'update-smoke'}", result.stdout)
                        flag = " --update-smoke" if not mode and name in ("libmupdf", "libpaper") else ""
                        self.assertEqual(calls[-2:], [f"test test-{name}.sh",
                                                      "args /run/locachy-gate-results" + flag])
                        self.assertFalse(cgroup)
        self.calls.unlink()
        result = subprocess.run(["bash", str(self.gate), "labwc-locachy", str(self.root), str(SOURCE),
                                 str(self.root), "--unknown-mode"], env=self.env, capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.calls.exists())

    def test_failed_installed_library_smoke_leaves_no_passed_gate(self):
        # Run the actual lifecycle smoke with a package query and compiler double.
        # The compiled probe fails; the script must not publish its gate.json.
        self.command("pacman", 'printf "libpaper 1.0-1\\n"')
        self.command("gcc", 'while [ "$1" != -o ]; do shift; done; '
                            'printf "#!/bin/sh\\nexit 7\\n" >"$2"; chmod +x "$2"')
        proof = self.root / "gate.json"
        proof.write_text('{"id":"libpaper-lifecycle-smoke-v1","passed":true}')
        result = subprocess.run(["bash", str(SOURCE / "tools/test-libpaper.sh"), str(self.root),
                                 "--update-smoke"], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertFalse(proof.exists())
        self.assertNotIn("PASS", result.stdout + result.stderr)

    def test_compiler_error_is_visible_and_exit_status_unchanged(self):
        result = subprocess.run(["bash", str(self.script), str(self.root), "sh", "-c",
                                 'printf "settings.c: error: format-truncation\\n" >&2; exit 7'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 7)
        self.assertRegex(result.stderr, r"labwc-locachy: host-clients-build failed \(status 7, \d+ s\)")
        self.assertIn("settings.c: error: format-truncation", result.stderr)
        self.assertIn(str(self.root / "host-clients-build.log"), result.stderr)
        self.assertEqual((self.root / "host-clients-build.log").read_text(), "settings.c: error: format-truncation\n")

    def test_contract_mismatch_names_both_ids_and_the_source(self):
        text = (SOURCE / "link/locachy-build").read_text()
        start = text.index("    expected = data['recipes'][name].get('candidate', {}).get('gate')")
        end = text.index("    shutil.copyfile(", start)
        code = "for name in builds:\n" + text[start:end]
        with self.assertRaises(RuntimeError) as failed:
            exec(code, {"builds": {"labwc-locachy": {}},
                        "data": {"recipes": {"labwc-locachy": {"candidate": {
                            "gate": "labwc-locachy-update-smoke-v1"}}}, "test_source": {"sha256": "a" * 64}},
                        "gate": {"name": "labwc-locachy", "id": "labwc-clipboard-corner-drop-v4", "passed": True}})
        self.assertIn("expected id='labwc-locachy-update-smoke-v1'", str(failed.exception))
        self.assertIn("id='labwc-clipboard-corner-drop-v4'", str(failed.exception))
        self.assertIn("trusted source " + "a" * 64, str(failed.exception))

    def test_success_keeps_output_in_its_own_log(self):
        result = subprocess.run(["bash", str(self.script), str(self.root), "sh", "-c", 'printf "build passed\\n"'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("labwc-locachy: starting host-clients-build", result.stderr)
        self.assertRegex(result.stderr, r"labwc-locachy: host-clients-build done \(\d+ s\)")
        self.assertNotIn("build passed", result.stderr)
        self.assertEqual((self.root / "host-clients-build.log").read_text(), "build passed\n")


if __name__ == "__main__":
    unittest.main()
