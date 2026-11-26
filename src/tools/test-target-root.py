#!/usr/bin/env python3
"""Target-builder preflight/namespace/flag contracts with command doubles.
No real mount, package transaction, user mapping or block device is used.
  python3 -B tools/test-target-root.py SCRATCH_PARENT
"""
import fcntl
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
        # Exercise the real lock, marker and workspace setup in the final
        # namespace, with no real mounts or package commands.
        self.builder = self.root / "builder"
        self.builder.write_text(self.text.split("# This build's mirror lists", 1)[0] +
                                'printf "TARGET-READY\\n"\n')
        (self.root / "link").mkdir()
        (self.root / "link/locachy-repos").write_text("# No additional repository levels in the fixture.\n")
        self.command("mount", 'test "$*" = "-t cgroup2 cgroup2 /sys/fs/cgroup"')
        self.command("grep", 'test "$*" = "-q ^Server /etc/pacman.d/mirrorlist"')
        self.marker = self.target / ".locachy-installer/install-id"
        self.install_id = "a" * 32
        self.command("id", "printf '0\\n'")
        self.command("realpath", 'python3 -c "import os,sys; print(os.path.realpath(sys.argv[-1]))" "$@"')
        self.command("mountpoint", 'test "${FIX_MOUNTED:-1}" = 1')
        self.command("findmnt", 'case "$*" in *FSTYPE*) printf "%s\\n" "${FIX_FS:-ext4}" ;; '
                              '*LABEL*) printf "%s\\n" "${FIX_LABEL:-locachy-root}" ;; *) exit 1 ;; esac')
        self.command("stat", 'case "$*" in *" /") printf "1\\n" ;; *) printf "%s\\n" "${FIX_DEVICE:-2}" ;; esac')
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"], CALLS=str(self.calls))
        self.env.pop("MAKE_ROOT_NS", None)

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

    def run_builder(self, *flags, work="work"):
        workspace = ["--work-dir", str(self.root / work)] if work else []
        return subprocess.run(["bash", str(self.builder), "--target-root", str(self.target),
                               "--install-id", self.install_id, *workspace, *flags],
                              env=dict(self.env, MAKE_ROOT_NS="1"), text=True, capture_output=True, timeout=10)

    def test_marker_is_on_target_and_resume_preserves_existing_contents(self):
        first = self.run_builder()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(self.marker.read_bytes(), (self.install_id + "\n").encode())
        self.assertEqual(self.marker.stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.root / "work/install-id").exists())
        self.assertEqual(list(self.marker.parent.iterdir()), [self.marker])
        marker_stat = self.marker.stat()
        for path in (self.root / "work/previous.log", self.target / "keep"):
            path.write_text("untouched")
        for work in ("work", "other-work", None):
            with self.subTest(work=work):
                result = self.run_builder("--resume", work=work)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("TARGET-READY", result.stdout)
                self.assertIn("where the installation stopped", result.stdout)
                self.assertEqual((self.target / "keep").read_text(), "untouched")
                self.assertEqual((self.root / "work/previous.log").read_text(), "untouched")
                self.assertEqual(self.marker.read_text(), self.install_id + "\n")
                current = self.marker.stat()
                self.assertEqual((current.st_ino, current.st_mtime_ns), (marker_stat.st_ino, marker_stat.st_mtime_ns))

    def test_resume_refuses_missing_wrong_or_aliased_target_marker(self):
        workspace = self.root / "work"
        workspace.mkdir()
        external_marker = workspace / "install-id"
        external_marker.write_text(self.install_id + "\n")
        result = self.run_builder("--resume")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs a regular target installation marker", result.stderr)
        self.assertFalse(self.marker.parent.exists())
        self.marker.parent.symlink_to(workspace, target_is_directory=True)
        result = self.run_builder("--resume")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("without symlinks", result.stderr)
        self.marker.parent.unlink()
        self.marker.parent.mkdir()
        self.marker.symlink_to(external_marker)
        result = self.run_builder("--resume")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs a regular target installation marker", result.stderr)
        self.marker.unlink()
        for contents in ("b" * 32 + "\n", self.install_id + "\n\n"):
            with self.subTest(contents=contents):
                self.marker.write_text(contents)
                for flags in ([], ["--resume"]):
                    result = self.run_builder(*flags, work="new-work")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(self.marker.read_text(), contents)
                    self.assertFalse((self.root / "new-work").exists())

    def test_target_lock_excludes_other_workspaces_and_releases_on_close(self):
        self.assertEqual(self.run_builder().returncode, 0)
        fd = os.open(self.target, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_builder("--resume", work="other-work")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot lock target", result.stderr)
            self.assertFalse((self.root / "other-work").exists())
            other = os.open(self.target, os.O_RDONLY | os.O_DIRECTORY)
            try:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally:
                os.close(other)
        finally:
            os.close(fd)
        result = self.run_builder("--resume", work="other-work")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_pacman_locks_are_never_removed_on_resume(self):
        self.assertEqual(self.run_builder().returncode, 0)
        for path in (self.target / "var/lib/pacman/db.lck", self.root / "work/raw-db/db.lck"):
            with self.subTest(path=path):
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("wb") as stream:
                    stream.write(b"open lock")
                    stream.flush()
                    original = os.fstat(stream.fileno())
                    result = self.run_builder("--resume")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"pacman database is locked: {path}", result.stderr)
                    self.assertEqual(path.stat().st_ino, original.st_ino)
                    self.assertEqual(path.read_bytes(), b"open lock")
                path.unlink()
        result = self.run_builder("--resume")
        self.assertEqual(result.returncode, 0, result.stderr)

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
