#!/usr/bin/env python3
"""Device-free unit tests for managed OpenSSH server support in tailnet-ssh-client."""
import importlib.machinery
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

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


if __name__ == "__main__":
    if not shutil.which("ssh-keygen"):
        print("SKIP: ssh-keygen not installed")
        sys.exit(0)
    unittest.main(verbosity=1)
