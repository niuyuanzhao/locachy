#!/usr/bin/env python3
"""Rescue cable API/unit gates, plus a real Linux rootless OpenSSH fixture.

  python3 -B tools/test-rescue-link.py
  python3 -B tools/test-rescue-link.py --linux-auth

The authentication gate requires installed OpenSSH, openssl, iproute2, util-linux
and subordinate UID/GID mappings. It runs in user/mount/network/PID namespaces,
never changes host accounts, packages, services, or network devices. It tests
real password rejection/acceptance, SSH, SFTP and unchanged outer shadow bytes;
it is not the full physical-port/systemd VM acceptance gate.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1]
BACKEND = SOURCE / "installer/rescue-link.py"
UNIT = SOURCE / "installer/locachy-rescue-link.service"
spec = importlib.util.spec_from_file_location("rescue_link", BACKEND)
link = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link)


class LinkTests(unittest.TestCase):
    def test_exact_password_validation(self):
        for value in (b"", b"a" * 11, b"a" * 257, b"valid-password\n", b"valid\x00password", b"valid\rpassword"):
            with self.assertRaises(link.LinkError):
                link.validate_password(value)
        for value in (b"twelve-bytes!", b"  exact spaces  ", b"x" * 256, "rescue-中文-password".encode()):
            link.validate_password(value)

    def test_interface_names_are_not_shell_syntax(self):
        for name in ("eno1", "enp0s31f6", "eth0.2", "usb-ether"):
            self.assertTrue(link.valid_name(name))
        for name in ("", ".", "..", "eth0;id", "../eno1", "a" * 16, "eth0\n"):
            self.assertFalse(link.valid_name(name))

    def test_config_is_independent_and_password_only(self):
        conf = link.ssh_config("fe80::1234", "enp1s0")
        for row in ("ListenAddress [fe80::1234%enp1s0]:2222", "UsePAM no",
                    "AuthenticationMethods password", "PubkeyAuthentication no",
                    "DisableForwarding yes", "PermitUserRC no", "Subsystem sftp internal-sftp"):
            self.assertIn(row, conf)
        self.assertNotIn("Include", conf)
        self.assertNotIn("0.0.0.0", conf)

    def test_unit_has_separate_cleanup_and_cgroup(self):
        conf = UNIT.read_text()
        for row in ("PrivateMounts=yes", "BindReadOnlyPaths=", "KillMode=control-group",
                    "ExecStopPost=/usr/bin/nsenter --mount=/proc/1/ns/mnt -- /usr/bin/python3 -I ",
                    "RuntimeMaxSec=1h", "LimitCORE=0"):
            self.assertIn(row, conf)
        self.assertNotIn("NetworkNamespacePath=", conf)
        self.assertNotIn("ExecStop=", conf)
        self.assertNotIn("WantedBy=", conf)

    def test_start_rejects_before_side_effects(self):
        with patch.object(link, "locked", side_effect=AssertionError("side effect")):
            with self.assertRaises(link.LinkError):
                link.start("eno1", b"short")

    def test_cleanup_does_not_stop_recursively(self):
        with patch.object(link, "locked", return_value=__import__("contextlib").nullcontext()), \
                patch.object(link, "load", return_value=None), patch.object(link, "command") as cmd:
            self.assertIn("No rescue", link.cleanup())
            cmd.assert_not_called()

    def test_private_auth_never_changes_global_shadow(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shadow = b"root:!:1:0:99999:7:::\nother:!:1:0:99999:7:::\n"
            original_read = Path.read_bytes
            hashed = b"$6$abcdefghijklmnop$" + b"a" * 86
            seen = []
            def read(path):
                return shadow if str(path) == "/etc/shadow" else original_read(path)
            def cmd(args, **kwargs):
                seen.append((args, kwargs))
                return subprocess.CompletedProcess(args, 0, hashed + b"\n", b"")
            with patch.object(link, "ROOT", root), patch.object(link, "secure_directory", lambda p: p.mkdir(exist_ok=True)), \
                    patch.object(Path, "read_bytes", read), patch.object(link, "command", cmd):
                link.private_auth(b" exact-password ")
            self.assertEqual(seen[0][0], ["openssl", "passwd", "-6", "-stdin"])
            self.assertEqual(seen[0][1]["data"], b" exact-password \n")
            self.assertNotIn("password", " ".join(seen[0][0]))
            view = (root / "auth/etc/shadow").read_bytes()
            self.assertIn(b"other:!:1:0:99999:7:::\n", view)
            self.assertIn(b"root:" + hashed + b":::::::\n", view)
            self.assertEqual((root / "auth/etc/shadow").stat().st_mode & 0o777, 0o600)

    def test_cleanup_waits_for_async_port_return(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_path = root / "state.json"
            state = {"interface": "eno1", "device": "/sys/devices/fake", "namespace": True,
                     "moved": True, "detached": False, "up": True, "mac": "aa:bb:cc:dd:ee:ff",
                     "nm_active": False, "nm_uuid": "", "nm_autoconnect": "no", "state": "stopping"}
            state_path.write_text(json.dumps(state))
            sequence = iter([None, "eno1"])
            original_root = link.ROOT
            original_state = link.STATE
            link.ROOT = root
            link.STATE = state_path
            try:
                with patch.object(link, "locked", return_value=__import__("contextlib").nullcontext()), \
                        patch.object(link, "load", return_value=state), \
                        patch.object(link, "text", return_value=""), patch.object(link, "ip") as ip, \
                        patch.object(link, "interface_by_identity", side_effect=lambda s: next(sequence)) as identity, \
                        patch.object(link.time, "sleep") as sleep:
                    self.assertEqual(link.cleanup(), "Rescue cable stopped; the selected port was restored")
                    self.assertEqual(identity.call_count, 2)
                    sleep.assert_called_once_with(0.1)
                    self.assertFalse(state_path.exists())
                    self.assertIn(__import__("unittest").mock.call("link", "set", "dev", "eno1", "up"), ip.call_args_list)
            finally:
                link.ROOT = original_root
                link.STATE = original_state

    def test_cleanup_keeps_state_when_port_never_returns(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_path = root / "state.json"
            state = {"interface": "eno1", "device": "/sys/devices/fake", "namespace": True,
                     "moved": True, "detached": False, "up": True, "mac": "aa:bb:cc:dd:ee:ff",
                     "nm_active": False, "nm_uuid": "", "nm_autoconnect": "no", "state": "stopping"}
            state_path.write_text(json.dumps(state))
            original_root = link.ROOT
            original_state = link.STATE
            link.ROOT = root
            link.STATE = state_path
            try:
                with patch.object(link, "locked", return_value=__import__("contextlib").nullcontext()), \
                        patch.object(link, "load", return_value=state), \
                        patch.object(link, "text", return_value=""), patch.object(link, "ip"), \
                        patch.object(link, "interface_by_identity", return_value=None), \
                        patch.object(link.time, "monotonic", side_effect=[0, 1, 6]), \
                        patch.object(link.time, "sleep"):
                    with self.assertRaises(link.LinkError):
                        link.cleanup()
                    self.assertTrue(state_path.exists())
                    self.assertEqual(json.loads(state_path.read_text())["state"], "stopping")
            finally:
                link.ROOT = original_root
                link.STATE = original_state

    def test_stop_waits_for_systemd_before_cleanup(self):
        calls = []
        with patch.object(link, "locked", return_value=__import__("contextlib").nullcontext()), \
                patch.object(link, "command", lambda *a, **k: calls.append("stop")), \
                patch.object(link, "service_active", return_value=False), \
                patch.object(link, "cleanup", lambda: calls.append("cleanup")):
            link.stop()
        self.assertEqual(calls, ["stop", "cleanup"])


def run(args, **kwargs):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20, **kwargs)
    if result.returncode:
        raise AssertionError(args[0] + ": " + result.stderr.decode(errors="replace"))
    return result


def interactive(args, password, *, expect_success):
    import fcntl
    import termios
    master, slave = pty.openpty()
    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave,
                            preexec_fn=controlling_terminal)
    os.close(slave)
    buf = b""
    deadline = time.monotonic() + 15
    sent = False
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    data = b""
                if not data:
                    break
                buf += data
                if b"password:" in buf.lower() and not sent:
                    os.write(master, password + b"\n")
                    sent = True
            if proc.poll() is not None and not ready:
                break
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        code = proc.wait(timeout=5)
        if expect_success and code != 0:
            raise AssertionError("SSH fixture failed: " + buf.decode(errors="replace"))
        if not expect_success and code == 0:
            raise AssertionError("Wrong SSH password was accepted")
        return buf
    finally:
        os.close(master)
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()


def auth_inside(work):
    work = Path(work)
    run(["mount", "--make-rprivate", "/"])
    # Mapped fixture files replace host account metadata only inside this mount
    # namespace. A private /run gives sshd its normal privilege-separation path.
    etc = work / "etc"
    etc.mkdir()
    (etc / "passwd").write_text("root:x:0:0:Rescue fixture:/root:/bin/sh\n"
                               "sshd:x:100:100:Privilege separation:/:/usr/bin/nologin\n"
                               "nobody:x:65534:65534:Overflow:/:/usr/bin/nologin\n")
    (etc / "group").write_text("root:x:0:\nsshd:x:100:\nnobody:x:65534:\n")
    (etc / "shadow").write_text("root:!:1:0:99999:7:::\n")
    (etc / "nsswitch.conf").write_text("passwd: files\ngroup: files\nshadow: files\n")
    for name in ("passwd", "group", "shadow", "nsswitch.conf"):
        run(["mount", "--bind", str(etc / name), "/etc/" + name])
    empty = work / "empty"
    empty.mkdir(mode=0o755)
    for directory in ("/usr/share/empty.sshd", "/var/empty", "/var/lib/sshd"):
        if Path(directory).is_dir():
            run(["mount", "--bind", str(empty), directory])
    run(["mount", "-t", "tmpfs", "-o", "mode=755", "tmpfs", "/run"])
    for path in ("/run/sshd", "/run/locachy-rescue", "/run/locachy-rescue/link"):
        Path(path).mkdir(mode=0o700)
    os.umask(0o077)
    password = b"  rescue-fixture-password  "
    link.private_auth(password)
    run(["mount", "--bind", str(link.ROOT / "auth/etc/shadow"), "/etc/shadow"])
    run(["mount", "-o", "remount,bind,ro", "/etc/shadow"])
    assert os.path.samefile("/etc/shadow", link.ROOT / "auth/etc/shadow")
    run(["ip", "link", "set", "lo", "up"])
    run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(link.ROOT / "host_ed25519")])
    run(["mkdir", "-p", "/etc/ssh"])
    config = link.ssh_config("::1", "lo").replace("[::1%lo]", "[::1]")
    (link.ROOT / "sshd_config").write_text(config)
    sshd = shutil.which("sshd")
    run([sshd, "-t", "-f", str(link.ROOT / "sshd_config")])
    known = work / "known_hosts"
    public = (link.ROOT / "host_ed25519.pub").read_text().split()
    known.write_text("[::1]:2222 " + " ".join(public[:2]) + "\n")
    server = subprocess.Popen([sshd, "-D", "-e", "-f", str(link.ROOT / "sshd_config")],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    common = ["-F", "/dev/null", "-o", "ControlPath=none", "-o", "StrictHostKeyChecking=yes",
              "-o", "UserKnownHostsFile=" + str(known), "-o", "GlobalKnownHostsFile=/dev/null",
              "-o", "PreferredAuthentications=password", "-o", "NumberOfPasswordPrompts=1",
              "-o", "ConnectTimeout=3"]
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise AssertionError("sshd startup: " + server.stderr.read().decode())
            if b":2222" in run(["ss", "-Hlnt"]).stdout:
                break
            time.sleep(0.1)
        args = ["ssh", *common, "-p", "2222", "root@::1"]
        interactive([*args, "true"], b"wrong-password", expect_success=False)
        print("PASS real OpenSSH rejects wrong password", flush=True)
        result = interactive([*args, "printf RESCUE_AUTH_OK; id -u"], password, expect_success=True)
        assert b"RESCUE_AUTH_OK0" in result, result
        print("PASS real OpenSSH authenticates exact password and executes as root", flush=True)
        result = interactive(["sftp", *common, "-P", "2222", "root@[::1]:/etc/passwd", str(work / "copied")],
                             password, expect_success=True)
        assert (work / "copied").read_bytes() == (etc / "passwd").read_bytes()
        print("PASS real internal SFTP transfers file", flush=True)
        assert (etc / "shadow").read_bytes() == b"root:!:1:0:99999:7:::\n"
        print("PASS private shadow source unchanged", flush=True)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


def linux_auth():
    if sys.platform != "linux":
        raise SystemExit("FAIL --linux-auth requires Linux")
    for name in ("unshare", "mount", "ip", "ss", "sshd", "ssh", "sftp", "openssl", "ssh-keygen"):
        if not shutil.which(name):
            raise SystemExit("FAIL missing packaged tool: " + name)
    before = hashlib.sha256(Path("/etc/shadow").read_bytes()).hexdigest() if os.access("/etc/shadow", os.R_OK) else None
    with tempfile.TemporaryDirectory(prefix="rescue-auth-") as tmp:
        result = subprocess.run(["unshare", "--user", "--map-auto", "--map-root-user", "--mount",
                                 "--net", "--pid", "--fork", "--mount-proc", sys.executable, "-B",
                                 str(Path(__file__).resolve()), "--auth-inside", tmp], timeout=90)
        if result.returncode:
            raise SystemExit("FAIL real namespace OpenSSH fixture")
    if before:
        assert hashlib.sha256(Path("/etc/shadow").read_bytes()).hexdigest() == before
    print("PASS real Linux namespace authentication fixture (not full VM acceptance)")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--linux-auth":
        linux_auth()
    elif len(sys.argv) == 3 and sys.argv[1] == "--auth-inside":
        auth_inside(sys.argv[2])
    else:
        unittest.main()
