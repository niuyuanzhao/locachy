#!/usr/bin/env python3
"""Target-builder preflight/namespace/flag contracts with command doubles.
No real mount, package transaction, user mapping or block device is used.
  python3 -B tools/test-target-root.py SCRATCH_PARENT
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class TargetRoot(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="target-root-", dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.target = self.root / "target"
        self.target.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        self.text = (SOURCE / "make-root.sh").read_text()
        # Stop before any namespace or build side effect. This is the actual
        # production preflight, with only read-only system queries doubled.
        prefix = self.text.split('if [ -z "${MAKE_ROOT_NS:-}" ]; then', 1)[0]
        self.script = self.root / "preflight"
        self.script.write_text(prefix + "printf 'PREFLIGHT-PASS\\n'\n")
        self.command("id", "printf '0\\n'")
        self.command("realpath", 'python3 -c "import os,sys; print(os.path.realpath(sys.argv[-1]))" "$@"')
        self.command("mountpoint", 'test "${FIX_MOUNTED:-1}" = 1')
        self.command("findmnt", 'case "$*" in *FSTYPE*) printf "%s\\n" "${FIX_FS:-ext4}" ;; '
                              '*LABEL*) printf "%s\\n" "${FIX_LABEL:-locachy-root}" ;; *) exit 1 ;; esac')
        self.command("stat", 'case "$*" in *" /") printf "1\\n" ;; *) printf "%s\\n" "${FIX_DEVICE:-2}" ;; esac')
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"], CALLS=str(self.calls))

    def command(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body + "\n")
        path.chmod(0o755)

    def run_preflight(self, *extra, env=None):
        return subprocess.run(["bash", str(self.script), "--target-root", str(self.target),
                               "--work-dir", str(self.root / "work"), "--cache-dir", str(self.root / "cache"),
                               *extra, "x86-64"], env=dict(self.env, **(env or {})), text=True, capture_output=True)

    def test_empty_mounted_target_accepts_only_empty_mountpoint_and_lost_found(self):
        (self.target / "lost+found").mkdir()
        (self.target / "efi").mkdir()
        result = self.run_preflight()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PREFLIGHT-PASS", result.stdout)
        self.assertFalse((self.root / "work").exists())
        self.assertFalse((self.root / "cache").exists())
        self.assertEqual(sorted(p.name for p in self.target.iterdir()), ["efi", "lost+found"])

    def test_nonempty_target_or_unmasked_esp_is_rejected_without_changes(self):
        for directory, filename in (("etc", "os-release"), ("efi", "BOOTX64.EFI"), ("lost+found", "recovered")):
            entry = self.target / directory
            entry.mkdir()
            sentinel = entry / filename
            sentinel.write_text("untouched")
            result = self.run_preflight()
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_text(), "untouched")
            sentinel.unlink()
            entry.rmdir()

    def test_mount_filesystem_label_and_running_root_are_verified(self):
        for env, message in (({"FIX_MOUNTED": "0"}, "already be mounted"),
                             ({"FIX_FS": "btrfs"}, "must be ext4"),
                             ({"FIX_LABEL": "other"}, "labelled locachy-root"),
                             ({"FIX_DEVICE": "1"}, "running root")):
            result = self.run_preflight(env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)

    def test_symlinked_target_and_paths_inside_target_are_rejected(self):
        alias = self.root / "alias"
        alias.symlink_to(self.target, target_is_directory=True)
        command = ["bash", str(self.script), "--target-root", str(alias), "x86-64"]
        result = subprocess.run(command, env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("without symlinks", result.stderr)
        command = ["bash", str(self.script), "--target-root", str(self.target),
                   "--work-dir", str(self.target / "work"), "--cache-dir", str(self.root / "cache"), "x86-64"]
        result = subprocess.run(command, env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("reserved .locachy-installer", result.stderr)

    def test_reserved_target_scratch_keeps_builds_off_live_tmpfs(self):
        # Both omitted defaults and explicitly selected reserved directories
        # pass before any directory is created on the initially empty target.
        for extra in ([], ["--work-dir", str(self.target / ".locachy-installer"),
                           "--cache-dir", str(self.target / "var/cache/pacman/pkg")]):
            result = subprocess.run(["bash", str(self.script), "--target-root", str(self.target),
                                     *extra, "x86-64"], env=self.env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(list(self.target.iterdir()), [])
        result = subprocess.run(["bash", str(self.script), "--target-root", str(self.target),
                                 "--cache-dir", str(self.target / "cache"), "x86-64"],
                                env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("reserved var/cache/pacman/pkg", result.stderr)

    def test_existing_nonempty_work_is_never_reset_in_target_mode(self):
        work = self.root / "work"
        work.mkdir()
        sentinel = work / "previous.log"
        sentinel.write_text("keep")
        result = self.run_preflight()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("work directory must be empty", result.stderr)
        self.assertEqual(sentinel.read_text(), "keep")

    def test_root_namespace_has_no_subuid_or_systemd_requirement(self):
        block = self.text.split('if [ "$(id -u)" = 0 ]; then', 1)[1].split('    # The clipboard contract', 1)[0]
        self.assertIn('--mount --mount-proc --cgroup', block)
        self.assertIn('--mount --propagation private --mount-proc', block)
        self.assertIn('mount -t cgroup2 cgroup2 /sys/fs/cgroup', block)
        self.assertIn('echo 0 >"$group/cgroup.procs"', block)
        self.assertNotIn('echo $$', block)
        self.assertNotIn('--map-auto', block)
        self.assertNotIn('systemd-run', block)
        self.assertIn('mktemp -d "/sys/fs/cgroup$parent/locachy-build.XXXXXX"', block)
        self.assertIn('env MAKE_ROOT_NS=1', block)

    def test_target_does_not_archive_write_esp_or_remove_the_callers_mount(self):
        self.assertIn('"$here/make-root.sh"', self.text)
        self.assertIn('installer-esp "$root/efi"', self.text)
        self.assertIn('owned_mounts+=("$root/efi")', self.text)
        cleanup = self.text.split('teardown() {', 1)[1].split('trap teardown EXIT', 1)[0]
        self.assertNotIn('umount -R "$root"', cleanup)
        self.assertIn('if [ -z "$target" ]; then', cleanup)
        finish = self.text.split('teardown\ntrap - EXIT', 1)[1]
        self.assertLess(finish.index('if [ -n "$target" ]; then'), finish.index('bsdtar --numeric-owner'))
        self.assertIn('ESP unchanged', finish)

    def test_makepkg_flags_keep_hardening_and_replace_only_cpu_choices(self):
        block = self.text.split('cat >"$root/etc/makepkg.conf.d/99-locachy-cpu.conf" <<EOF\n', 1)[1].split('\nEOF', 1)[0]
        # Expand only the CPU placeholder, as the production heredoc does.
        block = block.replace('\\$', '$').replace('$level', 'x86-64-v3').replace('$cpu$tune', 'x86-64-v3 -mtune=generic') \
            .replace('$cpu', 'x86-64-v3')
        script = 'CFLAGS="-march=x86-64 -mtune=native -O2 -pipe -fstack-protector-strong -D_FORTIFY_SOURCE=3"\n' \
                 'CXXFLAGS="-O3 -march=x86-64-v2 -fno-plt"\n' + block + \
                 '\nprintf "%s\\n%s\\n" "$CFLAGS" "$CXXFLAGS"\n'
        result = subprocess.run(["bash", "-euc", script], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        c, cpp = result.stdout.splitlines()
        self.assertIn('-fstack-protector-strong', c)
        self.assertIn('-D_FORTIFY_SOURCE=3', c)
        self.assertIn('-O2 -pipe', c)
        self.assertIn('-fno-plt', cpp)
        self.assertEqual(c.count('-march='), 1)
        self.assertEqual(cpp.count('-march='), 1)
        self.assertIn('-march=x86-64-v3 -mtune=generic', c)
        self.assertNotIn('-mtune=native', c)

    def test_makepkg_flags_for_a_cpu_model_tune_for_it(self):
        self.assertIn('[ "$cpu" != "$level" ] || tune=" -mtune=generic"', self.text)
        block = self.text.split('cat >"$root/etc/makepkg.conf.d/99-locachy-cpu.conf" <<EOF\n', 1)[1].split('\nEOF', 1)[0]
        block = block.replace('\\$', '$').replace('$level', 'x86-64-v2').replace('$cpu$tune', 'ivybridge') \
            .replace('$cpu', 'ivybridge')
        script = 'CFLAGS="-march=x86-64 -mtune=generic -O2 -pipe"\nCXXFLAGS="$CFLAGS"\n' + block + \
                 '\nprintf "%s\\n%s\\n" "$CFLAGS" "$RUSTFLAGS"\n'
        result = subprocess.run(["bash", "-euc", script], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        c, rust = result.stdout.splitlines()
        self.assertEqual(c, '-O2 -pipe -march=ivybridge')
        self.assertEqual(rust, '-C target-cpu=ivybridge')


if __name__ == "__main__":
    unittest.main()
