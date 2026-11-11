#!/usr/bin/env python3
"""locachy-repos without the network: the stack for each level and CPU, the
sections, a refused or allowed switch (the resolver stubbed), rc.conf as
base/rc.c writes it, and ranked lists replacing the old ones only with mirrors;
what it writes 0644 under any umask, and an earlier version's private files
made so again.
  python3 -B tools/test-repos.py [SCRATCH_PARENT]
"""
import datetime
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
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

    def test_written_files_are_0644_under_a_private_umask(self):
        # As a system job's step ran it before: the service's umask 077.
        old = os.umask(0o077)
        try:
            self.assertEqual(self.apply("multilib=on"), 0)
        finally:
            os.umask(old)
        for f in (self.conf, self.rc, self.d / "pacman.conf.locachy-old"):
            self.assertEqual(f.stat().st_mode & 0o777, 0o644, f)

    def test_fix_modes_repairs_what_an_earlier_version_left_private(self):
        lists = self.d / "pacman.d"
        lists.mkdir()
        private = [self.conf, self.rc, lists / "mirrorlist", lists / "cachyos-v3-mirrorlist",
                   lists / "alhp-mirrorlist.locachy-old"]
        for f in private[2:]:
            f.write_text("Server = https://old.example/$repo/os/$arch\n")
        other = lists / "custom-mirrorlist"  # not its file
        other.write_text("x\n")
        for f in private + [other]:
            f.chmod(0o600)
        with mock.patch("sys.stderr"):
            self.assertEqual(r.fix_modes(str(lists)), len(private))
            self.assertEqual(r.fix_modes(str(lists)), 0)
        for f in private:
            self.assertEqual(f.stat().st_mode & 0o777, 0o644, f)
        self.assertEqual(other.stat().st_mode & 0o777, 0o600)

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

    def rank(self, outputs, country="CN", due=False):
        """outputs: reflector, curl (geoip), api (CachyOS's mirror API), cachyos and
        stdin (rate-mirrors timing CachyOS's and ALHP's mirrors)."""
        def run_tool(cmd, env=None, timeout=600, stdin=None):
            self.calls.append((cmd, env, stdin))
            if cmd[0] == "curl":
                return outputs.get("api" if cmd[-1] in r.CACHYOS_API else "curl", "")
            if cmd[:2] == ["rate-mirrors", "stdin"]:
                return outputs.get("cachyos" if r.CACHYOS_TEST in cmd else "stdin", "")
            return outputs.get(cmd[0] if cmd[0] == "reflector" else cmd[1], "")
        args = type("Args", (), {"country": country, "dir": str(self.d), "cachyos_levels": None, "due": due})()
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
        cachyos_env = next(e for c, e, _ in self.calls if c[:2] == ["rate-mirrors", "stdin"] and r.CACHYOS_TEST in c)
        self.assertEqual(cachyos_env["RATE_MIRRORS_COUNTRY_NEIGHBORS_PER_COUNTRY"], "0")
        alhp_in = next(s for c, _, s in self.calls if c[:2] == ["rate-mirrors", "stdin"] and r.ALHP_TEST in c)
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
            self.assertFalse([c for c, _, _ in self.calls if c[0] == "curl" and "geoip" in c[-1]], rc)
            self.assertEqual(json.loads((self.d / ".locachy-ranked").read_text())["country"], "CN")
        # Clash with its port only: programs that do not ask go straight out.
        self.rc.write_text('service_clash="/usr/local/lib/locachy/clash-client"\nclash_mode="proxy"\n')
        self.calls.clear()
        self.rank(geoip, country=None)
        self.assertEqual(self.calls[0][0][0], "curl")

    def test_ranked_lists_are_0644_under_a_private_umask(self):
        old = os.umask(0o077)
        try:
            self.rank({"reflector": "Server = https://a.example/$repo/os/$arch\n",
                       "cachyos": "Server = https://c.example/repo/$arch/$repo\n",
                       "stdin": "Server = https://h.example/alhp/$repo/os/$arch/\n"})
        finally:
            os.umask(old)
        for name in ("mirrorlist", "cachyos-mirrorlist", "cachyos-v3-mirrorlist", "alhp-mirrorlist",
                     "mirrorlist.locachy-old", ".locachy-ranked"):
            self.assertEqual((self.d / name).stat().st_mode & 0o777, 0o644, name)

    API = json.dumps({"mirrors": [
        {"url": "https://a.example/repo/", "overall_status": "healthy", "out_of_date": False, "delay_seconds": 3600},
        {"url": "https://b.example/repo/", "overall_status": "healthy", "out_of_date": False, "delay_seconds": 0},
        {"url": "https://c.example/repo", "overall_status": "healthy", "out_of_date": False, "delay_seconds": 0},
        {"url": "https://d.example/repo/", "overall_status": "healthy", "out_of_date": False, "delay_seconds": 0},
        {"url": "https://lagging.example/repo/", "overall_status": "partial", "out_of_date": True,
         "delay_seconds": 7204},
        {"url": "https://broken.example/repo/", "overall_status": "error", "out_of_date": True, "delay_seconds": None},
        {"url": "https://behind.example/repo/", "overall_status": "healthy", "out_of_date": False,
         "delay_seconds": 20000},
        {"url": "http://plain.example/repo/", "overall_status": "healthy", "out_of_date": False, "delay_seconds": 0},
    ]})

    @staticmethod
    def timed(rows):
        """rate-mirrors' output for (base URL, MB/s) rows, in its speed order."""
        out = [f"# {i}. SpeedTestResult {{ speed: {mbs} MB/s; elapsed: 1s; connection_time: 9ms }} -> {url}"
               for i, (url, mbs) in enumerate(rows, 1)]
        return "\n".join(out + [f"Server = {url}$arch/$repo" for url, _ in rows]) + "\n"

    def test_cachyos_times_every_mirror_the_api_lists_healthy_and_in_sync(self):
        # rate-mirrors' own order, not by speed in the tail: d (1 MB/s) before c (2 MB/s).
        rows = [("https://lagging.example/repo/", 12.0), ("https://a.example/repo/", 10.0),
                ("https://b.example/repo/", 8.0), ("https://d.example/repo/", 1.0), ("https://c.example/repo/", 2.0)]
        self.assertEqual(self.rank({"reflector": "Server = https://a.example/$repo/os/$arch\n", "api": self.API,
                                    "cachyos": self.timed(rows),
                                    "stdin": "Server = https://h.example/alhp/$repo/os/$arch/\n"}), 0)
        timed = next(s for c, _, s in self.calls if c[:2] == ["rate-mirrors", "stdin"] and r.CACHYOS_TEST in c)
        # A mirror catching up (out of date, 7204 s behind) is timed; one whose checks all
        # fail (no delay), one too far behind, plain http and the present list are not.
        self.assertEqual(timed, "https://a.example/repo/\nhttps://b.example/repo/\nhttps://c.example/repo/\n"
                                "https://d.example/repo/\nhttps://lagging.example/repo/\n")
        # Every mirror that answered stays. In the window (at least half the fastest),
        # the newest data first: b and a, then the fastest one, which lags. The tail
        # follows by speed: c (2 MB/s) before d (1 MB/s), whatever rate-mirrors printed.
        self.assertEqual(r.servers((self.d / "cachyos-mirrorlist").read_text()),
                         ["Server = https://b.example/repo/$arch/$repo", "Server = https://a.example/repo/$arch/$repo",
                          "Server = https://lagging.example/repo/$arch/$repo",
                          "Server = https://c.example/repo/$arch/$repo", "Server = https://d.example/repo/$arch/$repo"])
        self.assertEqual(len(r.servers((self.d / "cachyos-v3-mirrorlist").read_text())), 5)
        self.assertIn("$arch_v3", (self.d / "cachyos-v3-mirrorlist").read_text())

    def test_without_the_api_the_present_cachyos_list_is_timed(self):
        (self.d / "cachyos-mirrorlist").write_text("Server = https://p.example/repo/$arch/$repo\n")
        self.rank({"cachyos": self.timed([("https://p.example/repo/", 5.0)])})
        timed = next(s for c, _, s in self.calls if c[:2] == ["rate-mirrors", "stdin"] and r.CACHYOS_TEST in c)
        self.assertEqual(timed, "https://p.example/repo/\n")

    def test_a_short_ranking_keeps_the_list_an_earlier_ranking_wrote(self):
        many = "".join(f"Server = https://c{i}.example/repo/$arch/$repo\n" for i in range(16))
        full = {"reflector": "Server = https://a.example/$repo/os/$arch\n", "cachyos": many,
                "stdin": "Server = https://h.example/alhp/$repo/os/$arch/\n"}
        self.assertEqual(self.rank(full), 0)
        # One round in which most mirrors timed out: 1 of 16 answered.
        short = dict(full, cachyos="Server = https://c0.example/repo/$arch/$repo\n")
        self.assertEqual(self.rank(short), 0)
        for name in ("cachyos-mirrorlist", "cachyos-v3-mirrorlist"):
            self.assertEqual(len(r.servers((self.d / name).read_text())), 16, name)
        self.assertTrue(json.loads((self.d / ".locachy-ranked").read_text())["cachyos"])
        # Half as many or more is a ranking: it replaces the list.
        half = dict(full, cachyos="".join(f"Server = https://d{i}.example/repo/$arch/$repo\n" for i in range(8)))
        self.rank(half)
        text = (self.d / "cachyos-mirrorlist").read_text()
        self.assertEqual(len(r.servers(text)), 8)
        self.assertIn("d0.example", text)

    def test_a_packaged_list_is_replaced_by_a_short_ranking(self):
        (self.d / "cachyos-mirrorlist").write_text(
            "".join(f"Server = https://p{i}.example/repo/$arch/$repo\n" for i in range(40)))
        self.rank({"reflector": "Server = https://a.example/$repo/os/$arch\n",
                   "cachyos": "Server = https://c.example/repo/$arch/$repo\n",
                   "stdin": "Server = https://h.example/alhp/$repo/os/$arch/\n"})
        self.assertEqual(r.servers((self.d / "cachyos-mirrorlist").read_text()),
                         ["Server = https://c.example/repo/$arch/$repo"])

    def test_nothing_ranked_leaves_the_lists(self):
        self.assertEqual(self.rank({}), 1)
        for name in ("mirrorlist", "cachyos-mirrorlist", "cachyos-v3-mirrorlist", "alhp-mirrorlist"):
            self.assertIn("old.example", (self.d / name).read_text())
        self.assertFalse((self.d / "mirrorlist.locachy-old").exists())
        # Arch's list from anywhere when the country gave none.
        self.assertEqual(sum(1 for c, _, _ in self.calls if c[0] == "reflector"), 2)

    def test_due_ranks_only_once_the_interval_has_passed(self):
        lists = {"reflector": "Server = https://a.example/$repo/os/$arch\n",
                 "cachyos": "Server = https://c.example/repo/$arch/$repo\n",
                 "stdin": "Server = https://h.example/alhp/$repo/os/$arch/\n"}

        def ranked():
            return [c for c, _, _ in self.calls if c[0] == "reflector"]
        self.assertEqual(self.rank(lists, due=True), 0)  # never ranked: due
        self.assertTrue(ranked())
        self.calls.clear()
        self.rank(lists, due=True)  # ranked just now; once a day when unset
        self.assertFalse(ranked())
        record = json.loads((self.d / ".locachy-ranked").read_text())
        record["time"] -= 2 * 86400
        (self.d / ".locachy-ranked").write_text(json.dumps(record))
        self.rc.write_text('repos_rank_days="3"\n')
        self.rank(lists, due=True)  # two days old, every three days
        self.assertFalse(ranked())
        self.rc.write_text('repos_rank_days="1"\n')
        self.rank(lists, due=True)
        self.assertTrue(ranked())
        # A ranking that left a list without mirrors is due again at once.
        self.rank({"reflector": lists["reflector"]})
        self.calls.clear()
        self.rank(lists, due=True)
        self.assertTrue(ranked())
        # 0: only by hand.
        self.rc.write_text('repos_rank_days="0"\n')
        (self.d / ".locachy-ranked").unlink()
        self.calls.clear()
        self.assertEqual(self.rank(lists, due=True), 0)
        self.assertFalse(self.calls)


class RankTimer(unittest.TestCase):
    """The periodic ranking (rank-timer, rc.conf service_repos_rank): when it
    is due, the retry an hour after a failure, off at 0, at once without a
    record; the fixed job it submits, rc.conf and the record read each wake."""
    DAY = 86400

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="repos-timer-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name)
        self.rc = self.d / "rc.conf"
        self.rc.write_text('repos_rank_days="1"\n')
        for p in (mock.patch.object(r, "RC_CONF", str(self.rc)), mock.patch("sys.stderr")):
            p.start()
            self.addCleanup(p.stop)
        self.now = 1_800_000_000.0
        self.submitted = []
        self.outcome = None  # what the job does: None fails, else the record's lists

    def record(self, when, complete=True):
        (self.d / ".locachy-ranked").write_text(json.dumps(
            {"at": "x", "time": int(when), "arch": True, "cachyos": complete, "alhp": True}))

    def submit(self, kind, args, wait, startup_wait):
        self.submitted.append((kind, list(args), wait))
        if self.outcome is None:
            return 1
        self.record(self.now, self.outcome)
        return 0

    def step(self, failed_at=None):
        jobs = type("Jobs", (), {"submit": staticmethod(self.submit)})
        return r.rank_timer_step(jobs, str(self.d), failed_at, clock=lambda: self.now)

    def test_next_time_from_the_last_complete_ranking(self):
        t = self.now
        self.assertEqual(r.next_rank_time(1, t - 3600, None, t), t - 3600 + self.DAY)
        self.assertEqual(r.next_rank_time(0.5, t, None, t), t + self.DAY / 2)  # days may be fractions
        self.assertEqual(r.next_rank_time(7, t, None, t), t + 7 * self.DAY)

    def test_without_a_record_it_is_due_at_once(self):
        self.assertEqual(r.next_rank_time(1, None, None, self.now), self.now)
        self.assertIsNone(r.ranked_time(str(self.d)))
        self.record(self.now, complete=False)  # a list left without mirrors: none complete
        self.assertIsNone(r.ranked_time(str(self.d)))

    def test_zero_or_less_is_only_by_hand(self):
        for days in (0, -1, 0.0):
            self.assertIsNone(r.next_rank_time(days, None, None, self.now))
        self.assertEqual(r.rank_interval({"repos_rank_days": "0"}), 0)

    def test_a_failure_is_tried_again_an_hour_later(self):
        t = self.now
        self.assertEqual(r.next_rank_time(1, None, t, t + 60), t + 3600)
        self.assertEqual(r.next_rank_time(1, t - 2 * self.DAY, t, t + 60), t + 3600)
        # A ranking (by hand) after the failure counts, the failure not.
        self.assertEqual(r.next_rank_time(1, t + 600, t, t + 700), t + 600 + self.DAY)

    def test_the_interval_from_rc_conf(self):
        self.assertEqual(r.rank_interval({}), 1)
        self.assertEqual(r.rank_interval({"repos_rank_days": "2.5"}), 2.5)
        for bad in ("x", "", "nan"):
            self.assertEqual(r.rank_interval({"repos_rank_days": bad}), 1, bad)
        self.assertEqual(r.rank_interval({"repos_rank_days": "1e400"}), r.RANK_DAYS_MAX)

    def test_due_submits_the_fixed_job_and_a_failure_waits_an_hour(self):
        failed, wake = self.step()  # no record: at once
        self.assertEqual(self.submitted, [("repos-rank-due", [], True)])
        self.assertEqual((failed, wake), (self.now, self.now + 3600))
        self.now += 120  # woken early (rc.conf changed): still waits for the retry
        self.assertEqual(self.step(failed), (failed, failed + 3600))
        self.assertEqual(len(self.submitted), 1)
        self.now = failed + 3600
        self.outcome = True
        failed, wake = self.step(failed)
        self.assertEqual((failed, wake, len(self.submitted)), (None, self.now + self.DAY, 2))

    def test_a_job_done_without_a_complete_record_counts_as_failed(self):
        self.outcome = False
        failed, wake = self.step()
        self.assertEqual((failed, wake), (self.now, self.now + 3600))

    def test_rc_conf_and_the_record_are_read_at_each_wake(self):
        self.record(self.now - 3600)
        self.assertEqual(self.step(), (None, self.now - 3600 + self.DAY))
        self.rc.write_text('repos_rank_days="0.25"\n')  # shortened: the record is older than six hours
        self.record(self.now - self.DAY / 2)
        self.outcome = True
        self.assertEqual(self.step(), (None, self.now + self.DAY / 4))
        self.assertEqual(len(self.submitted), 1)
        self.rc.write_text('repos_rank_days="7"\n')  # longer: it sleeps at most a day, then reckons again
        self.assertEqual(self.step(), (None, self.now + r.RECHECK))
        self.now += 3 * self.DAY
        self.record(self.now - 60)  # ranked by hand (settings): the period counts from there
        self.assertEqual(self.step(), (None, self.now + r.RECHECK))
        self.rc.write_text('repos_rank_days="0"\n')
        (self.d / ".locachy-ranked").unlink()
        self.assertEqual(self.step(), (None, self.now + r.RECHECK))
        self.assertEqual(len(self.submitted), 1)

    def test_status_says_the_next_ranking(self):
        self.assertEqual(r.next_text(str(self.d), {"repos_rank_days": "0"}, self.now), "off")
        self.assertEqual(r.next_text(str(self.d), {}, self.now), "now")
        self.record(self.now)
        self.assertEqual(r.next_text(str(self.d), {"repos_rank_days": "2"}, self.now),
                         time.strftime("%Y-%m-%d %H:%M", time.localtime(self.now + 2 * self.DAY)))

    def test_rank_due_from_a_terminal_is_the_due_job(self):
        asked = []
        jobs = type("Jobs", (), {"execution_mode": staticmethod(lambda: "queue"),
                                 "submit": staticmethod(lambda kind, args, wait: asked.append(kind) or 0)})
        with mock.patch.object(r, "jobs_helper", lambda: jobs):
            for due in (True, False):
                args = type("Args", (), {"command": "rank", "country": None, "cachyos_levels": None, "due": due})()
                self.assertEqual(r.job_route(args), (0, "queue"))
        self.assertEqual(asked, ["repos-rank-due", "repos-rank"])

    @unittest.skipUnless(hasattr(os, "timerfd_create"), "timerfd: Linux, Python 3.13+")
    def test_a_change_in_a_watched_folder_ends_the_sleep(self):
        watch = r.change_watch([str(self.d)])
        self.assertIsNotNone(watch)
        self.addCleanup(os.close, watch)
        threading.Timer(0.2, lambda: (self.d / "rc.conf").write_text('repos_rank_days="2"\n')).start()
        start = time.monotonic()
        r.sleep_until(time.time() + 60, watch)
        self.assertLess(time.monotonic() - start, 10)
        start = time.monotonic()
        r.sleep_until(time.time() + 0.3, watch)  # its events were read: the time ends this one
        self.assertGreaterEqual(time.monotonic() - start, 0.2)


class AlhpOrder(unittest.TestCase):
    """The user's rule: the fastest mirror first, since one that lags catches
    up; among those at least half as fast as the fastest, the newest data."""
    RESULTS = ("#   1. SpeedTestResult { speed: 6.2 MB/s; elapsed: 191ms; connection_time: 464ms } -> https://a.example/alhp/\n"
               "#   2. SpeedTestResult { speed: 4.9 MB/s; elapsed: 236ms; connection_time: 124ms } -> https://b.example/alhp/\n"
               "#   3. SpeedTestResult { speed: 3.5 MB/s; elapsed: 285ms; connection_time: 304ms } -> https://c.example/alhp/\n"
               "#   4. SpeedTestResult { speed: 919.9 KB/s; elapsed: 970ms; connection_time: 2.22s } -> https://d.example/alhp/\n")
    LINES = [f"Server = https://{m}.example/alhp/$repo/os/$arch/" for m in "abcd"]

    def order(self, dates):
        when = {f"https://{m}.example/alhp/": datetime.datetime(*d, tzinfo=datetime.timezone.utc)
                for m, d in dates.items()}
        with mock.patch.object(r, "data_time", lambda mirror: when.get(mirror)):
            out = r.alhp_order(self.LINES, r.measured_speeds(self.RESULTS))
        return out, [line.split("//")[1].split(".")[0] for line in out if line.startswith("Server")]

    def test_speeds_are_read_from_the_numbered_results_only(self):
        speeds = r.measured_speeds("# SpeedTestResult { speed: 9.0 MB/s; elapsed: 1ms; connection_time: 1ms }\n"
                                   + self.RESULTS)
        self.assertEqual(len(speeds), 4)
        self.assertAlmostEqual(speeds["https://a.example/alhp/"], 6.2e6)
        self.assertAlmostEqual(speeds["https://d.example/alhp/"], 919.9e3)

    def test_within_half_the_fastest_speed_the_newest_data_goes_first(self):
        out, order = self.order({"a": (2026, 10, 3), "b": (2026, 11, 6), "d": (2026, 11, 10)})
        # c is in the window without a date: after a and b. d has the newest
        # data but is under half the fastest speed: it stays last.
        self.assertEqual(order, ["b", "a", "c", "d"])
        self.assertIn("# 4.9 MB/s, data 2026-11-06 00:00 UTC", out)
        self.assertIn("# 3.5 MB/s, data date unknown", out)
        self.assertIn("# 919.9 KB/s", out)

    def test_the_same_date_keeps_the_faster_first(self):
        _, order = self.order({"a": (2026, 11, 6), "b": (2026, 11, 6), "c": (2026, 11, 6)})
        self.assertEqual(order, ["a", "b", "c", "d"])

    def test_only_mirrors_in_the_window_are_asked_for_their_date(self):
        asked = []
        with mock.patch.object(r, "data_time", lambda mirror: asked.append(mirror)):
            r.alhp_order(self.LINES, r.measured_speeds(self.RESULTS))
        self.assertEqual(sorted(asked), [f"https://{m}.example/alhp/" for m in "abc"])

    def test_without_measured_speeds_the_order_stands(self):
        with mock.patch.object(r, "data_time", lambda mirror: self.fail("asked for a date")):
            self.assertEqual(r.alhp_order(self.LINES, {}), self.LINES)

    def test_the_date_comes_from_a_one_byte_get_through_redirects(self):
        """A mirror that redirects to storage refusing HEAD (mirror.sjtu.edu.cn:
        403) still has a date: a GET of one byte, the last response's
        Last-Modified."""
        asked = []

        def run_tool(cmd, env=None, timeout=600, stdin=None):
            asked.append(cmd)
            return ("HTTP/2 307\r\nlocation: https://storage.example/signed\r\n\r\n"
                    "HTTP/1.1 206 Partial Content\r\nLast-Modified: Fri, 06 Nov 2026 00:45:54 GMT\r\n\r\n")
        with mock.patch.object(r, "run_tool", run_tool):
            when = r.data_time("https://a.example/alhp/")
        self.assertEqual(when, datetime.datetime(2026, 11, 6, 0, 45, 54, tzinfo=datetime.timezone.utc))
        self.assertEqual(asked[0][-1], "https://a.example/alhp/" + r.ALHP_TEST)
        self.assertIn("0-0", asked[0])
        self.assertFalse(any(a.startswith("-") and "I" in a for a in asked[0]), asked[0])  # no HEAD


if __name__ == "__main__":
    unittest.main()
