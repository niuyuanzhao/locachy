#!/usr/bin/env python3
"""Repair only the installed updater from an explicitly checked local file.

Run in LOCachy's managed administrator terminal as root:
  python3 -I repair-updater.py CANDIDATE FULL_SHA256

No download, package transaction, source commit update, service restart or
network-policy change is performed here. The next package job uses the new
script; an active package operation refuses this repair rather than being killed.
"""
import argparse
import ast
import fcntl
import hashlib
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

MAX_BYTES = 2 * 1024 * 1024
TARGET = Path('/usr/local/bin/locachy-pkg')
LOCK = Path('/run/locachy/pkg.lock')


def read_regular(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= MAX_BYTES:
            raise ValueError('not a bounded regular updater file: ' + str(path))
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            data = stream.read(MAX_BYTES + 1)
        if len(data) != info.st_size:
            raise ValueError('updater file changed while it was read: ' + str(path))
        return data, info
    finally:
        os.close(fd)


def checked_directory(path):
    if path.resolve(strict=True) != path:
        raise ValueError('repair directory must not contain symlinks: ' + str(path))
    info = path.stat()
    if info.st_uid != os.geteuid() or info.st_mode & 0o022:
        raise ValueError('repair directory must be owned by root and not writable by others: ' + str(path))


def repair(candidate, expected, root=Path('/')):
    # root is an internal fixture seam, not a CLI switch or environment bypass.
    if not re.fullmatch(r'[0-9a-fA-F]{64}', expected):
        raise ValueError('expected SHA256 must be the full 64 hexadecimal characters')
    data, _ = read_regular(candidate)
    if hashlib.sha256(data).hexdigest() != expected.lower():
        raise ValueError('candidate SHA256 does not match; installed updater left untouched')
    if not data.startswith(b'#!/usr/bin/env python3\n'):
        raise ValueError('candidate is not the Python updater')
    ast.parse(data, filename='locachy-pkg')  # Syntax only: never execute the candidate here.
    target, lock = root / TARGET.relative_to('/'), root / LOCK.relative_to('/')
    checked_directory(target.parent)
    checked_directory(lock.parent)
    fd = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ValueError('package domain is not an owned regular lock file')
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('a package or repository job is running; nothing changed; wait for it to finish') from error
        if (root / 'var/lib/pacman/db.lck').exists():
            raise RuntimeError('pacman database is locked; nothing changed; do not remove its lock')
        before, original = read_regular(target)
        if original.st_uid != os.geteuid() or original.st_mode & 0o022:
            raise ValueError('installed updater is not an owned, protected regular file')
        if before == data:
            print('Updater already matches the checked repair; nothing changed.')
            return
        old_hash = hashlib.sha256(before).hexdigest()
        backup = target.with_name('.locachy-pkg.' + old_hash + '.before-repair')
        if backup.exists() or backup.is_symlink():
            saved, saved_info = read_regular(backup)
            if saved != before or saved_info.st_uid != os.geteuid() or saved_info.st_mode & 0o022:
                raise ValueError('previous updater backup is not the expected protected file')
        else:
            # A hard link preserves the complete previous inode for explicit rollback.
            os.link(target, backup, follow_symlinks=False)
        staged_fd, name = tempfile.mkstemp(prefix='.locachy-pkg.repair-', dir=target.parent)
        try:
            with os.fdopen(staged_fd, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fchmod(stream.fileno(), 0o755)
                os.fsync(stream.fileno())
            current = target.lstat()
            if (current.st_dev, current.st_ino, current.st_mtime_ns, current.st_size) != (
                    original.st_dev, original.st_ino, original.st_mtime_ns, original.st_size):
                raise RuntimeError('installed updater changed during repair; left untouched')
            os.replace(name, target)
            directory = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            if os.path.exists(name):
                os.unlink(name)
        print('Updater replaced atomically. Previous file: ' + str(backup))
        print('No packages or services changed. Retry Check in packages; its next job uses this updater.')
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('sha256')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run in the managed administrator terminal as root')
    try:
        repair(args.candidate, args.sha256)
    except (OSError, ValueError, SyntaxError, RuntimeError) as error:
        print('Updater repair failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
