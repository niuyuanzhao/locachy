#!/usr/bin/env python3
"""Source-key preflight with the target's repositories and no target writes.

Only metadata is fetched into a disposable database. locachy-build performs the
same AUR recipe/key lookup as a build, ignoring the live system's installed
packages. No recipe is executed and no source key is fetched or trusted.
"""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

KEYS_REFUSED = 3
FINGERPRINT = re.compile(r"[0-9A-Fa-f]{40}|[0-9A-Fa-f]{64}")
PACKAGE = re.compile(r"[A-Za-z0-9@_+][A-Za-z0-9@._+\-]*")
BUILDER = "locachy-builder"
HOME = Path("/var/lib/locachy/builder/home")


def load(source, name):
    loader = importlib.machinery.SourceFileLoader(name.replace('-', '_'), str(Path(source) / 'link' / name))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def in_net(command):
    return (["ip", "netns", "exec", "locachy-net"] if Path('/run/netns/locachy-net').exists() else []) + command


def identity():
    """The live session's build identity, without installing any package."""
    import pwd
    try:
        account = pwd.getpwnam(BUILDER)
    except KeyError:
        subprocess.run(['useradd', '--system', '--user-group', '--home-dir', str(HOME), '--no-create-home',
                        '--shell', '/usr/bin/nologin', '--comment', 'LOCachy builds', BUILDER], check=True)
        account = pwd.getpwnam(BUILDER)
    if account.pw_uid == 0 or account.pw_gid == 0:
        raise ValueError('the build identity must be unprivileged')
    HOME.parent.mkdir(parents=True, exist_ok=True)
    HOME.mkdir(mode=0o700, exist_ok=True)
    os.chown(HOME, account.pw_uid, account.pw_gid)
    return account


def key_root(target):
    target = Path(target) if target else Path('/')
    if target != Path('/'):
        passwd = target / 'etc/passwd'
        builder_home = target / HOME.relative_to('/')
        if passwd.is_file() and builder_home.is_dir() and any(
                line.startswith(BUILDER + ':') for line in passwd.read_text(encoding='utf-8').splitlines()):
            return target
    # An empty mounted target, or a resume that failed during bootstrap,
    # has no builder identity yet. Its untouched state is not an error.
    identity()
    return Path('/')


def query_view(source, directory, level, cpu, sections):
    """Fresh metadata in the same repository order as make-root, never -Sy on the live DB."""
    repos = load(source, 'locachy-repos')
    mirrors = directory / 'mirrors'
    mirrors.mkdir()
    for name in ('mirrorlist', 'cachyos-mirrorlist', 'cachyos-v3-mirrorlist', 'cachyos-v4-mirrorlist', 'alhp-mirrorlist'):
        path = Path('/etc/pacman.d') / name
        if path.is_file() and any(s.startswith('Server') for s in path.read_text().splitlines()):
            text = path.read_text().replace('$arch_v3', 'x86_64_v3').replace('$arch_v4', 'x86_64_v4')
        elif name == 'mirrorlist':
            raise ValueError('the live system has no Arch mirror list')
        elif name == 'alhp-mirrorlist':
            text = '\n'.join('Server = ' + url + '$repo/os/$arch/' for url in repos.ALHP_MIRRORS) + '\n'
        elif name == 'cachyos-mirrorlist':
            text = 'Server = https://cdn77.cachyos.org/repo/$arch/$repo\n'
        else:
            suffix = 'v3' if 'v3' in name else 'v4'
            text = 'Server = https://cdn77.cachyos.org/repo/x86_64_' + suffix + '/$repo\n'
        (mirrors / name).write_text(text)
    config = directory / 'pacman.conf'
    # Query-only HTTPS metadata is never published or used to install a
    # package. Do not import repository signers just to inspect membership;
    # make-root verifies the actual databases/packages with its keyring.
    config.write_text(repos.config(level, cpu, str(mirrors),
                      {'repo_' + s: '1' if s in sections else '0' for s in repos.SECTIONS}).replace(
                          'SigLevel = Required DatabaseOptional', 'SigLevel = Required DatabaseNever'))
    raw, merged = directory / 'raw', directory / 'merged'
    for path in (raw / 'local', merged / 'local'):
        path.mkdir(parents=True)
    command = in_net(['pacman', '--config', str(config), '--dbpath', str(raw), '--logfile', '/dev/null', '-Sy'])
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise RuntimeError('installation repository lookup failed: ' + (result.stderr or result.stdout)[-2000:])
    # libalpm initializes this marker while refreshing raw. The read-only,
    # unprivileged lookup cannot initialize a new local DB itself.
    shutil.copyfile(raw / 'local/ALPM_DB_VERSION', merged / 'local/ALPM_DB_VERSION')
    stack = load(source, 'locachy_stack.py')
    stack.merge(str(raw / 'sync'), str(merged / 'sync'), stack.repositories(str(config)))
    # Metadata is public. The lookup process is unprivileged and must reach it.
    for path in [directory, *directory.rglob('*')]:
        os.chmod(path, 0o755 if path.is_dir() else 0o644)
    return config, merged


def recipe_view(source, directory):
    """Copy the lookup inputs, not the private live checkout, for the build user."""
    source = Path(source)
    view = directory / 'source'
    (view / 'link').mkdir(parents=True)
    shutil.copyfile(source / 'link/locachy_upstream.py', view / 'link/locachy_upstream.py')
    shutil.copytree(source / 'pkgbuilds', view / 'pkgbuilds', symlinks=True)
    for path in sorted(view.rglob('*')):
        # Let the adapter reject links inside recipe trees; never chmod the
        # link's target or turn a link into a regular input by following it.
        if not path.is_symlink():
            path.chmod(0o755 if path.is_dir() else 0o644)
    view.chmod(0o755)
    return view


def check(source, runtime, packages, level, cpu, repos, target=None):
    packages = sorted(set(packages))
    if not packages or any(not PACKAGE.fullmatch(p) for p in packages):
        raise ValueError('invalid package roots for source-key preflight')
    root = key_root(target)
    # A target keyring is selected only for a continued target. Fresh installs
    # use the live disposable keyring; its public keys are preserved explicitly
    # after preflight, never merged by this check.
    with tempfile.TemporaryDirectory(prefix='locachy-install-keys-') as temporary:
        directory = Path(temporary)
        config, database = query_view(source, directory, level, cpu, repos)
        recipes = recipe_view(source, directory)
        command = [str(Path(source) / 'link/locachy-build'), '--check-keys', '--key-root', str(root),
                   '--installation-db', str(config), str(database), str(recipes), *packages]
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)
    if result.returncode == 0:
        print('Installation source-key preflight passed.', flush=True)
        return 0
    keys = []
    if result.returncode == KEYS_REFUSED:
        for line in result.stdout.splitlines():
            fields = line.split('\t')
            if len(fields) == 5 and PACKAGE.fullmatch(fields[1]) and FINGERPRINT.fullmatch(fields[2]):
                keys.append(fields)
    if not keys:
        raise RuntimeError('source signing keys could not be checked: ' + (result.stderr or result.stdout)[-2000:])
    for needed, recipe, fingerprint, upstream, signed in keys:
        print(f'Source key to trust: {recipe} {fingerprint.upper()}')
        print(f'  Packages: {needed}\n  Upstream: {upstream}\n  Signed sources: {signed}')
        print('  Recipe: https://aur.archlinux.org/cgit/aur.git/tree/.SRCINFO?h=' + recipe)
    print('Check each fingerprint with its upstream project. No key was added automatically.')
    print('Trust them: locachy-install --trust-key ' + ' '.join(sorted({k[2].upper() for k in keys})))
    return KEYS_REFUSED


def trust(source, runtime, fingerprints, target=None):
    keys = [key.upper() for key in fingerprints]
    if not keys or any(not FINGERPRINT.fullmatch(key) for key in keys):
        raise ValueError('invalid source signing fingerprint')
    root = key_root(target)
    if root != Path('/'):
        command = [str(Path(source) / 'link/locachy-build'), '--trust-key', '--key-root', str(root), *keys]
    else:
        identity()
        command = [str(Path(source) / 'link/locachy-build'), '--trust-key', *keys]
    return subprocess.run(command).returncode


def preserve(target):
    """Transfer already approved public keys after target bootstrap, before builds.

    Only public certificates cross the boundary; no secret keys, trust database,
    daemon socket or live UID is copied. Repeating an interrupted transfer is safe.
    """
    original = HOME / '.gnupg'
    if not original.is_dir():
        return
    account = identity()
    live = ['setpriv', '--reuid=' + str(account.pw_uid), '--regid=' + str(account.pw_gid),
            '--clear-groups', '--reset-env', 'env', 'HOME=' + str(HOME), 'GNUPGHOME=' + str(original)]
    installed = ['chroot', str(target), 'setpriv', '--reuid=' + BUILDER, '--regid=' + BUILDER,
                 '--init-groups', '--reset-env', 'env', 'HOME=' + str(HOME), 'GNUPGHOME=' + str(HOME / '.gnupg')]
    try:
        public = subprocess.run(live + ['gpg', '--batch', '--export'], check=True, capture_output=True).stdout
        if public:
            subprocess.run(installed + ['install', '-d', '-m', '0700', str(HOME / '.gnupg')], check=True)
            subprocess.run(installed + ['gpg', '--batch', '--import'], input=public, check=True)
    finally:
        for command in (live, installed):
            subprocess.run(command + ['gpgconf', '--kill', 'all'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != 'preserve':
        raise SystemExit('usage: locachy_install_keys.py preserve TARGET')
    preserve(Path(sys.argv[2]))
