#!/usr/bin/env python3
"""Prepared local build performs target upgrade only in the disposable stage;
remote recipe metadata/builds stay under the build user. No mounts/pacman."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

s = importlib.util.spec_from_file_location('local_build_tests', Path(__file__).with_name('test-local-build.py'))
m = importlib.util.module_from_spec(s)
# The imported harness consumes the optional scratch parent once.
s.loader.exec_module(m)


class PreparedBuilder(m.LocalBuild):
    def test_prepare_build_stage_never_installs_untested_candidate(self):
        plan = self.root / 'plan'
        plan.mkdir()
        (plan / 'plan.json').write_text(json.dumps({'recipes': {'example': {'candidate': {'package_version': '1-1'}}}}))
        self.env['LOCACHY_BUILD_PREPARE'] = str(plan)
        self.command('pacman', 'echo "pacman $*" >>"$TEST_LOG"; '
                     'case "$1:${2:-}" in -T:installed) exit 0 ;; -T:*) exit 1 ;; '
                     '-Sp:aur-tool) exit 1 ;; -Qq:) echo example ;; -Q:) echo "base 1" ;; *) exit 0 ;; esac')
        self.command('bsdtar', 'case "$*" in *.PKGINFO*) printf "pkgname = example\\npkgver = 1-1\\n" ;; '
                     '*.BUILDINFO*) printf "pkgname = example\\n" ;; *) exit 1 ;; esac')
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.log.read_text()
        self.assertIn('pacman -Su --noscriptlet --noconfirm --ignore example', calls)
        self.assertIn('makepkg --force --noconfirm', calls)
        self.assertLess(calls.index('pacman -Su'), calls.index('makepkg --force'))
        self.assertNotIn('pacman -U', calls)
        text = m.SOURCE.read_text()
        self.assertIn('workdir=$run/work,userxattr', text)
        self.assertIn('mount -o rbind,ro=recursive /sys "$root/sys"', text)
        self.assertNotIn('mount -t sysfs', text)
        self.assertIn('pacman -U --noscriptlet --noconfirm --ask 4 --assume-installed initramfs --asdeps', text)
        self.assertIn('--assume-installed initramfs', calls)
        self.assertIn("hashlib.sha256(artifact.read_bytes()).hexdigest() != item['sha256']", text)
        self.assertIn('all prepared outputs tested together', text)
        self.assertIn('python3 -B - "$run/recipe"', text)
        self.assertIn('pacman -Rdd --noscriptlet --noconfirm example', calls)
        self.assertIn('--assume-installed example=1-1', calls)
        self.assertTrue((self.handover / 'target-base-packages.txt').exists())

    def test_declared_packages_are_in_every_build_and_contract_root(self):
        """A library the update's tests need (libraw for view-render) and the
        host lacks: installed after the target upgrade, before the root's
        package list is taken, in builds and in the contracts' root alike."""
        plan = self.root / 'plan'
        plan.mkdir()
        recipes = {'example': {'candidate': {'package_version': '1-1'}}}
        (plan / 'plan.json').write_text(json.dumps({'recipes': recipes, 'missing': ['chmlib', 'libraw']}))
        self.env['LOCACHY_BUILD_PREPARE'] = str(plan)
        self.command('pacman', 'echo "pacman $*" >>"$TEST_LOG"; '
                     'case "$1:${2:-}" in -T:installed) exit 0 ;; -T:*) exit 1 ;; '
                     '-Sp:aur-tool) exit 1 ;; -Qq:) echo example ;; -Q:) echo "base 1" ;; *) exit 0 ;; esac')
        self.command('bsdtar', 'case "$*" in *.PKGINFO*) printf "pkgname = example\\npkgver = 1-1\\n" ;; '
                     '*.BUILDINFO*) printf "pkgname = example\\n" ;; *) exit 1 ;; esac')
        for stage in (['--local', 'example'], ['--validate-prepared', str(plan)]):
            self.log.write_text('')
            result = subprocess.run(['bash', str(self.script)] + stage, env=self.env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = self.log.read_text().splitlines()
            install = [c for c in calls if c.startswith('pacman -S --needed --noscriptlet')]
            self.assertEqual(install, ['pacman -S --needed --noscriptlet --noconfirm --assume-installed initramfs '
                                       '--assume-installed example=1-1 chmlib libraw'], stage)
            self.assertLess(calls.index(next(c for c in calls if c.startswith('pacman -Su '))), calls.index(install[0]))
            self.assertLess(calls.index(install[0]), calls.index('pacman -Q'))
        (plan / 'plan.json').write_text(json.dumps({'recipes': recipes}))
        self.log.write_text('')
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('pacman -S --needed --noscriptlet', self.log.read_text())

    def test_tracked_overlay_does_not_need_a_baseline_pkgbuild(self):
        text = m.SOURCE.read_text()
        self.assertIn('[ -f "$recipes/$1/PKGBUILD" ] || [ -f "$recipes/$1/upstream.json" ]', text)
        self.assertIn('for marker in ("PKGBUILD", "upstream.json")', text)
        self.assertIn('effective recipe has no PKGBUILD; prepare the tracked upstream candidate first', text)

    def test_gate_initializes_first_root_and_links_idle_protocol(self):
        text = m.SOURCE.with_name('local-package-gate').read_text()
        self.assertIn('"$src/link/labwc-rc.xml" /etc/locachy/labwc/rc.xml', text)
        self.assertIn('"$src/rc.conf" /etc/locachy/rc.conf', text)
        self.assertIn('/var/lib/locachy/rime', text)
        self.assertIn(': >/etc/locachy/monitors.conf', text)
        self.assertIn('wlr-output-management-unstable-v1 ext-idle-notify-v1; do', text)
        self.assertIn('"$out"/ext-idle-notify-v1-protocol.c "$out/base.a"', text)
        self.assertNotIn('sh "$src/make.sh"', text)
        self.assertIn('mount -t cgroup2 -o nosuid,noexec,nodev cgroup2 "$root/sys/fs/cgroup"', text)
        self.assertNotIn('mount --rbind /sys', text)

    def test_every_build_uses_same_base_and_keeps_distro_library(self):
        plan = self.root / 'plan'
        plan.mkdir()
        recipes = {'labwc-locachy': {'candidate': {'upstream_package': 'labwc', 'package_version': '1:2-1'}},
                   'libinput-tools': {'candidate': {'upstream_package': 'libinput-tools', 'package_version': '1:3-1'}}}
        (plan / 'plan.json').write_text(json.dumps({'recipes': recipes}))
        self.env['LOCACHY_BUILD_PREPARE'] = str(plan)
        self.command('pacman', 'echo "pacman $*" >>"$TEST_LOG"; '
                     'case "$1:${2:-}" in -T:installed) exit 0 ;; -T:*) exit 1 ;; '
                     '-Sp:aur-tool) exit 1 ;; -Qq:) printf "labwc-locachy\\nlibinput-tools\\nlabwc\\n" ;; '
                     '-Q:) echo "base 1" ;; *) exit 0 ;; esac')
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.log.read_text()
        self.assertIn('pacman -Rdd --noscriptlet --noconfirm labwc-locachy libinput-tools labwc', calls)
        self.assertNotIn('libinput-tools labwc libinput', calls)
        self.assertIn('--ignore labwc-locachy,libinput-tools', calls)
        self.assertIn('--assume-installed libinput-tools=1:3-1', calls)
        self.assertNotIn('pacman -U', calls)

    def test_provider_alias_is_not_removed_as_an_installed_identity(self):
        plan = self.root / 'plan'
        plan.mkdir()
        (plan / 'plan.json').write_text(json.dumps({'recipes': {
            'labwc-locachy': {'candidate': {'upstream_package': 'labwc', 'package_version': '1:2-1'}}}}))
        self.env['LOCACHY_BUILD_PREPARE'] = str(plan)
        self.command('pacman', 'echo "pacman $*" >>"$TEST_LOG"; '
                     'case "$1:${2:-}" in -T:installed) exit 0 ;; -T:*) exit 1 ;; '
                     '-Sp:aur-tool) exit 1 ;; -Qq:*) echo labwc-locachy ;; -Q:) echo "base 1" ;; *) exit 0 ;; esac')
        result = self.run_stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        removal = [s for s in self.log.read_text().splitlines() if s.startswith('pacman -Rdd')]
        self.assertEqual(removal, ['pacman -Rdd --noscriptlet --noconfirm labwc-locachy'])


if __name__ == '__main__':
    unittest.main()
