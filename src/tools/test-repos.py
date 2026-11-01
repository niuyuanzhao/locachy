#!/usr/bin/env python3
"""locachy-repos without the network: the stack for each level and CPU, the
sections, a refused or allowed switch (the resolver stubbed), rc.conf as
base/rc.c writes it, and ranked lists replacing the old ones only with mirrors.
  python3 -B tools/test-repos.py [SCRATCH_PARENT]
"""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("repos", str(SOURCE / "link/locachy-repos"))
spec = importlib.util.spec_from_loader(loader.name, loader)
r = importlib.util.module_from_spec(spec)
loader.exec_module(r)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
ALL = list(r.SECTIONS)


def names(level, cpu, secs=ALL):
    return [name for name, _ in r.stack(level, cpu, secs)]


class Stack(unittest.TestCase):
    def test_baseline_is_cachyos_then_arch(self):
        self.assertEqual(names("x86-64", "x86-64"), ["cachyos", "core", "extra", "multilib"])
        self.assertEqual(r.architectures("x86-64"), "x86_64")

    def test_v2_puts_alhp_v2_over_cachyos_and_arch(self):
        self.assertEqual(names("x86-64-v2", "ivybridge"),
                         ["core-x86-64-v2", "extra-x86-64-v2", "multilib-x86-64-v2",
                          "cachyos", "core", "extra", "multilib"])
        self.assertEqual(r.architectures("x86-64-v2"), "x86_64")

    def test_v3_cachyos_v3_then_alhp_v3_v2(self):
        self.assertEqual(names("x86-64-v3", "skylake"),
                         ["cachyos-v3", "cachyos-core-v3", "cachyos-extra-v3",
                          "core-x86-64-v3", "extra-x86-64-v3", "multilib-x86-64-v3",
                          "core-x86-64-v2", "extra-x86-64-v2", "multilib-x86-64-v2",
                          "cachyos", "core", "extra", "multilib"])
        self.assertEqual(r.architectures("x86-64-v3"), "x86_64 x86_64_v3")
        self.assertEqual(r.cachyos_mirror_levels("x86-64-v3", "skylake"), ["v3"])

    def test_v4_zen4_first_each_level_falls_back_to_the_next(self):
        stack = r.stack("x86-64-v4", "znver4", ALL)
        self.assertEqual([n for n, _ in stack][:9],
                         ["cachyos-znver4", "cachyos-core-znver4", "cachyos-extra-znver4",
                          "cachyos-v4", "cachyos-core-v4", "cachyos-extra-v4",
                          "cachyos-v3", "cachyos-core-v3", "cachyos-extra-v3"])
        self.assertEqual(dict(stack)["cachyos-znver4"], "cachyos-v4-mirrorlist")
        alhp = [n for n, f in stack if f == "alhp-mirrorlist"]
        self.assertEqual([n.split("-", 1)[1] for n in alhp[::3]], ["x86-64-v4", "x86-64-v3", "x86-64-v2"])
        self.assertEqual([n for n, _ in stack][-4:], ["cachyos", "core", "extra", "multilib"])
        self.assertNotIn("cachyos-znver4", names("x86-64-v4", "sapphirerapids"))
        self.assertNotIn("cachyos-znver4", names("x86-64-v3", "znver4"))
        self.assertEqual(r.cachyos_mirror_levels("x86-64-v4", "znver5"), ["v3", "v4"])

    def test_a_section_off_goes_from_every_layer(self):
        stack = names("x86-64-v3", "x86-64-v3", ["core", "multilib"])
        self.assertFalse([n for n in stack if "extra" in n])
        self.assertIn("cachyos-core-v3", stack)
        self.assertIn("multilib-x86-64-v2", stack)
        self.assertEqual(r.sections({"repo_extra": "0"}), ["core", "multilib"])
        self.assertEqual(r.sections({"repo_core": "1"}), ALL)

    def test_config_includes_each_list(self):
        text = r.config("x86-64-v3", "x86-64-v3", "/m", {"repo_multilib": "0"})
        self.assertIn("Architecture = x86_64 x86_64_v3\n", text)
        self.assertIn("[cachyos-core-v3]\nInclude = /m/cachyos-v3-mirrorlist\n", text)
        self.assertIn("[extra-x86-64-v2]\nInclude = /m/alhp-mirrorlist\n", text)
        self.assertIn("[core]\nInclude = /m/mirrorlist\n", text)
        self.assertNotIn("multilib", text.split("[options]", 1)[1])
        with self.assertRaises(ValueError):
            r.config("x86-64-v5", "x", "/m", {})


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="repos-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name)
        self.rc = self.d / "rc.conf"
        self.rc.write_text('# keep\nhostname="x"\nrepo_multilib="0"\n')
        self.conf = self.d / "pacman.conf"
        for name, value in (("RC_CONF", str(self.rc)), ("PACMAN_CONF", str(self.conf))):
            p = mock.patch.object(r, name, value)
            p.start()
            self.addCleanup(p.stop)
        for name, value in (("system", lambda: ("x86-64-v2", "ivybridge")),
                            ("declared", lambda: ["base", "steam", "wine"])):
            p = mock.patch.object(r, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.conf.write_text(r.config("x86-64-v2", "ivybridge", "/etc/pacman.d", r.read_conf(str(self.rc))))

    def apply(self, *sets, have=None):
        """apply --set SETS, the repositories having for each conf what have says."""
        def resolvable(conf, leaves, dbpath):
            return have(conf) if have else set(leaves)
        args = type("Args", (), {"set": list(sets), "check_only": False})()
        with mock.patch.object(r, "resolvable", resolvable), mock.patch("sys.stdout"):
            return r.cmd_apply(args)

    def test_conf_set_as_base_rc(self):
        r.conf_set(str(self.rc), "repo_extra", "0")
        r.conf_set(str(self.rc), "repo_multilib", None)
        r.conf_set(str(self.rc), "hostname", "y")
        self.assertEqual(self.rc.read_text(), '# keep\nhostname="y"\nrepo_extra="0"\n')

    def test_turning_on_writes_conf_then_rc(self):
        self.assertEqual(self.apply("multilib=on"), 0)
        self.assertIn("[multilib-x86-64-v2]", self.conf.read_text())
        self.assertNotIn("repo_multilib", self.rc.read_text())
        self.assertNotIn("[multilib-x86-64-v2]", (self.d / "pacman.conf.locachy-old").read_text())

    def test_turning_off_what_declared_packages_need_is_refused(self):
        r.conf_set(str(self.rc), "repo_multilib", None)
        self.conf.write_text(r.config("x86-64-v2", "ivybridge", "/etc/pacman.d", r.read_conf(str(self.rc))))
        before_conf, before_rc = self.conf.read_text(), self.rc.read_text()
        have = lambda conf: {"base"} | ({"steam", "wine"} if "[multilib]" in conf else set())
        with mock.patch("sys.stderr") as err:
            self.assertEqual(self.apply("multilib=off", have=have), 1)
        said = "".join(c.args[0] for c in err.write.call_args_list)
        self.assertIn("not changed", said)
        self.assertIn("steam wine", said)
        self.assertEqual((self.conf.read_text(), self.rc.read_text()), (before_conf, before_rc))

    def test_turning_off_what_nothing_needs_is_allowed(self):
        r.conf_set(str(self.rc), "repo_multilib", None)
        self.conf.write_text(r.config("x86-64-v2", "ivybridge", "/etc/pacman.d", r.read_conf(str(self.rc))))
        self.assertEqual(self.apply("multilib=off", have=lambda conf: {"base"}), 0)
        self.assertNotIn("multilib", self.conf.read_text().split("[options]", 1)[1])
        self.assertIn('repo_multilib="0"', self.rc.read_text())

    def test_bad_set_is_an_error(self):
        with self.assertRaises(ValueError):
            self.apply("testing=on")
        with self.assertRaises(ValueError):
            self.apply("extra=maybe")


class Rank(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="repos-rank-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name)
        for name in ("mirrorlist", "cachyos-mirrorlist", "cachyos-v3-mirrorlist", "alhp-mirrorlist"):
            (self.d / name).write_text("Server = https://old.example/$repo/os/$arch\n")
        self.calls = []
        self.rc = self.d / "rc.conf"
        self.rc.write_text("")
        p = mock.patch.object(r, "RC_CONF", str(self.rc))
        p.start()
        self.addCleanup(p.stop)

    def rank(self, outputs, country="CN"):
        def run_tool(cmd, env=None, timeout=600, stdin=None):
            self.calls.append((cmd, env, stdin))
            return outputs.get(cmd[0] if cmd[0] in ("reflector", "curl") else cmd[1], "")
        args = type("Args", (), {"country": country, "dir": str(self.d), "cachyos_levels": None})()
        with mock.patch.object(r, "run_tool", run_tool), mock.patch("sys.stdout"), mock.patch("sys.stderr"):
            return r.cmd_rank(args)

    def test_ranked_lists_replace_old_and_v3_is_made_from_cachyos(self):
        status = self.rank({"reflector": "Server = https://a.example/$repo/os/$arch\n",
                            "cachyos": "# rate-mirrors\nServer = https://c.example/repo/$arch/$repo\n",
                            "stdin": "Server = https://h.example/alhp/$repo/os/$arch/\n"})
        self.assertEqual(status, 0)
        self.assertIn("a.example", (self.d / "mirrorlist").read_text())
        self.assertIn("old.example", (self.d / "mirrorlist.locachy-old").read_text())
        self.assertIn("https://c.example/repo/$arch_v3/$repo", (self.d / "cachyos-v3-mirrorlist").read_text())
        self.assertFalse((self.d / "cachyos-v4-mirrorlist").exists())
        self.assertIn("h.example", (self.d / "alhp-mirrorlist").read_text())
        ranked = json.loads((self.d / ".locachy-ranked").read_text())
        self.assertEqual((ranked["country"], ranked["arch"], ranked["cachyos"], ranked["alhp"]), ("CN", True, True, True))
        reflector = next(c for c, _, _ in self.calls if c[0] == "reflector")
        self.assertEqual(reflector[reflector.index("--country") + 1], "CN")
        cachyos_env = next(e for c, e, _ in self.calls if c[:2] == ["rate-mirrors", "cachyos"])
        self.assertEqual(cachyos_env["RATE_MIRRORS_COUNTRY_NEIGHBORS_PER_COUNTRY"], "0")
        alhp_in = next(s for c, _, s in self.calls if c[:2] == ["rate-mirrors", "stdin"])
        self.assertIn("https://old.example/\n", alhp_in)
        self.assertTrue(alhp_in.startswith("https://cdn.alhp.dev/\n"))

    def test_country_from_geoip_unless_tor_or_clash_tun_is_on(self):
        geoip = {"curl": "<Response><CountryCode>DE</CountryCode></Response>"}
        self.rank(geoip, country=None)
        self.assertEqual(json.loads((self.d / ".locachy-ranked").read_text())["country"], "DE")
        self.assertEqual(self.calls[0][0][0], "curl")
        for rc in ('service_tor="/usr/local/lib/locachy/tor-client"\n',
                   'service_clash="/usr/local/lib/locachy/clash-client"\n'):
            self.rc.write_text(rc)
            (self.d / ".locachy-ranked").write_text('{"country": "CN"}\n')
            self.calls.clear()
            self.rank(geoip, country=None)
            self.assertFalse([c for c, _, _ in self.calls if c[0] == "curl"], rc)
            self.assertEqual(json.loads((self.d / ".locachy-ranked").read_text())["country"], "CN")
        # Clash with its port only: programs that do not ask go straight out.
        self.rc.write_text('service_clash="/usr/local/lib/locachy/clash-client"\nclash_mode="proxy"\n')
        self.calls.clear()
        self.rank(geoip, country=None)
        self.assertEqual(self.calls[0][0][0], "curl")

    def test_nothing_ranked_leaves_the_lists(self):
        self.assertEqual(self.rank({}), 1)
        for name in ("mirrorlist", "cachyos-mirrorlist", "cachyos-v3-mirrorlist", "alhp-mirrorlist"):
            self.assertIn("old.example", (self.d / name).read_text())
        self.assertFalse((self.d / "mirrorlist.locachy-old").exists())
        # Arch's list from anywhere when the country gave none.
        self.assertEqual(sum(1 for c, _, _ in self.calls if c[0] == "reflector"), 2)


if __name__ == "__main__":
    unittest.main()
