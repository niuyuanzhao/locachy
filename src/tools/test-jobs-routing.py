#!/usr/bin/env python3
"""Fixed boot/TPM routes, with kernel identity and real socket input transfer."""
import array
import errno
import importlib.util
import os
from pathlib import Path
import socket
import tempfile
import threading
import types
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("locachy_jobs", SOURCE / "link/locachy_jobs.py")
jobs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jobs)


class Routing(unittest.TestCase):
    def test_pid1_init_and_deleted_init_require_queue(self):
        for path in ("/init", "/init (deleted)", jobs.INIT, jobs.INIT + " (deleted)"):
            with self.subTest(path=path), \
                 mock.patch.object(jobs.os, "stat", return_value=types.SimpleNamespace(st_dev=1, st_ino=2)), \
                 mock.patch.object(jobs.Path, "read_text", return_value="0::/p1\n"), \
                 mock.patch.object(jobs.os, "readlink", return_value=path):
                self.assertEqual(jobs.execution_mode(), "queue")

    @mock.patch.object(jobs.os, "stat", return_value=types.SimpleNamespace(st_dev=1, st_ino=2))
    def test_only_real_job_cgroup_membership_is_inline(self, _stat):
        def read(p):
            return "0::/locachy-jobs/job-7\n" if str(p) == "/proc/self/cgroup" else str(os.getpid()) + "\n"
        with mock.patch.object(jobs.Path, "read_text", read):
            self.assertEqual(jobs.execution_mode(), "job")
        with mock.patch.object(jobs.Path, "read_text", return_value="0::/locachy-jobs/job-7\n"):
            self.assertRaisesRegex(RuntimeError, "membership", jobs.execution_mode)
        with mock.patch.dict(os.environ, {"LOCACHY_JOB": "7", "LOCACHY_OFFLINE": "1"}), \
             mock.patch.object(jobs.Path, "read_text", return_value="0::/p1\n"), \
             mock.patch.object(jobs.os, "readlink", return_value="/init"):
            self.assertEqual(jobs.execution_mode(), "queue")

    def test_chroot_is_explicit_offline(self):
        with mock.patch.object(jobs.os, "stat", side_effect=[types.SimpleNamespace(st_dev=1, st_ino=2),
                                                            types.SimpleNamespace(st_dev=1, st_ino=3)]):
            self.assertEqual(jobs.execution_mode(), "offline")

    def test_boot_inside_job_is_inline_and_external_hook_never_waits(self):
        with mock.patch.object(jobs, "execution_mode", return_value="job"), mock.patch.object(jobs, "submit") as submit:
            self.assertEqual(jobs.boot_route(["--luks"]), 100)
            submit.assert_not_called()
        with mock.patch.object(jobs, "execution_mode", return_value="queue"), \
             mock.patch.object(jobs, "pacman_ancestor", return_value=True), mock.patch.object(jobs, "ask") as ask, \
             mock.patch.object(jobs, "submit") as submit:
            self.assertEqual(jobs.boot_route([]), 0)
            ask.assert_called_once_with(["submit", "boot"])
            submit.assert_not_called()

    def test_missing_service_does_not_build_inline(self):
        with mock.patch.object(jobs, "execution_mode", return_value="queue"), \
             mock.patch.object(jobs, "pacman_ancestor", return_value=False), \
             mock.patch.object(jobs, "ask", side_effect=FileNotFoundError):
            self.assertRaises(FileNotFoundError, jobs.boot_route, [])

    def test_pipe_input_has_eof_no_secret_in_message_and_no_fd_leak(self):
        try:
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        except OSError as e:
            if e.errno in (errno.EPROTONOSUPPORT, errno.EPROTOTYPE, errno.ENOPROTOOPT, errno.EAFNOSUPPORT):
                self.skipTest("platform lacks AF_UNIX SOCK_SEQPACKET")
            raise
        self.addCleanup(listener.close)
        with tempfile.TemporaryDirectory() as root:
            control = root + "/control"
            errors = []
            listener.bind(control)
            listener.listen()
            listener.settimeout(3)
            def serve():
                try:
                    with listener.accept()[0] as s:
                        msg, ancillary, _, _ = s.recvmsg(4096, socket.CMSG_SPACE(4))
                        self.assertEqual(msg, b"submit\0tpm-enroll")
                        fds = array.array("i")
                        fds.frombytes(ancillary[0][2])
                        with os.fdopen(fds[0], "rb") as stream:
                            self.assertEqual(stream.read(), b"secret with spaces")
                        s.send(b"ok\0" + b"7\0" + b"0")
                except BaseException as e:
                    errors.append(e)
            before = len(os.listdir("/proc/self/fd"))
            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            with mock.patch.object(jobs, "CONTROL", control):
                self.assertEqual(jobs.submit("tpm-enroll", secret="secret with spaces", wait=False), 0)
            thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors, errors)
            self.assertEqual(len(os.listdir("/proc/self/fd")), before)


if __name__ == "__main__":
    unittest.main()
