#!/usr/bin/env python3
"""Boot selection and publication using private paths and executable doubles."""
import gzip
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class BootKernel(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="boot-kernel-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.lib = self.root / "lib"
        self.lib.mkdir()
        (self.lib / "init").write_text("fixture")
        (self.lib / "init").chmod(0o755)
        self.rc = self.root / "rc.conf"
        self.rc.write_text("")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        self.image = self.root / "7.3-lts/vmlinuz"
        self.image.parent.mkdir()
        self.image.write_text("kernel")
        pacman = self.bin / "pacman"
        pacman.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >>"$TEST_CALLS"\n'
                          'case "${TEST_MODE:-one}" in missing) exit 1;; '
                          'two) printf "%s\\n%s\\n" "$TEST_IMAGE" "$TEST_IMAGE";; '
                          '*) printf "%s\\n" "$TEST_IMAGE";; esac\n')
        pacman.chmod(0o755)
        script = (SOURCE / "link/locachy-boot").read_text()
        # Execute the actual selection code only; building and ESP writes are
        # deliberately outside this test process.
        stop = script.index('\nmicrocode=')
        script = '#!/bin/sh\nset -eu\n' + script[script.index('lib=/usr/local/lib/locachy'):stop] + \
            '\nprintf "selected=%s version=%s\\n" "$kernel_package" "$version"\n'
        script = script.replace("lib=/usr/local/lib/locachy", "lib=" + str(self.lib))
        script = script.replace("rc=/etc/locachy/rc.conf", "rc=" + str(self.rc))
        self.script = self.root / "boot"
        self.script.write_text(script)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TEST_CALLS=str(self.calls), TEST_IMAGE=str(self.image), TEST_ROOT=str(self.root))

    def run_boot(self, selection=None, mode="one"):
        self.rc.write_text('boot_kernel="' + selection + '"\n' if selection is not None else "")
        return subprocess.run(["sh", str(self.script)], env=dict(self.env, TEST_MODE=mode),
                              capture_output=True, text=True, timeout=10)

    def test_default_preserves_existing_kernel(self):
        r = self.run_boot()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("selected=linux-cachyos-bore", r.stdout)

    def test_selection_controls_the_boot_package(self):
        r = self.run_boot("linux-lts")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("selected=linux-lts version=7.3-lts", r.stdout)
        self.assertEqual(self.calls.read_text().strip(), "-Qlq linux-lts")

    def test_bad_names_never_reach_pacman(self):
        for value in ("--help", "linux;bad", "linux-headers", "/vmlinuz", "linux foo"):
            with self.subTest(value=value):
                r = self.run_boot(value)
                self.assertNotEqual(r.returncode, 0)
                self.assertFalse(self.calls.exists())

    def test_absent_or_ambiguous_images_fail_without_building(self):
        for mode in ("missing", "two"):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.run_boot("linux-lts", mode).returncode, 0)


@unittest.skipUnless(shutil.which("flock"), "requires util-linux flock")
class BootTransaction(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="boot-transaction-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.fs = self.root / "root"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("usr/local/lib/locachy/boot", "usr/local/bin", "usr/lib/modules/fixture",
                     "etc/locachy", "efi/EFI/locachy", "boot"):
            (self.fs / name).mkdir(parents=True, exist_ok=True)
        init = self.fs / "usr/local/lib/locachy/init"
        init.write_text("init")
        init.chmod(0o755)
        self.boot = init.parent / "boot"
        (self.fs / "etc/locachy/rc.conf").write_text("")
        (self.fs / "usr/lib/modules/fixture/vmlinuz").write_text("kernel")
        self.current = self.fs / "efi/EFI/locachy/locachy.efi"
        self.previous = self.current.with_name("locachy-prev.efi")
        self.fallback = self.fs / "efi/EFI/BOOT/BOOTX64.EFI"
        self.fallback.parent.mkdir()
        for path in (self.boot / "initramfs.img", self.boot / "locachy.efi", self.current, self.previous, self.fallback):
            path.write_bytes(b"old " + path.name.encode())
        self.original = {p: p.read_bytes() for p in
                         (self.boot / "initramfs.img", self.boot / "locachy.efi", self.current, self.previous, self.fallback)}
        # This suite exercises boot transactions, not installed PID 1 routing.
        # The routing suite checks real socket/descriptor handling separately.
        (self.fs / "usr/local/lib/locachy/locachy_jobs.py").write_text(
            'import sys\nprint("offline transaction fixture", file=sys.stderr)\nsys.exit(100)\n')
        script = (SOURCE / "link/locachy-boot").read_text()
        for prefix in ("/usr/", "/etc/", "/var/", "/efi", "/boot/", "/run/lock"):
            script = script.replace(prefix, str(self.fs) + prefix)
        self.script = self.root / "boot.sh"
        self.script.write_text(script)
        self.helper = self.root / "double.py"
        self.helper.write_text('''import gzip, os
from pathlib import Path
import sys, time
root = Path(os.environ["TEST_ROOT"])
role = os.environ.get("TEST_ROLE", "A")
tool = sys.argv[1]
args = sys.argv[2:]
def wait(name):
    end = time.monotonic() + 10
    while not (root / name).exists():
        if time.monotonic() > end:
            raise RuntimeError("barrier timeout: " + name)
        time.sleep(0.01)
if tool == "pacman":
    print(root / "root/usr/lib/modules/fixture/vmlinuz")
elif tool == "gen_init_cpio":
    (root / (role + ".producer")).touch()
    with (root / "producer-paths").open("a") as f:
        f.write(args[0] + "\\n")
    if os.environ.get("TEST_BLOCK"):
        wait(role + ".producer-resume")
    sys.stdout.buffer.write(role.encode() * 65536)
    if os.environ.get("TEST_FAILURE") == "producer":
        sys.exit(42)
elif tool == "gzip":
    if args[0] == "-t":
        with (root / "checks").open("a") as f:
            f.write(args[1] + "\\n")
        data = Path(args[1]).read_bytes()
        if os.environ.get("TEST_FAILURE") == "trailing":
            sys.exit(2 if data.endswith(b"garbage") else 0)
        gzip.decompress(data)
    else:
        data = gzip.compress(sys.stdin.buffer.read(), mtime=0)
        if os.environ.get("TEST_FAILURE") == "gzip":
            data = b"invalid gzip"
        elif os.environ.get("TEST_FAILURE") == "trailing":
            data += b"garbage"
        sys.stdout.buffer.write(data)
elif tool == "ukify":
    (root / "ukify-called").touch()
    if os.environ.get("TEST_FAILURE") == "ukify":
        sys.exit(1)
    src = Path(next(a.split("=", 1)[1] for a in args if a.startswith("--initrd=")))
    dst = Path(next(a.split("=", 1)[1] for a in args if a.startswith("--output=")))
    dst.write_bytes(src.read_bytes())
elif tool == "findmnt":
    print(root / "root/efi")
elif tool == "sync":
    pass
elif tool == "efi":
    (root / (role + ".efi")).touch()
    if os.environ.get("TEST_BLOCK"):
        wait(role + ".efi-resume")
else:
    raise RuntimeError(tool)
''')
        for tool in ("pacman", "gen_init_cpio", "ukify", "findmnt", "sync", "efi"):
            self.write_double(tool)
        shutil.copyfile(self.bin / "gen_init_cpio", init.parent / "gen_init_cpio")
        (init.parent / "gen_init_cpio").chmod(0o755)
        shutil.copyfile(self.bin / "efi", self.fs / "usr/local/bin/locachy-efi")
        (self.fs / "usr/local/bin/locachy-efi").chmod(0o755)
        self.env = dict(os.environ, TEST_ROOT=str(self.root), PATH=str(self.bin) + os.pathsep + os.environ["PATH"])

    def write_double(self, tool):
        path = self.bin / tool
        path.write_text('#!/bin/sh\nexec "' + sys.executable + '" -B "' + str(self.helper) + '" ' + tool + ' "$@"\n')
        path.chmod(0o755)

    def run_boot(self, **env):
        return subprocess.run(["sh", str(self.script)], env=dict(self.env, **env), text=True,
                              capture_output=True, timeout=15)

    def wait_for(self, name):
        end = time.monotonic() + 10
        while not (self.root / name).exists():
            if time.monotonic() > end:
                self.fail("barrier timeout: " + name)
            time.sleep(0.01)

    def test_producer_failure_cannot_publish_a_valid_gzip_of_partial_cpio(self):
        result = self.run_boot(TEST_FAILURE="producer")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "ukify-called").exists())
        self.assertFalse((self.root / "checks").exists())
        for path, data in self.original.items():
            self.assertEqual(path.read_bytes(), data)
        self.assertEqual(list(self.boot.glob(".work.*")), [])

    def test_invalid_or_trailing_gzip_cannot_reach_ukify_or_publication(self):
        self.write_double("gzip")
        for failure in ("gzip", "trailing"):
            with self.subTest(failure=failure):
                result = self.run_boot(TEST_FAILURE=failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "ukify-called").exists())
                for path, data in self.original.items():
                    self.assertEqual(path.read_bytes(), data)
        self.assertEqual(list(self.boot.glob(".work.*")), [])

    def test_ukify_failure_leaves_all_published_images_unchanged(self):
        result = self.run_boot(TEST_FAILURE="ukify")
        self.assertNotEqual(result.returncode, 0)
        for path, data in self.original.items():
            self.assertEqual(path.read_bytes(), data)
        self.assertEqual(list(self.boot.glob(".work.*")), [])

    def test_concurrent_builds_wait_through_esp_registration_and_use_private_stages(self):
        output = []
        processes = []
        try:
            for role in ("A", "B"):
                out, err = self.root / (role + ".out"), self.root / (role + ".err")
                output.append((out, err))
                with out.open("w") as stdout, err.open("w") as stderr:
                    processes.append(subprocess.Popen(["sh", str(self.script)], stdout=stdout, stderr=stderr,
                                     env=dict(self.env, TEST_ROLE=role, TEST_BLOCK="1")))
                if role == "A":
                    self.wait_for("A.producer")
                    lock = self.fs / "run/lock/locachy-boot.lock"
                    inode = lock.stat().st_ino
            # A deterministic failed nonblocking lock proves A owns this inode.
            held = subprocess.run(["flock", "-n", str(lock), "true"], timeout=5)
            self.assertNotEqual(held.returncode, 0)
            self.assertFalse((self.root / "B.producer").exists())
            (self.root / "A.producer-resume").touch()
            self.wait_for("A.efi")
            self.assertFalse((self.root / "B.producer").exists())
            self.assertEqual(gzip.decompress(self.current.read_bytes()), b"A" * 65536)
            (self.root / "A.efi-resume").touch()
            self.wait_for("B.producer")
            (self.root / "B.producer-resume").touch()
            self.wait_for("B.efi")
            self.assertEqual(gzip.decompress(self.previous.read_bytes()), b"A" * 65536)
            (self.root / "B.efi-resume").touch()
            for process, (out, err) in zip(processes, output):
                self.assertEqual(process.wait(timeout=10), 0, out.read_text() + err.read_text())
            self.assertEqual(lock.stat().st_ino, inode)
            stages = [Path(p).parent for p in (self.root / "producer-paths").read_text().splitlines()]
            self.assertEqual(len(stages), 2)
            self.assertNotEqual(stages[0], stages[1])
            self.assertTrue(all(p.parent == self.boot for p in stages))
            for path in (self.boot / "initramfs.img", self.boot / "locachy.efi", self.current, self.fallback):
                self.assertEqual(gzip.decompress(path.read_bytes()), b"B" * 65536)
            self.assertEqual(list(self.boot.glob(".work.*")), [])
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
