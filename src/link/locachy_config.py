"""Shared, durable configuration transactions for LOCachy.

The lock protocol is intentionally the same as base/rc.c: resolve and open the
stable ``TARGET.lock`` beside the target, flock it, read and validate the target,
transform a complete byte string, write a unique same-directory temporary, fsync
the temporary, replace the target, then fsync the directory; ``remove`` unlinks
under the same lock and fsyncs the directory.  These locks
coordinate LOCachy writers; arbitrary writers that ignore the lock are detected
when possible by the target identity/metadata check but cannot be excluded.
"""
from __future__ import annotations

import errno
import fcntl
import os
import stat
import tempfile
from typing import Callable, Iterable, Mapping, Optional, Union

RC_CONF = "/etc/locachy/rc.conf"
ANY = object()
UNCHANGED = object()


class ConflictError(OSError):
    """The expected version or target identity no longer matches."""

    def __init__(self, path: str):
        super().__init__(errno.ESTALE, os.strerror(errno.ESTALE), path)


class TransactionError(OSError):
    """A transaction could not publish its complete replacement."""


def _target(path: Union[str, os.PathLike[str]]) -> tuple[str, str, str]:
    raw = os.fspath(path)
    if not os.path.isabs(raw):
        raise ValueError("configuration path must be absolute")
    parent, name = os.path.split(raw)
    if not name or name in (".", ".."):
        raise ValueError("configuration path has no regular target name")
    parent = os.path.realpath(parent)
    if not os.path.isdir(parent):
        raise FileNotFoundError(parent)
    target = os.path.join(parent, name)
    lock = target + ".lock"
    return parent, target, lock


def _open_lock(lock_path: str) -> int:
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1 or st.st_uid != os.geteuid():
            raise OSError(errno.EINVAL, "configuration lock must be a private owned regular file", lock_path)
        os.fchmod(fd, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX)
        return fd
    except BaseException:
        os.close(fd)
        raise


def _regular_stat(target: str) -> Optional[os.stat_result]:
    """The target's lstat when it is a regular file, None when it is absent;
    a symlink or any other kind of file is refused."""
    try:
        before = os.lstat(target)
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(before.st_mode):
        raise OSError(errno.EINVAL, "configuration target is not a regular file", target)
    return before


def _snapshot(target: str) -> tuple[Optional[bytes], Optional[os.stat_result]]:
    before = _regular_stat(target)
    if before is None:
        return None, None
    fd = os.open(target, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        opened = os.fstat(fd)
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise ConflictError(target)
        chunks = []
        while True:
            chunk = os.read(fd, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        data = b"".join(chunks)
        if b"\0" in data:
            raise ValueError("configuration contains NUL bytes")
        return data, before
    finally:
        os.close(fd)


def _same(a: os.stat_result, b: os.stat_result) -> bool:
    return (a.st_dev, a.st_ino, a.st_mode, a.st_uid, a.st_gid, a.st_size,
            a.st_mtime_ns, a.st_ctime_ns) == (b.st_dev, b.st_ino, b.st_mode,
                                               b.st_uid, b.st_gid, b.st_size,
                                               b.st_mtime_ns, b.st_ctime_ns)


def _still_target(target: str, before: Optional[os.stat_result]) -> None:
    try:
        now = os.lstat(target)
    except FileNotFoundError:
        if before is None:
            return
        raise ConflictError(target)
    if before is None or not _same(now, before):
        raise ConflictError(target)


def _sync_directory(parent: str) -> None:
    dirfd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        os.fsync(dirfd)
    finally:
        os.close(dirfd)


def _publish(parent: str, target: str, data: bytes, before: Optional[os.stat_result]) -> None:
    _still_target(target, before)
    fd, temporary = tempfile.mkstemp(prefix=f".{os.path.basename(target)}.tmp.", dir=parent)
    try:
        view = memoryview(data)
        while view:
            n = os.write(fd, view)
            if n <= 0:
                raise OSError(errno.EIO, "incomplete configuration write", target)
            view = view[n:]
        if before is not None:
            os.fchown(fd, before.st_uid, before.st_gid)
        os.fchmod(fd, stat.S_IMODE(before.st_mode) if before is not None else 0o644)
        os.fsync(fd)
        os.close(fd)
        fd = -1
        _still_target(target, before)
        os.replace(temporary, target)
        temporary = ""
        _sync_directory(parent)
    except BaseException:
        if fd >= 0:
            os.close(fd)
        if temporary:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
        raise


def transaction(path: Union[str, os.PathLike[str]],
                callback: Callable[[Optional[bytes]], Union[bytes, bytearray, memoryview, object]]) -> bool:
    """Run callback under the shared lock and publish its complete bytes.

    callback receives bytes, or None when the target is absent. Return
    UNCHANGED to skip publication. Returns True when published and False when
    unchanged. The callback must not perform I/O that can wait for another
    LOCachy domain or reacquire this configuration lock.
    """
    parent, target, lock_path = _target(path)
    lockfd = _open_lock(lock_path)
    try:
        original, before = _snapshot(target)
        result = callback(original)
        if result is UNCHANGED:
            return False
        if result is None or not isinstance(result, (bytes, bytearray, memoryview)):
            raise TypeError("configuration transaction must return bytes or UNCHANGED")
        result = bytes(result)
        if b"\0" in result:
            raise ValueError("configuration contains NUL bytes")
        _publish(parent, target, result, before)
        return True
    finally:
        fcntl.flock(lockfd, fcntl.LOCK_UN)
        os.close(lockfd)


def _as_bytes(value: Optional[Union[str, bytes]]) -> Optional[bytes]:
    if value is None:
        return None
    if isinstance(value, bytes):
        out = value
    else:
        out = value.encode("utf-8")
    if any(c in out for c in (b'"', b"\r", b"\n", b"\0")):
        raise ValueError("configuration value contains quote or newline")
    return out


def _name(name: str) -> bytes:
    if not name or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_" for c in name):
        raise ValueError("invalid configuration name")
    return name.encode("ascii")


def _changes(changes: Union[Mapping[str, Optional[Union[str, bytes]]], Iterable[tuple[str, Optional[Union[str, bytes]]]]]):
    items = list(changes.items()) if isinstance(changes, Mapping) else list(changes)
    out = []
    seen = set()
    for name, value in items:
        key = _name(name)
        if key in seen:
            raise ValueError("duplicate configuration name")
        seen.add(key)
        out.append((key, _as_bytes(value)))
    return out


def _update_bytes(original: Optional[bytes], changes) -> bytes:
    data = original or b""
    changes_by_name = dict(changes)
    done = set()
    out = bytearray()
    parts = data.split(b"\n")
    for index, part in enumerate(parts):
        if index == len(parts) - 1 and not part:
            break
        line = part + (b"\n" if index < len(parts) - 1 else b"")
        body = line[:-1] if line.endswith(b"\n") else line
        body = body[:-1] if body.endswith(b"\r") else body
        pos = 0
        while pos < len(body) and body[pos] in b" \t":
            pos += 1
        end = pos
        while end < len(body) and (body[end:end + 1].isalnum() or body[end:end + 1] == b"_"):
            end += 1
        key = body[pos:end] if end > pos and end < len(body) and body[end:end + 1] == b"=" else None
        if key not in changes_by_name:
            out.extend(line)
        elif key not in done:
            done.add(key)
            value = changes_by_name[key]
            if value is not None:
                out.extend(key + b'=\"' + value + b'\"\n')
    if out and not out.endswith(b"\n"):
        out.extend(b"\n")
    for key, value in changes:
        if key not in done and value is not None:
            out.extend(key + b'=\"' + value + b'\"\n')
    return bytes(out)


def update(path: Union[str, os.PathLike[str]], changes, expected=ANY) -> bool:
    normalized = _changes(changes)

    def apply(original):
        if expected is not ANY:
            if expected is None:
                if original is not None:
                    raise ConflictError(os.fspath(path))
            elif original != bytes(expected):
                raise ConflictError(os.fspath(path))
        result = _update_bytes(original, normalized)
        if original is None and not result:
            return UNCHANGED  # only removals from an absent file: nothing to create
        return result

    return transaction(path, apply)


def compare_exchange(path: Union[str, os.PathLike[str]], expected, replacement: Union[bytes, bytearray, memoryview]) -> bool:
    expected_bytes = None if expected is None else bytes(expected)
    replacement_bytes = bytes(replacement)

    def apply(original):
        if expected is None:
            if original is not None:
                raise ConflictError(os.fspath(path))
        elif original != expected_bytes:
            raise ConflictError(os.fspath(path))
        return replacement_bytes

    return transaction(path, apply)


def remove(path: Union[str, os.PathLike[str]], expected=ANY) -> bool:
    """Remove a configuration file under its stable lock.

    expected: ANY removes the regular file that is there without reading it
    (a damaged one, with NUL bytes say, can still be removed); bytes removes
    only that exact content (ConflictError otherwise, nothing removed); None
    requires the file to be absent. Returns True when removed, False when it
    was already absent. Under the lock the target's identity and metadata are
    checked again just before the unlink (ConflictError when they changed).
    Symlinks and non-regular targets are refused, never removed. TARGET.lock
    stays: it is the stable lock and is never unlinked. When the directory
    fsync fails after the unlink, OSError is raised: the file is gone but its
    removal is not confirmed durable, and a retry returns False.
    A removal is no tombstone: a later writer creates the name again, so
    callers keep their domain lock (lock order: domain lock, then this lock).
    """
    parent, target, lock_path = _target(path)
    lockfd = _open_lock(lock_path)
    try:
        if expected is ANY:
            before = _regular_stat(target)
        else:
            original, before = _snapshot(target)
            if expected is None:
                if before is not None:
                    raise ConflictError(os.fspath(path))
                return False
            if before is None or original != bytes(expected):
                raise ConflictError(os.fspath(path))
        if before is None:
            return False
        _still_target(target, before)
        os.unlink(target)
        _sync_directory(parent)
        return True
    finally:
        fcntl.flock(lockfd, fcntl.LOCK_UN)
        os.close(lockfd)


def rc_update(changes, expected=ANY) -> bool:
    return update(RC_CONF, changes, expected=expected)
