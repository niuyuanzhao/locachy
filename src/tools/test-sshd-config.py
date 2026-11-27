#!/usr/bin/env python3
"""Device-free unit tests for managed OpenSSH server support in tailnet-ssh-client."""
import importlib.machinery
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import patch
from urllib.parse import urlparse

SOURCE = Path(__file__).resolve().parents[1] / "link/tailnet-ssh-client"
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
if SCRATCH:
    Path(SCRATCH).mkdir(parents=True, exist_ok=True)

loader = importlib.machinery.SourceFileLoader("tailnet_ssh_client", str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)


def fake_owned(info, kind, forbidden):
    return (kind(info.st_mode) and info.st_uid == os.geteuid()
            and not (info.st_mode & forbidden))


class SshdConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="sshd-config-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.root.chmod(0o755)

    def _gen_keypair(self, name, comment=""):
        path = self.root / name
        cmd = ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(path)]
        if comment:
            cmd.extend(["-C", comment])
        subprocess.run(cmd, check=True, stdin=subprocess.DEVNULL)
        return path, Path(str(path) + ".pub").read_text(encoding="utf-8").strip()

    def test_owned_requires_root_and_mode_mask(self):
        good = types.SimpleNamespace(st_mode=stat.S_IFREG | 0o600, st_uid=0, st_gid=0)
        bad_uid = types.SimpleNamespace(st_mode=stat.S_IFREG | 0o600, st_uid=1000, st_gid=0)
        bad_gid = types.SimpleNamespace(st_mode=stat.S_IFREG | 0o600, st_uid=0, st_gid=1000)
        bad_mode = types.SimpleNamespace(st_mode=stat.S_IFREG | 0o644, st_uid=0, st_gid=0)
        bad_kind = types.SimpleNamespace(st_mode=stat.S_IFLNK | 0o600, st_uid=0, st_gid=0)
        self.assertTrue(mod.owned(good, stat.S_ISREG, 0o177))
        self.assertFalse(mod.owned(bad_uid, stat.S_ISREG, 0o177))
        self.assertFalse(mod.owned(bad_gid, stat.S_ISREG, 0o177))
        self.assertFalse(mod.owned(bad_mode, stat.S_ISREG, 0o177))
        self.assertFalse(mod.owned(bad_kind, stat.S_ISREG, 0o177))

    def test_rc_conf_parsing(self):
        missing = self.root / "missing.conf"
        self.assertEqual(mod.read_rc(missing), ("tailscale", 22, False))

        rc = self.root / "rc.conf"
        rc.write_text(
            "# comment\n"
            "\n"
            'ssh_listen="lan"\n'
            'ssh_port="2222" # custom port\n'
            'ssh_password="1"\n',
            encoding="utf-8",
        )
        self.assertEqual(mod.read_rc(rc), ("lan", 2222, True))

        # Last occurrence wins.
        rc.write_text(
            'ssh_listen="lan"\n'
            'ssh_listen="tailscale"\n'
            'ssh_port="2222"\n'
            'ssh_port="2200"\n'
            'ssh_password="1"\n'
            'ssh_password="0"\n',
            encoding="utf-8",
        )
        self.assertEqual(mod.read_rc(rc), ("tailscale", 2200, False))

        # Invalid values fall back safely.
        for bad_port in ("0", "65536", "-1", "ssh", "22.5", "٢٢"):
            rc.write_text(
                f'ssh_listen="everywhere"\nssh_port="{bad_port}"\nssh_password="yes"\n',
                encoding="utf-8",
            )
            self.assertEqual(mod.read_rc(rc), ("tailscale", 22, False))

    def test_ssh_config_modes_and_validation(self):
        with self.assertRaises(ValueError):
            mod.ssh_config(())

        bound = (
            ("tailscale0", ipaddress.ip_address("100.64.17.1")),
            ("tailscale0", ipaddress.ip_address("fd7a:115c:a1e0::17")),
            ("eth0", ipaddress.ip_address("192.168.10.2")),
            # Duplicate address on another entry should not emit duplicate ListenAddress.
            ("eth1", ipaddress.ip_address("192.168.10.2")),
        )
        host_key, _ = self._gen_keypair("host_ed25519")
        host_key.chmod(0o600)

        with patch.object(mod, "HOST_KEY", host_key):
            default_cfg = mod.ssh_config(bound, port=22, password_auth=False)
            pwd_cfg = mod.ssh_config(bound, port=2222, password_auth=True)

        self.assertIn("ListenAddress 100.64.17.1:22\n", default_cfg)
        self.assertIn("ListenAddress [fd7a:115c:a1e0::17]:22\n", default_cfg)
        self.assertEqual(default_cfg.count("ListenAddress 192.168.10.2:22\n"), 1)
        for expected in (
            "Port 22",
            "UsePAM no",
            "PasswordAuthentication no",
            "AuthenticationMethods publickey",
            "PermitRootLogin no",
            "KbdInteractiveAuthentication no",
            "PermitEmptyPasswords no",
            "PubkeyAuthentication yes",
            "HostbasedAuthentication no",
            "GSSAPIAuthentication no",
            "AuthorizedKeysFile none",
            f"AuthorizedKeysCommand {mod.CLIENT} --authorized-keys %u %U",
            "AuthorizedKeysCommandUser nobody",
            "StrictModes yes",
            "AllowTcpForwarding local",
            "AllowStreamLocalForwarding no",
            "AllowAgentForwarding no",
            "GatewayPorts no",
            "X11Forwarding no",
            "PermitTunnel no",
            "PermitUserRC no",
            "PermitUserEnvironment no",
            "UseDNS no",
            "LogLevel VERBOSE",
            "Subsystem sftp internal-sftp",
        ):
            self.assertIn(expected + "\n", default_cfg)
        self.assertNotIn("DisableForwarding", default_cfg)

        self.assertIn("ListenAddress 100.64.17.1:2222\n", pwd_cfg)
        self.assertIn("ListenAddress [fd7a:115c:a1e0::17]:2222\n", pwd_cfg)
        self.assertIn("Port 2222\n", pwd_cfg)
        self.assertIn("UsePAM yes\n", pwd_cfg)
        self.assertIn("PasswordAuthentication yes\n", pwd_cfg)
        self.assertIn("PermitRootLogin no\n", pwd_cfg)
        self.assertIn("PermitEmptyPasswords no\n", pwd_cfg)
        self.assertIn("KbdInteractiveAuthentication no\n", pwd_cfg)
        self.assertNotIn("AuthenticationMethods publickey", pwd_cfg)

        if os.access(mod.SSHD, os.X_OK):
            for name, text in (("default.conf", default_cfg), ("password.conf", pwd_cfg)):
                cfg_path = self.root / name
                cfg_path.write_text(text, encoding="utf-8")
                res = subprocess.run([mod.SSHD, "-t", "-f", str(cfg_path)],
                                     capture_output=True, text=True, timeout=10)
                self.assertEqual(res.returncode, 0, f"sshd -t failed for {name}: {res.stderr}")

    def test_addresses_filtering(self):
        links = [
            {
                "ifname": "lo",
                "flags": ["LOOPBACK", "UP", "LOWER_UP"],
                "addr_info": [
                    {"local": "127.0.0.1", "scope": "host"},
                    {"local": "127.0.0.2", "scope": "global"},
                ],
            },
            {
                "ifname": "tailscale0",
                "flags": ["POINTOPOINT", "UP", "LOWER_UP"],
                "addr_info": [
                    {"local": "100.64.17.1", "scope": "global"},
                    {"local": "fd7a:115c:a1e0::17", "scope": "global"},
                    {"local": "fd7a:115c:a1e0::99", "scope": "global", "tentative": True},
                    {"local": "203.0.113.1", "scope": "global"},
                    {"local": "fe80::1", "scope": "link"},
                ],
            },
            {
                "ifname": "eth0",
                "flags": ["BROADCAST", "UP", "LOWER_UP"],
                "addr_info": [
                    {"local": "192.168.1.20", "scope": "global"},
                    {"local": "2001:db8::20", "scope": "global"},
                    {"local": "2001:db8::21", "scope": "global", "flags": ["dadfailed"]},
                    {"local": "fe80::20", "scope": "link"},
                    {"local": "fe80::21", "scope": "global"},
                    {"local": "224.0.0.1", "scope": "global"},
                    {"local": "0.0.0.0", "scope": "global"},
                ],
            },
            {
                "ifname": "wlan0",
                "flags": ["BROADCAST", "UP", "LOWER_UP"],
                "addr_info": [
                    {"local": "10.0.0.5", "scope": "global"},
                ],
            },
            {
                "ifname": "eth1",
                "flags": ["BROADCAST"],  # DOWN
                "addr_info": [
                    {"local": "192.168.2.20", "scope": "global"},
                ],
            },
            {
                "ifname": "mihomo",
                "flags": ["POINTOPOINT", "UP", "LOWER_UP"],
                "addr_info": [
                    {"local": "198.18.0.1", "scope": "global"},
                ],
            },
            {
                "ifname": "tb0",
                "flags": ["BROADCAST", "UP", "LOWER_UP"],
                "addr_info": [
                    {"local": "192.168.99.1", "scope": "global"},
                    {"local": "fe80::99", "scope": "link"},
                ],
            },
        ]
        drivers = {"tb0": "thunderbolt-net", "eth0": "e1000e", "wlan0": "iwlwifi"}
        fake_ip = types.SimpleNamespace(stdout=json.dumps(links))

        with patch.object(mod, "checked", return_value=fake_ip), \
                patch.object(mod, "iface_driver", side_effect=lambda name: drivers.get(name, "")):
            ts_only = mod.addresses("tailscale")
            lan_and_ts = mod.addresses("lan")

        self.assertEqual(ts_only, (
            ("tailscale0", ipaddress.ip_address("100.64.17.1")),
            ("tailscale0", ipaddress.ip_address("fd7a:115c:a1e0::17")),
        ))
        self.assertEqual(lan_and_ts, (
            ("eth0", ipaddress.ip_address("192.168.1.20")),
            ("eth0", ipaddress.ip_address("2001:db8::20")),
            ("tailscale0", ipaddress.ip_address("100.64.17.1")),
            ("tailscale0", ipaddress.ip_address("fd7a:115c:a1e0::17")),
            ("wlan0", ipaddress.ip_address("10.0.0.5")),
        ))

    def test_firewall_rules_and_filter_sync(self):
        ts_bound = (
            ("tailscale0", ipaddress.ip_address("100.64.17.1")),
            ("tailscale0", ipaddress.ip_address("fd7a:115c:a1e0::17")),
        )
        lan_bound = (
            ("eth0", ipaddress.ip_address("192.168.1.20")),
            ("eth0", ipaddress.ip_address("2001:db8::20")),
            ("tailscale0", ipaddress.ip_address("100.64.17.1")),
        )

        self.assertEqual(mod.firewall_rules((), 22), "destroy table inet locachy-tailnet-ssh\n")
        self.assertEqual(
            mod.filter_sync_rules((), 22),
            "flush set inet locachy-filter ssh_lan_v4\nflush set inet locachy-filter ssh_lan_v6\n",
        )

        ts_fw = mod.firewall_rules(ts_bound, 22)
        self.assertIn('iifname != "tailscale0" ip daddr 100.64.17.1 tcp dport 22 drop\n', ts_fw)
        self.assertIn('iifname != "tailscale0" ip6 daddr fd7a:115c:a1e0::17 tcp dport 22 drop\n', ts_fw)
        ts_sync = mod.filter_sync_rules(ts_bound, 22)
        self.assertNotIn("add element", ts_sync)

        lan_fw = mod.firewall_rules(lan_bound, 2222)
        self.assertIn('iifname != "eth0" ip daddr 192.168.1.20 tcp dport 2222 drop\n', lan_fw)
        self.assertIn('iifname != "eth0" ip6 daddr 2001:db8::20 tcp dport 2222 drop\n', lan_fw)
        self.assertIn('iifname != "tailscale0" ip daddr 100.64.17.1 tcp dport 2222 drop\n', lan_fw)
        lan_sync = mod.filter_sync_rules(lan_bound, 2222)
        self.assertIn("flush set inet locachy-filter ssh_lan_v4\n", lan_sync)
        self.assertIn("flush set inet locachy-filter ssh_lan_v6\n", lan_sync)
        self.assertIn('add element inet locachy-filter ssh_lan_v4 { "eth0" . 192.168.1.20 . 2222 }\n', lan_sync)
        self.assertIn('add element inet locachy-filter ssh_lan_v6 { "eth0" . 2001:db8::20 . 2222 }\n', lan_sync)
        self.assertNotIn("tailscale0", lan_sync)

        calls = []
        with patch.object(mod, "checked", side_effect=lambda *a, **kw: calls.append((a, kw.get("input")))), \
                patch.object(mod, "_filter_sets_exist", return_value=False):
            mod.firewall(lan_bound, 2222)
        self.assertEqual(len(calls), 1)

        calls.clear()
        with patch.object(mod, "checked", side_effect=lambda *a, **kw: calls.append((a, kw.get("input")))), \
                patch.object(mod, "_filter_sets_exist", return_value=True):
            mod.firewall(lan_bound, 2222)
        self.assertEqual(len(calls), 2)
        self.assertIn("ssh_lan_v4", calls[1][1])

    def test_authorized_keys_crud_and_rejections(self):
        base = self.root / "tailnet-ssh"
        keys = base / "authorized_keys"
        _, pub1 = self._gen_keypair("k1", comment="alice@laptop")
        _, pub2 = self._gen_keypair("k2", comment="alice@workstation")

        with patch.object(mod, "BASE", base), \
                patch.object(mod, "KEYS", keys), \
                patch.object(mod, "owned", side_effect=fake_owned):
            self.assertEqual(mod.keys_list("alice"), [])

            fp1 = mod.keys_add("alice", pub1 + "\n")
            self.assertTrue(fp1.startswith("SHA256:"))
            self.assertEqual((keys / "alice").stat().st_mode & 0o777, 0o644)

            # Duplicate key body (even with another comment) is a no-op.
            parts = pub1.split()
            fp1_dup = mod.keys_add("alice", f"{parts[0]} {parts[1]} renamed@host")
            self.assertEqual(fp1_dup, fp1)
            entries = mod.keys_list("alice")
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["fingerprint"], fp1)
            self.assertEqual(entries[0]["comment"], "alice@laptop")

            fp2 = mod.keys_add("alice", pub2 + "\r\n")
            self.assertTrue(fp2.startswith("SHA256:"))
            self.assertNotEqual(fp1, fp2)
            entries = mod.keys_list("alice")
            self.assertEqual([e["fingerprint"] for e in entries], [fp1, fp2])

            for bad_user in ("", "root", ".hidden", "../bob", "a/b", "Alice", "a" * 33):
                with self.assertRaises(ValueError):
                    mod.keys_add(bad_user, pub1)
                with self.assertRaises(ValueError):
                    mod.keys_list(bad_user)

            for bad_key in (
                "",
                "# comment only",
                pub1 + "\n" + pub2,
                pub1 + "\x00",
                "not-a-valid-key AAAAB3Nza",
                'command="/bin/sh" ' + pub1,
            ):
                with self.assertRaises((ValueError, subprocess.SubprocessError)):
                    mod.keys_add("alice", bad_key)

            with self.assertRaises(ValueError):
                mod.keys_remove("alice", 0)
            with self.assertRaises(ValueError):
                mod.keys_remove("alice", 3)

            mod.keys_remove("alice", 1)
            entries = mod.keys_list("alice")
            self.assertEqual([e["fingerprint"] for e in entries], [fp2])

            mod.keys_remove("alice", 1)
            self.assertFalse((keys / "alice").exists())
            self.assertEqual(mod.keys_list("alice"), [])

            # Symlinked authorized_keys target must be rejected.
            target = self.root / "elsewhere"
            target.write_text(pub1 + "\n", encoding="utf-8")
            (keys / "alice").symlink_to(target)
            with self.assertRaises(OSError):
                mod.keys_list("alice")
            with self.assertRaises(OSError):
                mod.keys_add("alice", pub1)
            with self.assertRaises(RuntimeError):
                mod._write_user_keys_atomic("alice", [pub1])

    def test_host_key_fingerprint_and_regen(self):
        base = self.root / "ssh-host"
        keys = base / "authorized_keys"
        host_key = base / "ssh_host_ed25519_key"
        host_pub = Path(str(host_key) + ".pub")

        with patch.object(mod, "BASE", base), \
                patch.object(mod, "KEYS", keys), \
                patch.object(mod, "HOST_KEY", host_key), \
                patch.object(mod, "HOST_PUBLIC_KEY", host_pub), \
                patch.object(mod, "owned", side_effect=fake_owned):
            self.assertEqual(mod.host_key_fingerprint(), "")

            mod.ensure_host_key()
            self.assertTrue(host_key.is_file())
            self.assertEqual(host_key.stat().st_mode & 0o777, 0o600)
            fp1 = mod.host_key_fingerprint()
            self.assertTrue(fp1.startswith("SHA256:"))

            host_pub.write_text("stale pub\n", encoding="utf-8")
            with patch.object(mod, "signal_reload") as reload_mock:
                fp2 = mod.host_key_regen(notify=True)
                reload_mock.assert_called_once()
            self.assertTrue(fp2.startswith("SHA256:"))
            self.assertNotEqual(fp1, fp2)
            self.assertEqual(host_key.stat().st_mode & 0o777, 0o600)
            self.assertFalse(host_pub.exists())
            self.assertEqual(mod.host_key_fingerprint(), fp2)

    def test_signal_reload_handles_missing_and_stale_pidfile(self):
        run_dir = self.root / "run"
        run_dir.mkdir(mode=0o700)
        pidfile = run_dir / "supervisor.pid"
        with patch.object(mod, "RUN", run_dir), \
                patch.object(mod, "PIDFILE", pidfile), \
                patch.object(mod, "owned", side_effect=fake_owned):
            self.assertFalse(mod.signal_reload())
            pidfile.write_text("not-a-pid\n", encoding="utf-8")
            pidfile.chmod(0o600)
            self.assertFalse(mod.signal_reload())
            pidfile.write_text(f"{os.getpid()}\n", encoding="utf-8")
            with patch.object(mod, "_pid_is_supervisor", return_value=False):
                self.assertFalse(mod.signal_reload())
            with patch.object(mod, "_pid_is_supervisor", return_value=True), \
                    patch.object(mod.os, "kill") as kill_mock:
                self.assertTrue(mod.signal_reload())
                kill_mock.assert_called_once_with(os.getpid(), mod.signal.SIGHUP)

    def test_keys_pending_staging_dedup_and_safety(self):
        base = self.root / "pending-base"
        keys = base / "authorized_keys"
        home = self.root / "home-alice"
        ssh_dir = home / ".ssh"
        ssh_dir.mkdir(parents=True, mode=0o700)
        _, pub1 = self._gen_keypair("p1", comment="alice@existing")
        _, pub2 = self._gen_keypair("p2", comment="alice@new-laptop")
        _, pub3 = self._gen_keypair("p3", comment="alice\twith\ttabs")
        fake_pw = types.SimpleNamespace(pw_dir=str(home), pw_uid=os.geteuid())

        with patch.object(mod, "BASE", base), \
                patch.object(mod, "KEYS", keys), \
                patch.object(mod, "owned", side_effect=fake_owned), \
                patch.object(mod.pwd, "getpwnam", return_value=fake_pw):
            # Missing ~/.ssh/authorized_keys returns empty cleanly.
            self.assertEqual(mod.keys_pending("alice"), [])

            # Enroll pub1 as already authorized.
            fp1 = mod.keys_add("alice", pub1)
            self.assertTrue(fp1.startswith("SHA256:"))

            # Stage existing key, duplicate pending key, options-bearing line,
            # malformed line, comment, and a key with tabs in comment.
            auth_file = ssh_dir / "authorized_keys"
            auth_file.write_text(
                "# staged by ssh-copy-id\n"
                "\n"
                + pub1 + "\n"
                + 'command="/bin/sh" ' + pub2 + "\n"
                + "not-a-key garbage\n"
                + pub2 + "\n"
                + pub2 + " duplicate-comment\n"
                + pub3 + "\n",
                encoding="utf-8",
            )
            auth_file.chmod(0o600)

            pending = mod.keys_pending("alice")
            self.assertEqual(len(pending), 2)
            self.assertEqual(pending[0]["comment"], "alice@new-laptop")
            self.assertTrue(pending[0]["fingerprint"].startswith("SHA256:"))
            self.assertNotIn("\t", pending[1]["comment"])
            self.assertNotIn("\t", pending[1]["line"])
            self.assertEqual(pending[1]["comment"], "alice with tabs")
            # Authorized store must remain untouched (still only pub1).
            self.assertEqual([e["fingerprint"] for e in mod.keys_list("alice")], [fp1])

            # World-writable authorized_keys must be ignored.
            auth_file.chmod(0o666)
            self.assertEqual(mod.keys_pending("alice"), [])
            auth_file.chmod(0o600)

            # Oversized (> 64 KiB) authorized_keys must be ignored.
            auth_file.write_text((pub2 + "\n") * 2000, encoding="utf-8")
            self.assertGreater(auth_file.stat().st_size, mod.PENDING_MAX_BYTES)
            self.assertEqual(mod.keys_pending("alice"), [])

            # Symlinked authorized_keys must be rejected (O_NOFOLLOW).
            target = self.root / "real_keys"
            target.write_text(pub2 + "\n", encoding="utf-8")
            auth_file.unlink()
            auth_file.symlink_to(target)
            self.assertEqual(mod.keys_pending("alice"), [])

            # Symlinked .ssh directory must be rejected (O_DIRECTORY|O_NOFOLLOW).
            real_ssh = self.root / "real_ssh"
            real_ssh.mkdir(mode=0o700)
            (real_ssh / "authorized_keys").write_text(pub2 + "\n", encoding="utf-8")
            auth_file.unlink()
            ssh_dir.rmdir()
            ssh_dir.symlink_to(real_ssh)
            self.assertEqual(mod.keys_pending("alice"), [])

            # Unknown user returns empty; invalid user name raises ValueError.
            with patch.object(mod.pwd, "getpwnam", side_effect=KeyError("nobody")):
                self.assertEqual(mod.keys_pending("ghost"), [])
            with self.assertRaises(ValueError):
                mod.keys_pending("root")

    def test_keys_add_file_import_and_rejections(self):
        base = self.root / "file-import-base"
        keys = base / "authorized_keys"
        _, pub1 = self._gen_keypair("f1", comment="alice@usb")
        _, pub2 = self._gen_keypair("f2", comment="alice@second")
        pub_file = self.root / "operator.pub"
        pub_file.write_text(f"# header\n{pub1}\n", encoding="utf-8")

        with patch.object(mod, "BASE", base), \
                patch.object(mod, "KEYS", keys), \
                patch.object(mod, "owned", side_effect=fake_owned):
            fp1 = mod.keys_add_file("alice", str(pub_file))
            self.assertTrue(fp1.startswith("SHA256:"))
            self.assertEqual(len(mod.keys_list("alice")), 1)

            # Multi-key file is refused.
            multi = self.root / "multi.pub"
            multi.write_text(f"{pub1}\n{pub2}\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                mod.keys_add_file("alice", str(multi))

            # Symlinked .pub file is refused (O_NOFOLLOW).
            link = self.root / "link.pub"
            link.symlink_to(pub_file)
            with self.assertRaises(OSError):
                mod.keys_add_file("alice", str(link))

    def test_enroll_filter_rules_and_url_selection(self):
        self.assertEqual(
            mod.enroll_filter_rules(()),
            "flush set inet locachy-filter ssh_enroll_v4\n"
            "flush set inet locachy-filter ssh_enroll_v6\n",
        )
        bound = (
            ("tailscale0", ipaddress.ip_address("100.64.17.1"), 41234),
            ("eth0", ipaddress.ip_address("192.168.1.20"), 41235),
            ("eth0", ipaddress.ip_address("2001:db8::20"), 41236),
            ("lo", ipaddress.ip_address("127.0.0.1"), 41237),
        )
        rules = mod.enroll_filter_rules(bound)
        self.assertIn('add element inet locachy-filter ssh_enroll_v4 { "eth0" . 192.168.1.20 . 41235 }\n', rules)
        self.assertIn('add element inet locachy-filter ssh_enroll_v6 { "eth0" . 2001:db8::20 . 41236 }\n', rules)
        self.assertNotIn("tailscale0", rules)
        self.assertNotIn('"lo"', rules)

        url = mod._select_receive_url([(i, a, p, None) for i, a, p in bound], "ABCDEF")
        self.assertEqual(url, "http://100.64.17.1:41234/ABCDEF")
        lan_only = [
            ("eth0", ipaddress.ip_address("2001:db8::20"), 41236, None),
            ("eth0", ipaddress.ip_address("192.168.1.20"), 41235, None),
        ]
        self.assertEqual(mod._select_receive_url(lan_only, "ABCDEF"), "http://192.168.1.20:41235/ABCDEF")
        v6_only = [("eth0", ipaddress.ip_address("2001:db8::20"), 41236, None)]
        self.assertEqual(mod._select_receive_url(v6_only, "ABCDEF"), "http://[2001:db8::20]:41236/ABCDEF")

    def _raw_http(self, host, port, req_bytes, body_after_100=None):
        with socket.create_connection((host, port), timeout=5) as conn:
            conn.settimeout(5)
            conn.sendall(req_bytes)
            chunks = []
            if body_after_100 is not None:
                prelim = b""
                while b"\r\n\r\n" not in prelim:
                    data = conn.recv(4096)
                    if not data:
                        break
                    prelim += data
                self.assertTrue(prelim.startswith(b"HTTP/1.1 100 Continue\r\n\r\n"), prelim)
                rest = prelim.split(b"\r\n\r\n", 1)[1]
                if rest:
                    chunks.append(rest)
                conn.sendall(body_after_100)
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                chunks.append(data)
            return b"".join(chunks).decode("utf-8", errors="replace")

    def _spawn_receive(self, base, rc_path, timeout=15):
        env = os.environ.copy()
        env["LOCACHY_SSH_BASE"] = str(base)
        env["LOCACHY_SSH_RC"] = str(rc_path)
        env["LOCACHY_SSH_NO_NETNS"] = "1"
        env["LOCACHY_SSH_BIND_ADDR"] = "127.0.0.1"
        proc = subprocess.Popen(
            [sys.executable, "-I", str(SOURCE), "--receive-key", "alice", "--timeout", str(timeout)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        ready = proc.stdout.readline().strip()
        return proc, ready

    def test_receive_key_http_flow_rejections_and_no_autowrite(self):
        base = self.root / "recv-base"
        keys = base / "authorized_keys"
        rc_path = self.root / "recv-rc.conf"
        rc_path.write_text('ssh_listen="tailscale"\n', encoding="utf-8")
        _, pub = self._gen_keypair("r1", comment="macbook@studio")

        proc, ready = self._spawn_receive(base, rc_path, timeout=15)
        try:
            parts = ready.split("\t")
            self.assertEqual(len(parts), 3, f"unexpected READY line: {ready!r}")
            self.assertEqual(parts[0], "READY")
            self.assertEqual(parts[2], "15")
            parsed_url = urlparse(parts[1])
            self.assertEqual(parsed_url.scheme, "http")
            self.assertEqual(parsed_url.hostname, "127.0.0.1")
            self.assertTrue(parsed_url.port and parsed_url.port > 0)
            token = parsed_url.path.lstrip("/")
            self.assertEqual(len(token), mod.RECEIVE_TOKEN_LEN)
            for ch in token:
                self.assertIn(ch, mod.RECEIVE_TOKEN_ALPHABET)

            host, port = parsed_url.hostname, parsed_url.port

            # 1. Wrong token rejected with 403.
            bad_tok = self._raw_http(
                host, port,
                f"PUT /WRONG1 HTTP/1.1\r\nHost: {host}:{port}\r\nContent-Length: {len(pub)}\r\n\r\n{pub}".encode("utf-8"),
            )
            self.assertTrue(bad_tok.startswith("HTTP/1.1 403 "), bad_tok)

            # 2. Oversized body (> 4096) rejected with 413.
            big_len = mod.RECEIVE_MAX_BODY + 1
            oversized = self._raw_http(
                host, port,
                f"PUT /{token} HTTP/1.1\r\nHost: {host}:{port}\r\nContent-Length: {big_len}\r\n\r\n".encode("ascii"),
            )
            self.assertTrue(oversized.startswith("HTTP/1.1 413 "), oversized)

            # 3. Chunked transfer encoding rejected with 411.
            chunked = self._raw_http(
                host, port,
                f"PUT /{token} HTTP/1.1\r\nHost: {host}:{port}\r\nTransfer-Encoding: chunked\r\n\r\n".encode("ascii"),
            )
            self.assertTrue(chunked.startswith("HTTP/1.1 411 "), chunked)

            # 4. Invalid public key rejected with 400.
            bad_body = b"not-a-valid-ssh-key\n"
            invalid = self._raw_http(
                host, port,
                f"PUT /{token} HTTP/1.1\r\nHost: {host}:{port}\r\nContent-Length: {len(bad_body)}\r\n\r\n".encode("ascii") + bad_body,
            )
            self.assertTrue(invalid.startswith("HTTP/1.1 400 "), invalid)

            # 5. Valid curl-style PUT with Expect: 100-continue succeeds with 200 OK.
            good_body = (pub + "\n").encode("utf-8")
            ok_resp = self._raw_http(
                host, port,
                (
                    f"PUT /{token} HTTP/1.1\r\n"
                    f"Host: {host}:{port}\r\n"
                    f"Expect: 100-continue\r\n"
                    f"Content-Length: {len(good_body)}\r\n\r\n"
                ).encode("ascii"),
                body_after_100=good_body,
            )
            self.assertTrue(ok_resp.startswith("HTTP/1.1 200 OK\r\n"), ok_resp)
            self.assertIn("SHA256:", ok_resp)

            key_line = proc.stdout.readline().strip()
            rc_code = proc.wait(timeout=5)
            self.assertEqual(rc_code, 0, proc.stderr.read())
            kparts = key_line.split("\t")
            self.assertEqual(len(kparts), 6, f"unexpected KEY line: {key_line!r}")
            self.assertEqual(kparts[0], "KEY")
            self.assertEqual(kparts[1], "127.0.0.1")
            self.assertEqual(kparts[2], "ssh-ed25519")
            self.assertTrue(kparts[3].startswith("SHA256:"))
            self.assertIn(kparts[3], ok_resp)
            self.assertEqual(kparts[4], "macbook@studio")
            self.assertEqual(kparts[5], pub)

            # Receiving a key must NEVER auto-write authorized_keys.
            self.assertFalse((keys / "alice").exists())
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)

    def test_receive_key_timeout_and_sigterm(self):
        base = self.root / "recv-timeout-base"
        rc_path = self.root / "recv-timeout-rc.conf"
        rc_path.write_text('ssh_listen="tailscale"\n', encoding="utf-8")

        proc, ready = self._spawn_receive(base, rc_path, timeout=1)
        try:
            self.assertTrue(ready.startswith("READY\thttp://127.0.0.1:"), ready)
            rc_code = proc.wait(timeout=5)
            self.assertEqual(rc_code, 124)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)

        proc2, ready2 = self._spawn_receive(base, rc_path, timeout=30)
        try:
            self.assertTrue(ready2.startswith("READY\thttp://127.0.0.1:"), ready2)
            proc2.send_signal(signal.SIGTERM)
            rc_code2 = proc2.wait(timeout=5)
            self.assertNotEqual(rc_code2, 0)
        finally:
            if proc2.poll() is None:
                proc2.kill()
                proc2.wait(timeout=5)


if __name__ == "__main__":
    if not shutil.which("ssh-keygen"):
        print("SKIP: ssh-keygen not installed")
        sys.exit(0)
    unittest.main(verbosity=1)
