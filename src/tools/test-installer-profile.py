#!/usr/bin/env python3
"""Build the ISO profile with command doubles, without Linux/archiso/network.
  python3 -B tools/test-installer-profile.py SCRATCH_PARENT
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class InstallerProfile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="installer-profile-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "source"
        self.src.mkdir()
        shutil.copyfile(SOURCE / "make-installer.sh", self.src / "make-installer.sh")
        shutil.copytree(SOURCE / "installer", self.src / "installer")
        for directory in ("base", "init", "progs", "edit", "link", "tools", "pkgbuilds", "aur-patches", "containers"):
            path = self.src / directory
            path.mkdir()
            (path / "data").write_text("trusted " + directory)
        for filename in ("make-root.sh", "make.sh"):
            (self.src / filename).write_text("#!/bin/bash\nexit 0\n")
        (self.src / "link/locachy-install").write_text("#!/usr/bin/env python3\nprint('fixture installer')\n")
        (self.src / "link/gestures").write_text("#!/usr/bin/env python3\nprint('fixture')\n")
        (self.src / "packages").write_text("base\nlabwc-locachy\n")
        (self.src / "rc.conf").write_text('key_home="Super+Esc"\n')
        self.baseline = self.root / "baseline"
        self.baseline.mkdir()
        (self.baseline / "packages.x86_64").write_text("base\nlinux\nopenssh\ncloud-init\n")
        (self.baseline / "profiledef.sh").write_text("# Baseline fixture is copied, not sourced by this test.\n"
                                                      "iso_name=upstream-fixture\n"
                                                      "bootmodes=('bios.syslinux' 'uefi.grub')\n"
                                                      "file_permissions=([\"/etc/shadow\"]=\"0:0:400\")\n")
        (self.baseline / "pacman.conf").write_text("[options]\n[cachyos-v4]\nInclude = bad-host-mirror\n")
        sysroot = self.baseline / "airootfs/etc/systemd/system"
        for target, service in (("multi-user.target.wants", "sshd.service"),
                                ("multi-user.target.wants", "systemd-networkd.service"),
                                ("network-online.target.wants", "systemd-networkd-wait-online.service"),
                                ("sockets.target.wants", "systemd-networkd.socket"),
                                ("multi-user.target.wants", "vmtoolsd.service")):
            (sysroot / target).mkdir(parents=True, exist_ok=True)
            (sysroot / target / service).symlink_to("/usr/lib/systemd/system/" + service)
        (self.baseline / "airootfs/etc/ssh/sshd_config.d").mkdir(parents=True)
        (self.baseline / "airootfs/etc/ssh/sshd_config.d/10-archiso.conf").write_text("PermitRootLogin yes\n")
        (self.baseline / "airootfs/etc/mkinitcpio.conf.d").mkdir(parents=True)
        (self.baseline / "airootfs/etc/mkinitcpio.conf.d/archiso.conf").write_text("HOOKS=(base udev modconf archiso block filesystems)\n")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        tool = self.bin / "mkarchiso"
        tool.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >\"$TEST_CALLS\"\nexit \"${TEST_RESULT:-0}\"\n")
        tool.chmod(0o755)
        # installer/make-live.sh's double: the programs it compiles, and how it was called.
        self.live = self.root / "live-calls"
        live = self.root / "make-live"
        live.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >"$TEST_LIVE"\nmkdir -p "$2/usr/local/bin"\n'
                        'printf elf >"$2/usr/local/bin/installer"\nprintf elf >"$2/usr/local/bin/term"\n')
        self.env = dict(os.environ, ARCHISO_BASELINE=str(self.baseline), PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        TEST_CALLS=str(self.calls), LIVE_BUILD=str(live), TEST_LIVE=str(self.live))
        self.work = self.root / "work"
        self.out = self.root / "out"

    def run_builder(self, *args):
        return subprocess.run(["bash", str(self.src / "make-installer.sh"), *map(str, args)],
                              env=self.env, text=True, capture_output=True)

    def profile(self):
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.calls.exists())
        return self.work / "profile"

    def test_official_generic_profile_preserves_upstream_live_implementation(self):
        profile = self.profile()
        config = (profile / "pacman.conf").read_text()
        self.assertIn("Architecture = x86_64", config)
        self.assertEqual([line for line in config.splitlines() if line.startswith("[")], ["[options]", "[core]", "[extra]"])
        self.assertNotIn("cachyos", config.lower())
        self.assertNotIn("alhp", config.lower())
        self.assertIn("CacheDir = " + str(self.work / "cache"), config)
        self.assertTrue((self.work / "cache").is_dir())
        self.assertEqual(config.count("Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch"), 2)
        self.assertNotIn("Include =", config)
        live = profile / "airootfs"
        live_config = (live / "etc/pacman.conf").read_text()
        self.assertEqual(live_config, (self.src / "installer/pacman.conf").read_text())
        self.assertNotIn(str(self.work), live_config)
        self.assertIn("Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch",
                      (live / "etc/pacman.d/mirrorlist").read_text())
        self.assertNotIn("bad-host-mirror", live_config)
        packages = (profile / "packages.x86_64").read_text().splitlines()
        self.assertEqual(len(packages), len(set(packages)))
        self.assertTrue({"base", "linux", "linux-firmware", "base-devel", "python", "git", "curl",
                         "pacman-contrib", "networkmanager", "rsync", "e2fsprogs", "dosfstools"} <= set(packages))
        self.assertNotIn("grub", packages)  # UEFI through systemd-boot, from the live system's systemd
        self.assertNotIn("linux-cachyos-bore", packages)
        self.assertTrue({"openssh", "cloud-init"} <= set(packages))  # Do not trim the packaged baseline.
        self.assertEqual((profile / "airootfs/etc/mkinitcpio.conf.d/archiso.conf").read_text(),
                         (self.baseline / "airootfs/etc/mkinitcpio.conf.d/archiso.conf").read_text())
        definition = (profile / "profiledef.sh").read_text()
        # The baseline's line is kept; LOCachy's override after it wins when sourced.
        self.assertIn("bootmodes=('bios.syslinux' 'uefi.grub')", definition)
        self.assertGreater(definition.rindex("bootmodes=('bios.syslinux' 'uefi.systemd-boot')"),
                           definition.index("bootmodes=('bios.syslinux' 'uefi.grub')"))
        self.assertIn('iso_name="locachy-installer"', definition)
        self.assertIn("buildmodes=('iso')", definition)
        self.assertNotIn('source "$', definition)

    def test_payload_and_permissions_cover_source_entrypoints_not_data(self):
        profile = self.profile()
        root = profile / "airootfs"
        payload = root / "usr/local/src/locachy"
        for directory in ("base", "init", "progs", "edit", "link", "tools", "pkgbuilds", "aur-patches", "containers"):
            self.assertEqual((payload / directory / "data").read_text(), "trusted " + directory)
        for name in ("make-root.sh", "make.sh", "packages", "rc.conf"):
            self.assertEqual((payload / name).read_bytes(), (self.src / name).read_bytes())
        self.assertEqual((root / "usr/local/bin/locachy-install").read_bytes(),
                         (SOURCE / "installer/locachy-install").read_bytes())
        definition = (profile / "profiledef.sh").read_text()
        for name in ("make-root.sh", "make.sh", "link/locachy-install", "link/gestures"):
            self.assertIn('["/usr/local/src/locachy/' + name + '"]="0:0:755"', definition)
        self.assertNotIn('["/usr/local/src/locachy/base/data"]', definition)
        self.assertTrue((root / "usr/local/bin/locachy-install").stat().st_mode & 0o111)

    def test_full_screen_installer_on_tty1_only(self):
        profile = self.profile()
        root = profile / "airootfs"
        self.assertEqual(self.live.read_text().split(), [str(self.src), str(root)])
        definition = (profile / "profiledef.sh").read_text()
        for name in ("installer", "term"):
            self.assertTrue((root / "usr/local/bin" / name).is_file())
            self.assertIn('["/usr/local/bin/' + name + '"]="0:0:755"', definition)
        autologin = (root / "etc/systemd/system/getty@tty1.service.d/autologin.conf").read_text()
        self.assertIn("--autologin root", autologin)
        self.assertFalse((root / "etc/systemd/system/serial-getty@.service.d").exists())
        login = (root / "root/.bash_profile").read_text()
        self.assertIn('if [ "$(tty)" = /dev/tty1 ]; then', login)
        self.assertLess(login.index("/usr/local/bin/installer\n"), login.index("/usr/local/bin/installer --text"))
        packages = set((profile / "packages.x86_64").read_text().splitlines())
        self.assertTrue({"freetype2", "libinput", "json-c", "ttf-dejavu", "noto-fonts-cjk", "wayland"} <= packages)
        # The clipboard the installer copies to and its terminal pastes from.
        unit = (root / "etc/systemd/system/locachy-clipboard.service").read_text()
        self.assertIn("ExecStart=/usr/local/lib/locachy/clipboard --serve", unit)
        self.assertIn("RuntimeDirectory=locachy", unit)
        wants = root / "etc/systemd/system/multi-user.target.wants/locachy-clipboard.service"
        self.assertEqual(os.readlink(wants), "../locachy-clipboard.service")
        self.assertIn('["/usr/local/lib/locachy/clipboard"]="0:0:755"', definition)

    def test_source_origin_records_repository_and_branch(self):
        self.env.update(LOCACHY_REPO="https://github.com/me/locachy.git", LOCACHY_BRANCH="next", LOCACHY_COMMIT="b" * 40)
        origin = json.loads((self.profile() / "airootfs/etc/locachy/source-origin.json").read_text())
        self.assertEqual(origin, {"repo": "https://github.com/me/locachy.git", "branch": "next", "commit": "b" * 40})

    def test_networkmanager_and_resolved_replace_networkd_without_remote_root(self):
        profile = self.profile()
        root = profile / "airootfs"
        system = root / "etc/systemd/system"
        for name in ("sshd.service", "cloud-init.target", "systemd-networkd.service", "systemd-networkd.socket"):
            self.assertEqual(os.readlink(system / name), "/dev/null")
        self.assertFalse((system / "multi-user.target.wants/sshd.service").is_symlink())
        self.assertFalse((root / "etc/ssh/sshd_config.d/10-archiso.conf").exists())
        self.assertFalse((system / "multi-user.target.wants/systemd-networkd.service").is_symlink())
        self.assertFalse((system / "sockets.target.wants/systemd-networkd.socket").is_symlink())
        self.assertTrue((system / "multi-user.target.wants/vmtoolsd.service").is_symlink())
        self.assertEqual(os.readlink(system / "multi-user.target.wants/NetworkManager.service"),
                         "/usr/lib/systemd/system/NetworkManager.service")
        self.assertEqual(os.readlink(root / "etc/resolv.conf"), "/run/systemd/resolve/stub-resolv.conf")
        self.assertEqual((root / "etc/NetworkManager/conf.d/20-locachy-installer.conf").read_text(),
                         "[main]\ndns=systemd-resolved\n")

    def test_success_requires_one_image_and_writes_cpu_source_record(self):
        tool = self.bin / "mkarchiso"
        tool.write_text('#!/bin/sh\nwhile [ "$1" != -o ]; do shift; done; shift\nprintf fixture-image >"$1/locachy-installer-test-x86_64.iso"\n')
        tool.chmod(0o755)
        result = self.run_builder(self.work, self.out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = json.loads((self.out / "build.json").read_text())
        self.assertEqual(record["cpu_baseline"], "x86-64")
        self.assertTrue(record["target_built_on_install"])
        self.assertIn("make-root.sh", record["source_sha256"])
        self.assertIn(record["sha256"], (self.out / "SHA256SUMS").read_text())

    def test_existing_destination_refusal_does_not_change_source_or_baseline(self):
        self.out.mkdir()
        sentinel = self.out / "keep"
        sentinel.write_text("do not overwrite")
        before = hashlib.sha256((self.src / "make-root.sh").read_bytes()).hexdigest()
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("destination exists", result.stderr)
        self.assertFalse(self.work.exists())
        self.assertEqual(sentinel.read_text(), "do not overwrite")
        self.assertEqual(hashlib.sha256((self.src / "make-root.sh").read_bytes()).hexdigest(), before)
        self.assertEqual((self.baseline / "pacman.conf").read_text(), "[options]\n[cachyos-v4]\nInclude = bad-host-mirror\n")

    def test_source_tree_and_shared_destination_are_rejected(self):
        result = self.run_builder("--profile-only", self.src / "work", self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.out.exists())
        result = self.run_builder("--profile-only", self.work, self.work)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.work.exists())

    def test_real_mode_invokes_standard_mkarchiso_and_propagates_failure(self):
        self.env["TEST_RESULT"] = "19"
        result = self.run_builder(self.work, self.out)
        self.assertEqual(result.returncode, 19, result.stdout + result.stderr)
        self.assertEqual(self.calls.read_text().strip(),
                         f"-w {self.work}/build -o {self.out} {self.work}/profile")

    def test_missing_installer_is_not_replaced_with_a_placeholder(self):
        (self.src / "link/locachy-install").unlink()
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source installer entry is missing", result.stderr)
        self.assertFalse(self.work.exists())
        self.assertFalse(self.out.exists())


class LiveEntry(unittest.TestCase):
    """installer/locachy-install, the live entry: the newest source from the
    update repository (here a local one, reached at an https URL through
    git's insteadOf), else what was fetched from there before, else the
    medium's own; LOCACHY_SOURCE says which and why."""
    URL = "https://example.invalid/locachy.git"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="live-entry-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.git_env = dict(os.environ, HOME=str(self.root), GIT_CONFIG_NOSYSTEM="1",
                            GIT_AUTHOR_NAME="test", GIT_AUTHOR_EMAIL="test@example.invalid",
                            GIT_COMMITTER_NAME="test", GIT_COMMITTER_EMAIL="test@example.invalid")
        self.repo = self.root / "repo"
        self.git("init", "-q", "-b", "main", str(self.repo))
        self.commit("v1")
        bundled = self.root / "bundled"
        self.fake(bundled / "link/locachy-install", "bundled")
        origin = self.root / "source-origin.json"
        origin.write_text(json.dumps({"repo": self.URL, "branch": "main", "commit": "0" * 40}))
        self.fetched = self.root / "run/locachy-source"
        self.fetched.parent.mkdir()
        self.env = dict(self.git_env, LOCACHY_SOURCE_BUNDLED=str(bundled), LOCACHY_SOURCE_FETCHED=str(self.fetched),
                        LOCACHY_SOURCE_ORIGIN=str(origin), GIT_ALLOW_PROTOCOL="file", GIT_CONFIG_COUNT="1",
                        GIT_CONFIG_KEY_0=f"url.file://{self.repo}.insteadOf", GIT_CONFIG_VALUE_0=self.URL)

    def git(self, *args):
        subprocess.run(["git", *args], check=True, capture_output=True, env=self.git_env)

    @staticmethod
    def fake(path, tag):
        """A locachy-install that says which it is, how it was got, its arguments."""
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"import os, sys\nprint({tag!r}, os.environ.get('LOCACHY_SOURCE'), *sys.argv[1:])\n")

    def commit(self, tag):
        self.fake(self.repo / "src/link/locachy-install", tag)
        self.git("-C", str(self.repo), "add", "-A")
        self.git("-C", str(self.repo), "commit", "-q", "-m", tag)

    def entry(self, *args):
        result = subprocess.run(["bash", str(SOURCE / "installer/locachy-install"), *args], env=self.env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip(), result.stderr

    def test_installs_from_the_newest_then_the_one_before_then_the_medium_own(self):
        out, err = self.entry("--describe-json")
        self.assertEqual(out, "v1 fetched --describe-json")
        self.assertIn(f"Fetching the newest LOCachy source: {self.URL} (main)", err)
        self.commit("v2")
        out, err = self.entry("--check", "--disk=/dev/sda")
        self.assertEqual(out, "v1 fetched earlier --check --disk=/dev/sda")
        self.assertEqual(err, "")
        out, _ = self.entry("--yes", "--disk=/dev/sda")
        self.assertEqual(out, "v2 fetched --yes --disk=/dev/sda")
        out, _ = self.entry("--yes", "--update-branch", "other")
        self.assertTrue(out.startswith(f"bundled bundled: could not fetch {self.URL} (other): "), out)
        shutil.rmtree(self.repo)
        out, _ = self.entry("--yes")
        self.assertTrue(out.startswith(f"v2 fetched before: could not fetch {self.URL} (main): "), out)
        self.assertEqual(sorted(p.name for p in self.fetched.parent.iterdir()),
                         ["locachy-source", "locachy-source.from"])

    def test_updates_off_or_no_repository_run_the_medium_own(self):
        out, _ = self.entry("--yes", "--update-repo=off")
        self.assertEqual(out, "bundled bundled: updates are off --yes --update-repo=off")
        out, _ = self.entry("--update-repo", "/somewhere/locachy", "--yes")
        self.assertEqual(out, "bundled bundled: no repository to fetch from (/somewhere/locachy, main) "
                              "--update-repo /somewhere/locachy --yes")
        self.assertFalse(self.fetched.exists())

    def test_continuing_fetches_from_where_that_installation_did(self):
        fork = "https://example.invalid/fork.git"
        self.git("-C", str(self.repo), "branch", "next")
        record = self.root / "resume.json"
        record.write_text(json.dumps({"install_id": "a" * 32, "update_repo": fork, "update_branch": "next"}))
        self.env.update(LOCACHY_RESUME_RECORD=str(record), GIT_CONFIG_COUNT="2",
                        GIT_CONFIG_KEY_1=f"url.file://{self.repo}.insteadOf", GIT_CONFIG_VALUE_1=fork)
        out, err = self.entry("--resume", "--yes")
        self.assertEqual(out, "v1 fetched --resume --yes")
        self.assertIn(f"Fetching the newest LOCachy source: {fork} (next)", err)
        out, err = self.entry("--trust-key", "40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C")
        self.assertEqual(out, "v1 fetched earlier --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C")
        self.assertEqual(err, "")
        out, err = self.entry("--yes")  # not continuing: the medium's update repository
        self.assertIn(f"Fetching the newest LOCachy source: {self.URL} (main)", err)

    def test_fetches_over_https_and_ssh_only(self):
        del self.env["GIT_ALLOW_PROTOCOL"]
        out, _ = self.entry("--yes")
        self.assertTrue(out.startswith(f"bundled bundled: could not fetch {self.URL} (main): "), out)
        self.assertIn("transport 'file' not allowed", out)


if __name__ == "__main__":
    unittest.main()
