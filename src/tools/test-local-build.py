#!/usr/bin/env python3
"""Run locachy-build's chroot-stage logic with subprocess command doubles.
The stage's /run and state paths are rewritten only in a scratch script;
no mount, user switch, package manager or PKGBUILD execution takes place.
  python3 -B tools/test-local-build.py SCRATCH_PARENT
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
SOURCE = Path(__file__).resolve().parents[1] / "link/locachy-build"


class LocalBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="local-build-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.bdir = self.root / "builder"
        self.home = self.bdir / "home"
        self.handover = self.bdir / "out"
        self.home.mkdir(parents=True)
        self.handover.mkdir()
        self.recipe = self.root / "recipe"
        self.recipe.mkdir()
        (self.recipe / "PKGBUILD").write_text("# fixture: real makepkg is deliberately never run\n")
        (self.recipe / "guard.patch").write_text("patch data\n")
        self.info = self.root / "fixture.srcinfo"
        self.info.write_text("pkgbase = example\npkgver = 1\npkgrel = 1\n"
                             "\tdepends = installed\n\tmakedepends = repo-tool>=2\n"
                             "\tcheckdepends = aur-tool\n\tdepends_aarch64 = wrong-arch\n"
                             "\tdepends_x86_64 = repo-lib\npkgname = example\n")
        self.log = self.root / "calls"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        LOCACHY_BUILD_STAGE="chroot", TEST_ROOT=str(self.root),
                        TEST_HOME=str(self.home), TEST_INFO=str(self.info), TEST_LOG=str(self.log),
                        TEST_PYTHON=sys.executable)
        text = SOURCE.read_text().replace("bdir=/var/lib/locachy/builder", f"bdir={self.bdir}")
        text = text.replace("out=/var/cache/locachy-pkgs", f"out={self.root / 'cache'}")
        text = text.replace("/run/locachy-recipe", str(self.recipe))
        text = text.replace("/run/locachy-local.srcinfo", str(self.root / "generated.srcinfo"))
        self.script = self.root / "locachy-build"
        self.script.write_text(text)
        self.command("id", 'case "$1" in -u|-g) echo 123 ;; *) exit 1 ;; esac')
        self.command("uname", 'echo x86_64')
        self.command("install", 'last=; for a do case "$a" in /*) mkdir -p "$a" ;; esac; done')
        self.command("chown", ':')
        self.command("find", ':')
        self.command("setpriv", 'while [ "$1" != --reset-env ]; do shift; done; shift; '
                                  'printf "builder HOME=%s command=%s\\n" "$TEST_HOME" "$*" >>"$TEST_LOG"; '
                                  'exec env HOME="$TEST_HOME" "$@"')
        self.command("makepkg", 'if [ "$1" = --printsrcinfo ]; then cat "$TEST_INFO"; else '
                                 'echo "makepkg $* HOME=$HOME PKGDEST=$PKGDEST" >>"$TEST_LOG"; '
                                 'test "${TEST_FAIL_BUILD:-0}" != 1 || exit 7; '
                                 'printf package >"$PKGDEST/example-1-1-x86_64.pkg.tar.zst"; fi')
        self.command("pacman", 'echo "pacman $*" >>"$TEST_LOG"; '
                                'case "$1:$2" in -T:installed) exit 0 ;; -T:*) exit 1 ;; '
                                '-Sp:aur-tool) exit 1 ;; *) exit 0 ;; esac')
        self.command("pikaur", 'echo "pikaur $*" >>"$TEST_LOG"; '
                                'mkdir -p "$TEST_HOME/.cache/pikaur/pkg"; '
                                'echo dependency >"$TEST_HOME/.cache/pikaur/pkg/aur-tool-1-1-x86_64.pkg.tar.zst"')
        # aur_keys (the AUR's .SRCINFO of the AUR dependencies): TEST_AUR_KEYS.
        self.command("python3", 'case "$*" in *"${LOCACHY_BUILD_PREPARE:-/no-plan}/plan.json"*) '
                     'exec "$TEST_PYTHON" "$@";; esac; '
                     'cat >/dev/null; echo "aur_keys $*" >>"$TEST_LOG"; printf "%b" "${TEST_AUR_KEYS:-}"')
        # The build user's keyring: the keys listed in TEST_KEYRING.
        self.keyring = self.root / "keyring"
        self.keyring.write_text("")
        self.env["TEST_KEYRING"] = str(self.keyring)
        self.command("gpg", 'echo "gpg $*" >>"$TEST_LOG"; case "$*" in *--list-keys*) '
                            'for k do :; done; grep -qx "$k" "$TEST_KEYRING" ;; esac')

    def command(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body + "\n")
        path.chmod(0o755)

    def run_stage(self):
        return subprocess.run(["bash", str(self.script), "--local", "example"],
                              env=self.env, text=True, capture_output=True)

    def test_local_recipe_deps_and_makepkg_are_inside_builder_stage(self):
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.log.read_text()
        self.assertIn("pacman -S --needed --noconfirm --asdeps repo-lib repo-tool>=2", calls)
        self.assertIn("pikaur -S --aur --noconfirm --user-id 123 --group-id 123 --home-dir " + str(self.home) +
                      " --build-gpgdir " + str(self.home) + "/.gnupg aur-tool", calls)
        self.assertIn("makepkg --force --noconfirm HOME=" + str(self.home), calls)
        self.assertNotIn("wrong-arch", calls)
        self.assertEqual((self.home / "local/example/guard.patch").read_text(), "patch data\n")
        self.assertTrue((self.handover / "example-1-1-x86_64.pkg.tar.zst").is_file())
        self.assertTrue((self.handover / "aur-tool-1-1-x86_64.pkg.tar.zst").is_file())
        self.assertIn("makepkg --printsrcinfo", calls)
        self.assertIn("aur_keys -I - aur-tool", calls)

    def test_untrusted_source_keys_stop_before_anything_is_built(self):
        own, dependency = "3C2C43D9447D5938EF4551EBE23B7E70B467F0BF", "07948FFA64160A425BCD27EAC732B1D1C28F4E2F"
        self.info.write_text(self.info.read_text() + "\tvalidpgpkeys = " + own.lower() + "\n")
        self.env["TEST_AUR_KEYS"] = "aur-tool " + dependency + "\\n"
        result = self.run_stage()
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertEqual(sorted((self.handover / "untrusted-keys").read_text().splitlines()),
                         ["aur-tool " + dependency, "example " + own])
        calls = self.log.read_text()
        self.assertNotIn("--recv-keys", calls)  # nothing is fetched, let alone trusted, by a build
        self.assertNotIn("pikaur -S", calls)
        self.assertNotIn("makepkg --force", calls)
        self.assertFalse((self.handover / "example-1-1-x86_64.pkg.tar.zst").exists())
        # Trusted (in the build user's keyring): built, and checked with them.
        (self.handover / "untrusted-keys").unlink()
        self.log.unlink()
        self.keyring.write_text(own + "\n" + dependency + "\n")
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.log.read_text()
        self.assertIn("makepkg --force --noconfirm", calls)
        self.assertNotIn("--skippgpcheck", calls)
        self.assertFalse((self.handover / "untrusted-keys").exists())
        # Not a full fingerprint: refused.
        self.log.unlink()
        self.info.write_text(self.info.read_text().replace(own.lower(), "B467F0BF"))
        result = self.run_stage()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid source PGP fingerprint B467F0BF", result.stderr)
        self.assertNotIn("makepkg --force", self.log.read_text())

    def test_trust_key_asks_and_adds_only_what_is_answered_y(self):
        key, other = "40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C", "07948FFA64160A425BCD27EAC732B1D1C28F4E2F"
        (self.bdir / "untrusted-keys").write_text(f"lyrebird-proxy {key}\nlyrebird-proxy {other}\n")
        self.command("getent", 'exit 0')
        # A separate keyring per call (--homedir) gets what the keyserver has;
        # the build user's (no --homedir) is TEST_KEYRING.
        self.command("gpg", 'echo "gpg $*" >>"$TEST_LOG"; for k do :; done\n'
                            'case "$*" in\n'
                            '*--recv-keys*) exit 0 ;;\n'
                            '*--with-colons*) echo "fpr:::::::::$k:" ;;\n'
                            '*--homedir*--fingerprint*) echo "pub   ed25519 2023-01-01"; echo "uid   Upstream Maintainer" ;;\n'
                            '*--export*) echo "key $k" ;;\n'
                            '*--import*) sed "s/^key //" >>"$TEST_KEYRING" ;;\n'
                            '*--list-keys*) grep -qx "$k" "$TEST_KEYRING" ;;\n'
                            '*--kill*) ;;\n'
                            'esac')
        self.command("gpgconf", ':')
        env = dict(self.env)
        env.pop("LOCACHY_BUILD_STAGE")
        result = subprocess.run(["bash", str(self.script), "--trust-key", key.lower(), other],
                                env=env, text=True, capture_output=True, input="y\nn\n")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)  # one was not trusted
        self.assertIn("Upstream Maintainer", result.stdout)
        self.assertIn(key + ": trusted", result.stdout)
        self.assertIn(other + ": not trusted", result.stdout)
        self.assertEqual(self.keyring.read_text().split(), [key])
        self.assertEqual((self.bdir / "untrusted-keys").read_text(), f"lyrebird-proxy {other}\n")
        calls = self.log.read_text()
        for server in ("keys.openpgp.org", "keyserver.ubuntu.com"):  # every copy, merged
            self.assertIn(f"--keyserver hkps://{server} --recv-keys {key}", calls)
        # Trusted already: brought up to date (the same key's newer copy), not asked again.
        result = subprocess.run(["bash", str(self.script), "--trust-key", key],
                                env=env, text=True, capture_output=True, input="")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(key + ": trusted already; brought up to date from keys.openpgp.org, keyserver.ubuntu.com",
                      result.stdout)
        self.assertNotIn("[y/N]", result.stdout)
        result = subprocess.run(["bash", str(self.script), "--trust-key", "B467F0BF"],
                                env=env, text=True, capture_output=True, input="y\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a full fingerprint: B467F0BF", result.stderr)

    def test_makepkg_failure_is_propagated(self):
        self.env["TEST_FAIL_BUILD"] = "1"
        result = self.run_stage()
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertFalse((self.handover / "example-1-1-x86_64.pkg.tar.zst").exists())

    def test_install_exact_artifact_resolves_repo_runtime_deps_in_netns(self):
        # Execute only the real installation tail, not mounts or a build.
        # The namespace/file paths are private fixtures in this script.
        text = SOURCE.read_text()
        marker = '\nif [ "$local_recipe" = 1 ]; then\n    name=$2\n    if [ "$inst" = 1 ]; then'
        tail = marker + text.split(marker, 1)[1]
        tail = tail.replace('/run/netns/locachy-net', str(self.root / "netns"))
        (self.root / "netns").touch()
        records = self.root / "records"
        records.mkdir()
        (records / "example.build.json").write_text('{"version":"1-1"}')
        artifact = self.handover / "example-1-1-x86_64.pkg.tar.zst"
        artifact.write_text("package")
        install_script = self.root / "install-local"
        install_script.write_text('#!/bin/bash\nset -euo pipefail\n'
                                  f'local_recipe=1; inst=1; artifact={artifact}; records={records}; out={self.handover}\n'
                                  'set -- --local example\n' + tail)
        self.command("bsdtar", 'printf "pkgname = example\\npkgver = 1-1\\ndepend = installed\\ndepend = repo-tool>=2\\n"')
        self.command("ip", 'echo "ip $*" >>"$TEST_LOG"; shift 3; exec "$@"')
        self.command("install", 'while [ "$1" != 0 ]; do case "$1" in -m|-o|-g) shift 2 ;; *) break ;; esac; done; cp "$1" "$2"')
        env = dict(self.env)
        env.pop("LOCACHY_BUILD_STAGE")
        result = subprocess.run(["bash", str(install_script)], env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.log.read_text()
        self.assertIn("pacman -T repo-tool>=2", calls)
        self.assertIn("pacman -Sp repo-tool>=2", calls)
        self.assertIn("ip netns exec locachy-net pacman -U --noconfirm --asdeps " + str(artifact), calls)
        self.assertNotIn("pacman -S --", calls)
        self.assertTrue((records / "example.json").is_file())

    def test_failed_file_listing_never_reaches_host_install(self):
        # Execute the actual AUR installation tail with a resolver that emits
        # one valid-looking path and then fails. mapfile/process substitution
        # used to hide that failure and install the partial list.
        text = SOURCE.read_text()
        marker = 'elif [ "$inst" = 1 ]; then\n    listing='
        tail = 'if [ "$inst" = 1 ]; then\n    listing=' + text.split(marker, 1)[1]
        resolver = self.root / 'partial-resolver'
        resolver.write_text('#!/bin/sh\necho /partial.pkg.tar.zst\nexit 23\n')
        resolver.chmod(0o755)
        tail = tail.replace('"$0" --files / "$@"', str(resolver) + ' --files / "$@"')
        script = self.root / 'install-failed-list'
        script.write_text('#!/bin/bash\nset -euo pipefail\ninst=1\n'
                          'die() { echo "$*" >&2; exit 1; }\nset -- example\n' + tail)
        env = dict(self.env)
        env.pop('LOCACHY_BUILD_STAGE')
        result = subprocess.run(['bash', str(script)], env=env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.log.exists())
        self.assertIn('cannot resolve built package files', result.stderr)

    def test_invalid_local_names_rejected_before_build_environment(self):
        env = dict(self.env)
        env.pop("LOCACHY_BUILD_STAGE")
        for name in ("../outside", "-option", "a/b"):
            result = subprocess.run(["bash", str(self.script), "--local", name],
                                    env=env, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue("--local takes one package name" in result.stderr or "usage: locachy-build" in result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
