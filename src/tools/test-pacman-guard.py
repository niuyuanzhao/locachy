#!/usr/bin/env python3
"""locachy-pacman-guard against real flock, real pidfd and a real process
named pacman (a shell script of that name). Linux only: elsewhere it skips.
  python3 -B tools/test-pacman-guard.py [SCRATCH_PARENT]
"""
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
GUARD = SOURCE / "link/locachy-pacman-guard"
LINUX = os.path.isdir("/proc/self") and hasattr(os, "pidfd_open")

# Runs the real guard with the lock and the job decision under the test's control.
WRAPPER = """import importlib.machinery, importlib.util, sys, types
loader = importlib.machinery.SourceFileLoader("guard", sys.argv[1])
spec = importlib.util.spec_from_loader("guard", loader)
m = importlib.util.module_from_spec(spec)
loader.exec_module(m)
m.LOCK = sys.argv[2]
real = m.jobs()
mode = sys.argv[3]
m.jobs = lambda: types.SimpleNamespace(execution_mode=lambda: mode, pacman_ancestor=real.pacman_ancestor)
sys.exit(m.pre())
"""


def busy(lock):
    """Whether another open file description holds lock (a real flock probe)."""
    fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    except BlockingIOError:
        return True
    finally:
        os.close(fd)


@unittest.skipUnless(LINUX, "pidfd and /proc: Linux only")
class PacmanGuard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pacman-guard-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.lock = str(self.dir / "pkg.lock")
        (self.dir / "wrapper.py").write_text(WRAPPER)
        self.procs = []
        self.addCleanup(self.kill_all)

    def kill_all(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait(timeout=10)
            for stream in (p.stdout, p.stderr):
                if stream:
                    stream.close()

    def fake_pacman(self, hold, mode="queue"):
        """A shell script named pacman: runs the guard as its child, then either
        holds (sleep) or exits at once, with the guard's status."""
        path = self.dir / "pacman"
        path.write_text(
            "#!/bin/sh\n"
            f'python3 -I -B "{self.dir}/wrapper.py" "{GUARD}" "{self.lock}" "{mode}"\n'
            "status=$?\n"
            'echo "guard=$status"\n'
            f'if [ "$status" = 0 ] && [ "{hold}" = 1 ]; then sleep 30; fi\n'
            "exit $status\n")
        path.chmod(0o755)
        return str(path)

    def start(self, command):
        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(p)
        return p

    def wait_until_free(self, seconds=5):
        end = time.monotonic() + seconds
        while busy(self.lock):
            if time.monotonic() > end:
                self.fail("the domain stayed held after pacman exited")
            time.sleep(0.05)

    def test_busy_domain_refuses_at_once_and_changes_nothing(self):
        holder = self.start(["python3", "-I", "-c",
                             "import fcntl,os,sys,time\n"
                             "fd=os.open(sys.argv[1],os.O_RDWR|os.O_CREAT,0o600)\n"
                             "fcntl.flock(fd,fcntl.LOCK_EX)\n"
                             "print('held',flush=True)\ntime.sleep(30)\n", self.lock])
        self.assertEqual(holder.stdout.readline().strip(), "held")
        started = time.monotonic()
        result = subprocess.run([self.fake_pacman(hold=0)], capture_output=True, text=True, timeout=10)
        self.assertLess(time.monotonic() - started, 5)
        self.assertIn("guard=1", result.stdout)
        self.assertIn("busy", result.stderr)
        self.assertNotEqual(result.returncode, 0)  # AbortOnFail: pacman stops
        self.assertTrue(busy(self.lock))  # the holder still holds it, the guard holds nothing

    def test_hook_returns_while_pacman_runs_and_domain_frees_when_pacman_is_killed(self):
        pacman = self.start([self.fake_pacman(hold=1)])
        self.assertEqual(pacman.stdout.readline().strip(), "guard=0")
        self.assertTrue(busy(self.lock))  # held for as long as this pacman lives
        os.kill(pacman.pid, signal.SIGKILL)
        pacman.wait(timeout=10)
        self.wait_until_free()

    def test_guard_never_holds_pacmans_output_pipe(self):
        # The guard's child must not keep stdout/stderr: communicate() returns
        # as soon as pacman exits, while the child may still hold the lock.
        result = subprocess.run([self.fake_pacman(hold=0)], capture_output=True, text=True, timeout=10)
        self.assertIn("guard=0", result.stdout)
        self.wait_until_free()

    def test_a_system_job_passes_without_taking_the_domain(self):
        pacman = self.start([self.fake_pacman(hold=1, mode="job")])
        self.assertEqual(pacman.stdout.readline().strip(), "guard=0")
        self.assertFalse(busy(self.lock))
        os.kill(pacman.pid, signal.SIGKILL)
        pacman.wait(timeout=10)

    def test_the_guard_refuses_outside_pacman(self):
        result = subprocess.run(["python3", "-I", "-B", str(self.dir / "wrapper.py"), str(GUARD), self.lock,
                                 "queue"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not run by pacman", result.stderr)


if __name__ == "__main__":
    unittest.main()
