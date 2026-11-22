#!/usr/bin/env python3
"""Exercise prepared builds and gate proof handling without mounts or pacman."""
import hashlib
import importlib.util
import json
from pathlib import Path
import runpy
import shutil
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
        self.assertIn('upperdir=$2/upper,workdir=$2/work,userxattr', text)
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
        """Missing declared packages are installed after the target upgrade
        and before the package list is taken, for builds and both gate modes."""
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
        for stage in (['--local', 'example'], ['--validate-prepared'], ['--validate-prepared', '--regression']):
            self.log.write_text('')
            result = subprocess.run(['bash', str(self.script)] + stage, env=self.env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = self.log.read_text().splitlines()
            install = [c for c in calls if c.startswith('pacman -S --needed --noscriptlet')]
            self.assertEqual(install, ['pacman -S --needed --noscriptlet --noconfirm --assume-installed initramfs '
                                       '--assume-installed example=1-1 chmlib libraw'], stage)
            self.assertLess(calls.index(next(c for c in calls if c.startswith('pacman -Su '))), calls.index(install[0]))
            self.assertLess(calls.index(install[0]), calls.index('pacman -Q'))
            if stage[0] == '--validate-prepared':
                self.assertNotIn('makepkg', '\n'.join(calls))
                self.assertFalse(any(c.startswith('pacman -U ') for c in calls))
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

    def test_labwc_regression_initializes_first_root_and_links_idle_protocol(self):
        text = m.SOURCE.with_name('local-package-gate').read_text()
        self.assertNotIn('mount --rbind /sys', text)
        text = text[text.index('        run_logged() {'):]
        self.assertIn('"$src/link/labwc-rc.xml" /etc/locachy/labwc/rc.xml', text)
        self.assertIn('"$src/rc.conf" /etc/locachy/rc.conf', text)
        self.assertIn('/var/lib/locachy/rime', text)
        self.assertIn(': >/etc/locachy/monitors.conf', text)
        self.assertIn('run_logged host-clients-build.log sh "$src/tools/build-check.sh" "$out"', text)
        self.assertIn('install -m 755 "$out/labwc-control" /usr/local/lib/locachy/labwc-control', text)
        build = (m.SOURCE.parents[1] / 'tools/build-check.sh').read_text()
        control = build.split('"$src/progs/labwc-control.c"', 1)[1].split('-o "$out/labwc-control"', 1)[0]
        self.assertIn('wlr-output-power-management-unstable-v1 ext-idle-notify-v1', control)
        self.assertNotIn('sh "$src/make.sh"', text)

    def sealed_validation_fixture(self):
        """Run the actual wrapper, seal checks and proof writer with a fake installed root."""
        self.env.pop('LOCACHY_BUILD_STAGE')
        plan = self.root / 'plan'
        self.name = 'labwc-locachy'
        self.smoke_id = 'labwc-locachy-update-smoke-v1'
        self.regression_id = 'labwc-clipboard-corner-drop-v4'
        self.prepared = plan / 'prepared' / self.name
        self.prepared.mkdir(parents=True)
        self.proof = self.prepared / 'result.json'
        source = plan / 'test-source'
        (source / 'link').mkdir(parents=True)
        transaction = source / 'link/locachy_transaction.py'
        shutil.copyfile(m.SOURCE.with_name(transaction.name), transaction)
        tx = runpy.run_path(str(transaction))
        for directory in ('base', 'init', 'progs', 'tools'):
            (source / directory).mkdir()
        for file in ('rc.conf', 'packages'):
            (source / file).touch()
        gate = source / 'link/local-package-gate'
        gate.write_text('set -eu\n'
                        'printf "gate %s %s\\n" "$1" "${5:-update-smoke}" >>"$TEST_LOG"\n'
                        'id=$TEST_SMOKE_ID\n'
                        'if [ "${5:-}" = --regression ]; then id=$TEST_REGRESSION_ID; fi\n'
                        'if [ "${TEST_GATE_MISSING:-0}" = 0 ]; then\n'
                        '    printf "{\\"name\\":\\"%s\\",\\"id\\":\\"%s\\",\\"passed\\":%s}\\n" '
                        '"$1" "${TEST_GATE_ID:-$id}" "${TEST_GATE_PASSED:-true}" >"$4/gate.json"\n'
                        'fi\n'
                        'exit "${TEST_GATE_STATUS:-0}"\n')
        self.env.update(TEST_SMOKE_ID=self.smoke_id, TEST_REGRESSION_ID=self.regression_id)
        (plan / 'sync').mkdir()
        (plan / 'pacman.conf').touch()
        recipe_hash = tx['tree_hash'](self.recipe)
        data = {'version': 1, 'host_sha256': hashlib.sha256(b'base 1\n').hexdigest(),
                'sync_sha256': tx['tree_hash'](plan / 'sync'),
                'config_sha256': tx['file_hash'](plan / 'pacman.conf'),
                'recipes': {self.name: {'recipe_sha256': recipe_hash, 'baseline_sha256': recipe_hash,
                            'recipe_dir': str(self.recipe), 'baseline_dir': str(self.recipe),
                            'candidate': {'gate': self.smoke_id, 'package_version': '1-1'}}},
                'test_source': {'snapshot': str(source), 'original': str(source),
                                'sha256': tx['test_source_hash'](source)}}
        (plan / 'plan.json').write_text(json.dumps(data))
        self.plan_sha = tx['file_hash'](plan / 'plan.json')
        artifact = self.prepared / f'{self.name}-1-1-x86_64.pkg.tar.zst'
        artifact.write_text('sealed candidate')
        base = self.root / 'base-manifest'
        base.write_text('base 1\n')
        (self.prepared / 'build.json').write_text(json.dumps({
            'name': self.name, 'artifact': str(artifact), 'sha256': tx['file_hash'](artifact),
            'source_host_sha256': data['host_sha256'], 'recipe_sha256': recipe_hash,
            'target_base_sha256': tx['file_hash'](base), 'version': '1-1', 'record': {}}))
        installed_helper = self.root / 'helper'
        installed_helper.mkdir()
        shutil.copyfile(transaction, installed_helper / transaction.name)
        self.script.write_text(self.script.read_text().replace('/run/netns/locachy-net',
                                                               str(self.root / 'no-netns'))
                               .replace("sys.path.insert(0, '/usr/local/lib/locachy')",
                                        f"sys.path.insert(0, {str(installed_helper)!r})"))
        self.command('getent', ':')
        self.command('modprobe', ':')
        self.command('mount', ':')
        self.command('unshare', 'while [ "${1#--}" != "$1" ]; do '
                     'if [ "$1" = --propagation ]; then shift 2; else shift; fi; done; exec "$@"')
        self.command('setpriv', 'while [ "$1" != -- ]; do shift; done; shift; exec "$@"')
        self.command('chroot', 'root=$1; shift; '
                     'if [ "$1" = env ]; then '
                     '    shift 2; while [ "${1#*=}" != "$1" ]; do shift; done; '
                     'fi; '
                     'case "$1:$2" in '
                     '/usr/local/bin/locachy-build:--validate-prepared) '
                     'printf "base 1\\n" >"$root/../out/target-base-packages.txt" ;; '
                     'pacman:-U) printf "install %s\\n" "$*" >>"$TEST_LOG" ;; '
                     'pacman:-Q) printf "base 1\\nlabwc-locachy 1-1\\n" ;; *) exit 99 ;; esac')
        self.env['TEST_HOST_VERSION'] = '1'
        self.command('pacman', '[ "$*" = -Q ] || exit 99; printf "base %s\\n" "${TEST_HOST_VERSION:-1}"')
        self.command('python3', 'exec "$TEST_PYTHON" "$@"')
        # Mounts are no-ops; give the scratch overlay the directories it would have inherited.
        text = self.script.read_text().replace('    mount_build_overlay / "$run"',
                                                '    mkdir -p "$root/dev" "$root/etc" "$root/usr/local/bin"')
        self.script.write_text(text)
        self.script.chmod(0o755)  # The outer wrapper execs itself for its namespace stage.
        return plan, data, tx

    def run_validation(self, mode, plan, **env):
        command = ["bash", str(self.script), mode, str(plan)]
        if mode == '--regress-prepared':
            command.append(env.pop('TEST_PLAN_SHA', self.plan_sha))
        return subprocess.run(command, env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=15)

    def test_developer_regression_keeps_separate_logs_and_never_publishes_update_proof(self):
        plan, data, tx = self.sealed_validation_fixture()
        notice = self.bdir / 'untrusted-keys'
        notice.write_bytes(b'unchanged source-key notification\n')
        run = self.run_validation('--regress-prepared', plan)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(notice.read_bytes(), b'unchanged source-key notification\n')
        self.assertIn('developer regression logs:', run.stdout)
        [logs] = plan.glob('regression.*')
        self.assertEqual(json.loads((logs / self.name / 'gate.json').read_text())['id'], self.regression_id)
        self.assertFalse(self.proof.exists())
        self.assertIsNone(tx['reusable_results'](plan, data))

        run = self.run_validation('--validate-prepared', plan)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        proof = tx['prepared_result'](plan, self.name, data)
        self.assertEqual(proof['tests']['id'], self.smoke_id)
        self.assertEqual(proof['tests']['artifacts'], {self.name: proof['sha256']})
        self.assertEqual([line for line in self.log.read_text().splitlines() if line.startswith('gate ')],
                         [f'gate {self.name} --regression', f'gate {self.name} update-smoke'])

        original = self.proof.read_bytes()
        run = self.run_validation('--regress-prepared', plan, TEST_GATE_STATUS='7')
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('developer regression failed', run.stderr)
        self.assertEqual(self.proof.read_bytes(), original)
        self.assertEqual(len(list(plan.glob('regression.*'))), 2)

    def test_regression_refuses_stale_inputs_before_testing(self):
        plan, _, _ = self.sealed_validation_fixture()
        for env, reason in (({'TEST_PLAN_SHA': '0' * 64}, 'update plan lock changed'),
                            ({'TEST_HOST_VERSION': '2'}, 'host packages changed')):
            with self.subTest(env=env):
                run = self.run_validation('--regress-prepared', plan, **env)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(reason, run.stderr)
                self.assertFalse(self.log.exists())
        source = plan / 'test-source/link/local-package-gate'
        with source.open('a') as gate:
            gate.write('\n# Changed since Check.\n')
        run = self.run_validation('--regress-prepared', plan)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('trusted test sources changed', run.stderr)
        self.assertFalse(self.log.exists())
        self.assertFalse(list(plan.glob('regression.*')))

    def test_failed_smoke_does_not_publish_update_proof_even_with_passing_receipt(self):
        plan, data, tx = self.sealed_validation_fixture()
        run = self.run_validation('--validate-prepared', plan, TEST_GATE_STATUS='7')
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('update smoke failed', run.stderr)
        failed = plan / 'failed/contracts'
        [attempt] = failed.glob('attempt.*')
        receipt = attempt / self.name / 'gate.json'
        self.assertIs(json.loads(receipt.read_text())['passed'], True)
        self.assertFalse(self.proof.exists())
        self.assertIsNone(tx['reusable_results'](plan, data))

        retry = self.run_validation('--validate-prepared', plan, TEST_GATE_STATUS='7', TEST_GATE_MISSING='1')
        self.assertNotEqual(retry.returncode, 0, retry.stdout + retry.stderr)
        [latest] = set(failed.glob('attempt.*')) - {attempt}
        self.assertIn(f'failed update smoke logs: {latest}', retry.stderr)
        self.assertTrue((latest / self.name / 'runner.log').is_file())
        self.assertFalse((latest / self.name / 'gate.json').exists())
        self.assertTrue(receipt.is_file())
        self.assertIsNone(tx['reusable_results'](plan, data))

    def test_zero_exit_without_matching_passed_smoke_proof_fails_validation(self):
        plan, data, tx = self.sealed_validation_fixture()
        for settings, reason in (({'TEST_GATE_MISSING': '1'}, 'No such file or directory'),
                                 ({'TEST_GATE_PASSED': 'false'}, 'passed=False'),
                                 ({'TEST_GATE_ID': self.regression_id}, f'id={self.regression_id!r}')):
            with self.subTest(settings=settings):
                run = self.run_validation('--validate-prepared', plan, **settings)
                self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn(reason, run.stderr)
                self.assertFalse(self.proof.exists())
                self.assertIsNone(tx['reusable_results'](plan, data))

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
