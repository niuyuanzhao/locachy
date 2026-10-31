#!/usr/bin/env python3
"""Cloud supervisor unit tests; real FUSE coverage is test-rclone-vm.sh."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1] / "link/rclone-client"
loader = importlib.machinery.SourceFileLoader("rclone_client", str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
cloud = importlib.util.module_from_spec(spec)
loader.exec_module(cloud)


class CloudTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="rclone-test-")
        self.root = Path(self.tmp.name)
        self.user = types.SimpleNamespace(pw_name="tester", pw_dir=str(self.root), pw_uid=1000, pw_gid=1000)
        with patch.object(cloud, "identity", return_value=(self.user, self.root / "Drives")):
            self.service = cloud.Service()
        self.service.write_status = Mock()
        self.service.notify = Mock()
        self.remote = cloud.Remote("disk space", self.root / "Drives/disk space", self.root / "private")
        self.service.remotes[self.remote.name] = self.remote

    def tearDown(self):
        self.service.selector.close()
        if self.remote.log:
            self.remote.log.close()
        self.tmp.cleanup()

    def test_safe_names_and_paths(self):
        for name in ("disk space", "日本語", "Google.Drive-1"):
            self.assertTrue(cloud.safe_name(name))
        for name in ("", ".", "..", "../other", "a:b", "a\\b", "line\nnext", "x\t"):
            self.assertFalse(cloud.safe_name(name))
        for path in ("relative", str(self.root / "Drives/disk space") + "/../other", "/other", "/line\nnext"):
            with self.assertRaises(cloud.CloudError):
                self.service.find(path)
        remote, directory = self.service.find(str(self.remote.point / "child space"))
        self.assertIs(remote, self.remote)
        self.assertEqual(directory, "child space")
        self.assertEqual(self.service.find(str(self.remote.point))[1], "")

    def test_oauth_is_conservative_and_no_raw_message_in_status(self):
        self.assertEqual(cloud.error_kind('oauth2: cannot fetch token: {"error":"invalid_grant"}'), "authentication")
        self.assertEqual(cloud.error_kind('oauth2: "invalid_token"'), "authentication")
        self.assertEqual(cloud.error_kind("couldn't fetch token: invalid_grant: maybe token expired? - try "
                                          'refreshing with "rclone config reconnect gdrive:"'), "authentication")
        for message, category in (("401 Unauthorized", "attention"), ("403 Forbidden", "permission"),
                                  ("429 Too Many Requests", "rate-limit"), ("503 Service Unavailable", "attention"),
                                  ("dial tcp: lookup host: no such host", "network"),
                                  ("Failed to open file named invalid_token.txt", "attention")):
            self.assertEqual(cloud.error_kind(message), category)
        self.service.failed(self.remote, 'oauth2: {"error":"invalid_grant","secret":"TOP-SECRET"}')
        row = cloud.public_row(self.remote)
        self.assertEqual(row["reason"], "authentication")
        self.assertNotIn("TOP-SECRET", json.dumps(row))

    def test_unsafe_remote_names_are_skipped_not_fatal(self):
        self.service.config.parent.mkdir(parents=True)
        self.service.config.write_text("")
        listing = json.dumps([{"name": "good"}, {"name": "../escape"}, {"name": "a:b"}, "junk", {"name": "good"}])
        with patch.object(cloud, "run", return_value=types.SimpleNamespace(stdout=listing)), \
                patch("builtins.print"):
            self.assertEqual(self.service.discover(), ["good"])

    def test_refresh_parameters(self):
        self.assertEqual(cloud.refresh_params(""), {"recursive": "false", "_async": True})
        self.assertEqual(cloud.refresh_params("a b/c"), {"recursive": "false", "_async": True, "dir": "a b/c"})

    def test_refresh_checks_requested_inner_result(self):
        cloud.refresh_output({"result": {"": "OK"}}, "")
        cloud.refresh_output({"result": {"a b": "OK", "other": "403"}}, "a b")
        for output in ({}, {"result": {"other": "OK"}}, {"result": {"a b": "403 Forbidden"}}):
            with self.assertRaises(cloud.CloudError):
                cloud.refresh_output(output, "a b")

    def test_inuse_is_not_an_open_file_counter(self):
        self.assertFalse(cloud.pending_writes({"inUse": 1, "diskCache": {"files": 3}}))
        for key in ("uploadsInProgress", "uploadsQueued", "erroredFiles"):
            self.assertTrue(cloud.pending_writes({"diskCache": {key: 1}}))

    def test_stop_refuses_dirty_cache_without_signal_or_unmount(self):
        process = self.remote.process = Mock()
        process.poll.return_value = None
        with patch.object(cloud, "rc", return_value={"inUse": 1, "diskCache": {"uploadsQueued": 1}}), \
                patch.object(cloud, "run") as run:
            with self.assertRaises(cloud.CloudError):
                self.service.stop(self.remote)
            run.assert_not_called()
        process.terminate.assert_not_called()

    def test_stop_never_unmounts_foreign_mount(self):
        self.remote.mounted_once = True
        with patch.object(cloud, "mounts", return_value={str(self.remote.point): ("fuse.sshfs", "elsewhere")}), \
                patch.object(cloud, "run") as run:
            with self.assertRaises(cloud.CloudError):
                self.service.stop(self.remote)
            run.assert_not_called()

    def test_log_failure_is_classified_and_not_replayed(self):
        self.remote.log = tempfile.TemporaryFile("w+b")
        self.remote.log_offset = 0
        self.remote.log.write(json.dumps({"level": "error", "msg": 'oauth2: {"error":"invalid_grant","token":"SECRET"}'}).encode() + b"\n")
        self.remote.log.flush()  # rclone writes the real file directly
        self.service.read_log(self.remote)
        self.assertEqual(self.remote.reason, "authentication")
        calls = self.service.notify.call_count
        self.service.read_log(self.remote)
        self.assertEqual(self.service.notify.call_count, calls)
        self.assertNotIn("SECRET", json.dumps(cloud.public_row(self.remote)))

    def test_ordinary_log_errors_do_not_mark_failure(self):
        self.remote.log, self.remote.log_offset = tempfile.TemporaryFile("w+b"), 0
        for msg in ("file.txt: Failed to copy: 503 Service Unavailable", "403 Forbidden", "dial tcp: no such host"):
            self.remote.log.write(json.dumps({"level": "error", "msg": msg}).encode() + b"\n")
        self.remote.log.write(b"not json\n")
        self.remote.log.flush()
        self.service.read_log(self.remote)
        self.assertEqual(self.remote.reason, "")

    def test_control_peers_and_busy_reply(self):
        server = cloud.socket.socket(cloud.socket.AF_UNIX, cloud.socket.SOCK_SEQPACKET)
        path = self.root / "control.sock"
        server.bind(str(path))
        server.listen(1)
        self.service.server = server
        self.service.stop = Mock(side_effect=cloud.CloudBusy("open"))
        with patch.object(cloud, "CONTROL", path):
            out = []
            with patch("builtins.print", side_effect=lambda s: out.append(s)):
                import threading
                t = threading.Thread(target=lambda: out.append(cloud.client("unmount", str(self.remote.point))))
                t.start()
                self.service.request()
                t.join(5)
        server.close()
        self.assertEqual(out[-1], 1)
        self.assertIn(cloud.REASONS["busy"], out)
        self.assertNotEqual(self.remote.state, "failed")
        self.service.notify.assert_not_called()

    def test_config_change_restarts_only_idle_authentication_failures(self):
        self.service.discover = Mock(return_value=[self.remote.name])
        self.service.start = Mock()
        self.service.config.parent.mkdir(parents=True)
        self.service.config.write_text("[x]\n")
        self.service.config_changed = 1
        self.remote.process, self.remote.reason = Mock(), "authentication"
        other = cloud.Remote("other", self.root / "Drives/other", self.root / "p2")
        other.process, other.reason = Mock(), "network"
        self.service.remotes["other"] = other
        stopped = []
        self.service.stop = Mock(side_effect=lambda r: stopped.append(r.name))
        self.remote.process.poll.return_value = other.process.poll.return_value = None
        self.service.discover = Mock(return_value=[self.remote.name, "other"])
        with patch.object(cloud, "mounts", return_value={}):
            self.service.tick()
        self.assertEqual(stopped, [self.remote.name])

    def test_network_command_preserves_only_explicit_shared_children(self):
        ns, resolver = self.root / "netns", self.root / "resolver"
        ns.touch()
        resolver.write_text("nameserver 127.0.0.1\n")
        with patch.object(cloud, "NETNS", str(ns)), patch.object(cloud, "RESOLVER", str(resolver)):
            command = cloud.network_command(["program", "argument with spaces"])
            self.assertIn("unchanged", command)
            shell = command[command.index("-c") + 1]
            self.assertIn("mount --make-slave /;", shell)
            self.assertNotIn("make-rslave", shell)
            self.assertIn("nsenter", shell)
            self.assertEqual(command[-2:], ["program", "argument with spaces"])
        with patch.object(cloud, "NETNS", "/missing"):
            with self.assertRaises(cloud.CloudError):
                cloud.network_command(["program"])

    def test_mount_command_does_not_discard_write_cache(self):
        with patch.object(cloud, "network_command", side_effect=lambda args: args):
            command = cloud.mount_command(self.user, self.remote.name, self.remote.point,
                                          self.remote.rc_socket, self.service.config)
        self.assertIn("--rc", command)
        self.assertIn("--rc-no-auth", command)
        self.assertIn(str(self.remote.rc_socket), command)
        self.assertIn("writes", command)
        self.assertEqual(command[command.index("--devname") + 1], "disk space:")
        for flag in ("--allow-non-empty", "--daemon", "--rc-web-gui"):
            self.assertNotIn(flag, command)
        self.assertIn("-i", command)

    def test_status_appends_only_safe_reason(self):
        self.remote.state, self.remote.reason = "failed", "authentication"
        self.service.write_status = cloud.Service.write_status.__get__(self.service)
        with patch.object(cloud, "STATUS", self.root / "status"):
            self.service.write_status()
        fields = (self.root / "status").read_text().strip().split("\t")
        self.assertEqual(fields, [self.remote.name, str(self.remote.point), "failed", "0", "authentication"])

    def test_notifications_deduplicate_replace_and_clear(self):
        self.service.notify = cloud.Service.notify.__get__(self.service)
        self.remote.state, self.remote.reason = "failed", "authentication"
        result = types.SimpleNamespace(returncode=0, stdout="(uint32 17,)\n")
        with patch.object(cloud.subprocess, "run", return_value=result) as run:
            self.service.notify(self.remote)
            self.service.notify(self.remote)
            self.assertEqual(run.call_count, 1)
            self.remote.reason = "network"
            self.service.notify(self.remote)
            self.assertIn("17", run.call_args.args[0])
            self.remote.state = "ready"
            self.service.notify(self.remote)
            self.assertIn("org.freedesktop.Notifications.CloseNotification", run.call_args.args[0])
            self.assertEqual(self.remote.notified, "")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(CloudTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f"{'PASS' if result.wasSuccessful() else 'FAIL'} cloud supervisor unit tests: {result.testsRun}")
    raise SystemExit(0 if result.wasSuccessful() else 1)
