#!/usr/bin/env python3
"""Shared native multi-disk storage volume library for LOCachy.

Manages secondary internal disks (NVMe, SATA, SAS, VirtIO, etc.) and multi-disk
Btrfs arrays without relying on volatile kernel device paths (/dev/nvmeXn1 or
/dev/sdX). Every volume member is tracked in /etc/locachy/volumes.json by
GPT PARTUUID and LUKS2/filesystem UUID, mounted under /srv/storage/<name>, and
optionally bind-projected into ~/Drives/<name> depending on its role:

  - data:    desktop & system data (/srv/storage/<name> + ~/Drives/<name>);
             refused by locachy-compute --volume and compute_storage.
  - compute: compute-only storage (/srv/storage/<name> only, never projected
             into ~/Drives); allowed for locachy-compute.
  - shared:  dual access (/srv/storage/<name> + ~/Drives/<name>) and allowed
             for locachy-compute.
"""
from __future__ import annotations

import ctypes
import datetime
import errno
import fcntl
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any

sys.dont_write_bytecode = True

SCHEMA_VERSION = 1

DEFAULT_VOLUMES_FILE = "/etc/locachy/volumes.json"
DEFAULT_KEYS_DIR = "/etc/locachy/keys"
DEFAULT_STORAGE_ROOT = "/srv/storage"
DEFAULT_RC_CONF = "/etc/locachy/rc.conf"
DEFAULT_COMPUTE_BASE = "/var/lib/locachy/compute"
DEFAULT_WINMAINT_STATE = "/var/lib/locachy/winmaint/state.json"
DEFAULT_USER_UID = 1000

SYS_BLOCK_DIR = "/sys/class/block"
SYS_DM_MISC_DEV = "/sys/class/misc/device-mapper/dev"
DEV_DIR = "/dev"
DEV_MAPPER_DIR = "/dev/mapper"
RUN_CRYPTSETUP_DIR = "/run/cryptsetup"

# Maintenance Windows (locachy-winmaint): the GPT names of its partitions (it finds
# its triplet by them), and the reason a disk holding them is never formatted.
WINMAINT_PART_LABELS = ("LOCACHY_WIN_ESP", "LOCACHY_WIN_MSR", "LOCACHY_WIN_C")
WINMAINT_REASON = "Maintenance Windows"

VOLUME_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,31}$")
MAPPER_NAME_RE = re.compile(r"^locachy-vol-[a-z0-9][a-z0-9._-]{0,31}-[0-9]{1,3}$")
UUID_RE = re.compile(r"^[0-9A-Za-z-]{4,64}$")

VALID_TOPOLOGIES = ("single", "btrfs-array")
VALID_FS = ("ext4", "btrfs", "xfs", "ntfs", "exfat", "vfat")
VALID_FORMAT_FS = ("ext4", "btrfs")
VALID_RAIDS = ("single", "raid0", "raid1", "raid10")
VALID_ROLES = ("data", "shared", "compute")
VALID_ENCRYPT = ("none", "luks")
VALID_MODES = ("format", "attach", "array")

# GPT partition type GUID for Linux filesystem data.
GPT_TYPE_LINUX_DATA = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
GPT_ENTRY_BYTES = 16384          # 128 entries x 128 bytes
PART_ALIGN_BYTES = 1048576       # 1 MiB partition alignment
MIN_VOLUME_BYTES = 1073741824    # a volume keeps at least 1 GiB

# Linux mount(2) flags.
MS_RDONLY = 1
MS_NOSUID = 2
MS_NODEV = 4
MS_NOEXEC = 8
MS_REMOUNT = 32
MS_NOATIME = 1024
MS_NODIRATIME = 2048
MS_BIND = 4096
MS_REC = 16384
MS_SHARED = 1 << 20
MS_RELATIME = 1 << 21
MS_STRICTATIME = 1 << 24
MNT_DETACH = 2
UMOUNT_NOFOLLOW = 8

# Protected mount points whose backing physical disks may never be formatted or
# claimed as new volumes.
SYSTEM_MOUNT_PREFIXES = (
    "/",
    "/efi",
    "/boot",
    "/run/archiso",
    "/run/initramfs",
)


class StorageError(RuntimeError):
    """Raised on invalid storage configuration, policy violation, or OS failure."""


def _utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _abs_env(name: str, default: str) -> Path:
    val = os.environ.get(name, default).strip()
    if not val or not os.path.isabs(val):
        raise StorageError(f"{name} must be an absolute path: {val!r}")
    return Path(val)


def _target_subpath(root: str | os.PathLike[str] | None, absolute_path: str) -> Path:
    if root is None:
        return Path(absolute_path)
    root_p = Path(root)
    if not root_p.is_absolute():
        raise StorageError(f"target root must be an absolute path: {root!r}")
    if str(root_p) == "/":
        return Path(absolute_path)
    return root_p / absolute_path.lstrip("/")


def volumes_file_path(root: str | os.PathLike[str] | None = None) -> Path:
    if root is not None and str(Path(root)) != "/":
        return _target_subpath(root, DEFAULT_VOLUMES_FILE)
    return _abs_env("LOCACHY_VOLUMES_FILE", DEFAULT_VOLUMES_FILE)


def keys_dir_path(root: str | os.PathLike[str] | None = None) -> Path:
    if root is not None and str(Path(root)) != "/":
        return _target_subpath(root, DEFAULT_KEYS_DIR)
    return _abs_env("LOCACHY_KEYS_DIR", DEFAULT_KEYS_DIR)


def storage_root_path(root: str | os.PathLike[str] | None = None) -> Path:
    if root is not None and str(Path(root)) != "/":
        return _target_subpath(root, DEFAULT_STORAGE_ROOT)
    return _abs_env("LOCACHY_STORAGE_ROOT", DEFAULT_STORAGE_ROOT)


def rc_conf_path(root: str | os.PathLike[str] | None = None) -> Path:
    if root is not None and str(Path(root)) != "/":
        return _target_subpath(root, DEFAULT_RC_CONF)
    for var in ("LOCACHY_STORAGE_RC", "LOCACHY_RC_CONF"):
        raw = os.environ.get(var, "").strip()
        if raw:
            if not os.path.isabs(raw):
                raise StorageError(f"{var} must be an absolute path: {raw!r}")
            return Path(raw)
    return Path(DEFAULT_RC_CONF)


def winmaint_part_uuids(root: str | os.PathLike[str] | None = None) -> set[str]:
    """PARTUUIDs of the Maintenance Windows partitions locachy-winmaint recorded for this
    system (or for TARGET_ROOT) in its state.json; none without one."""
    try:
        state = json.loads(_target_subpath(root, DEFAULT_WINMAINT_STATE).read_text(encoding="utf-8"))
        parts = state.get("partitions") or {}
        return {str(p["part_guid"]).lower() for p in parts.values() if isinstance(p, dict) and p.get("part_guid")}
    except (OSError, ValueError, AttributeError):
        return set()


def is_winmaint_partition(node: dict[str, Any], recorded: set[str] | frozenset[str] = frozenset()) -> bool:
    """A partition of Maintenance Windows: by its GPT name, or by a PARTUUID recorded in state.json."""
    if node.get("type") != "part":
        return False
    return (str(node.get("partlabel") or "") in WINMAINT_PART_LABELS
            or str(node.get("partuuid") or "").lower() in recorded)


def drives_dir_path(root: str | os.PathLike[str] | None = None) -> Path:
    if root is None or str(Path(root)) == "/":
        env_val = os.environ.get("LOCACHY_DRIVES_DIR", "").strip()
        if env_val:
            if not os.path.isabs(env_val):
                raise StorageError(f"LOCACHY_DRIVES_DIR must be an absolute path: {env_val!r}")
            return Path(env_val)
    home = _primary_user_home(root=root)
    if not home:
        raise StorageError(f"cannot determine primary user home directory (uid {DEFAULT_USER_UID})")
    if root is not None and str(Path(root)) != "/":
        return _target_subpath(root, str(Path(home) / "Drives"))
    return Path(home) / "Drives"


def _primary_user_home(root: str | os.PathLike[str] | None = None) -> str:
    if root is not None and str(Path(root)) != "/":
        passwd_file = _target_subpath(root, "/etc/passwd")
        if passwd_file.is_file():
            for line in passwd_file.read_text(encoding="utf-8", errors="replace").splitlines():
                parts = line.split(":")
                if len(parts) >= 6 and parts[2] == str(DEFAULT_USER_UID):
                    return parts[5]
    try:
        return pwd.getpwuid(DEFAULT_USER_UID).pw_dir
    except KeyError:
        return ""


def _primary_user_uid_gid() -> tuple[int, int]:
    try:
        pw = pwd.getpwuid(DEFAULT_USER_UID)
        return (pw.pw_uid, pw.pw_gid)
    except KeyError:
        return (DEFAULT_USER_UID, DEFAULT_USER_UID)


def _fsync_dir(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def _atomic_write_bytes(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
        tmp = ""
        _fsync_dir(path.parent)
    finally:
        if tmp:
            try:
                os.unlink(tmp)
            except OSError:
                pass


def _ensure_keys_dir(kdir: Path) -> None:
    if kdir.exists():
        st = os.lstat(kdir)
        if not stat.S_ISDIR(st.st_mode):
            raise StorageError(f"keys path is not a directory: {kdir}")
        if st.st_mode & 0o077:
            os.chmod(kdir, 0o700)
    else:
        kdir.parent.mkdir(parents=True, exist_ok=True)
        os.mkdir(kdir, 0o700)
        os.chmod(kdir, 0o700)
        _fsync_dir(kdir.parent)


def _write_keyfile(path: Path, key_bytes: bytes) -> None:
    _ensure_keys_dir(path.parent)
    _atomic_write_bytes(path, key_bytes, mode=0o600)


def _lock_volumes(vfile: Path) -> int:
    vfile.parent.mkdir(parents=True, exist_ok=True)
    lock_path = vfile.parent / f".{vfile.name}.lock"
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        os.fchmod(fd, 0o600)
    except OSError:
        pass
    fcntl.flock(fd, fcntl.LOCK_EX)
    return fd


def _unlock_fd(fd: int) -> None:
    try:
        fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def _run(
    cmd: list[str],
    *,
    input_bytes: bytes | None = None,
    check: bool = True,
    timeout: int = 300,
) -> subprocess.CompletedProcess:
    env = dict(os.environ, LC_ALL="C")
    try:
        kwargs: dict[str, Any] = {
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
            "timeout": timeout,
            "env": env,
            "check": False,
        }
        if input_bytes is not None:
            kwargs["input"] = input_bytes
        else:
            kwargs["stdin"] = subprocess.DEVNULL
        proc = subprocess.run(cmd, **kwargs)
    except FileNotFoundError as exc:
        raise StorageError(f"required tool not found: {cmd[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        raise StorageError(f"command timed out ({timeout}s): {' '.join(cmd)}") from exc
    if check and proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        out = proc.stdout.decode("utf-8", errors="replace").strip()
        detail = err or out or f"exit {proc.returncode}"
        raise StorageError(f"{' '.join(cmd)}: {detail}")
    return proc


# ---- Schema validation & config I/O ----

def validate_volume_name(name: str) -> str:
    if not isinstance(name, str) or name in (".", "..") or not VOLUME_NAME_RE.fullmatch(name):
        raise StorageError(
            f"invalid volume name {name!r}: must match ^[a-z0-9][a-z0-9._-]{{0,31}}$"
        )
    return name


def validate_role(role: str) -> str:
    if role not in VALID_ROLES:
        raise StorageError(f"invalid role {role!r}; expected one of {', '.join(VALID_ROLES)}")
    return role


def validate_volume_entry(vol: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(vol, dict):
        raise StorageError("volume entry must be a JSON object")
    name = validate_volume_name(str(vol.get("name", "")))
    topology = str(vol.get("topology", ""))
    if topology not in VALID_TOPOLOGIES:
        raise StorageError(f"volume {name!r}: invalid topology {topology!r}")
    fs = str(vol.get("fs", ""))
    if fs not in VALID_FS:
        raise StorageError(f"volume {name!r}: invalid filesystem {fs!r}")
    raid = str(vol.get("raid", "") or "")
    if topology == "single":
        if raid != "":
            raise StorageError(f"volume {name!r}: single topology must have empty raid field, got {raid!r}")
    else:
        if fs != "btrfs":
            raise StorageError(f"volume {name!r}: btrfs-array topology requires fs='btrfs', got {fs!r}")
        if raid not in VALID_RAIDS:
            raise StorageError(
                f"volume {name!r}: btrfs-array requires raid in {', '.join(VALID_RAIDS)}, got {raid!r}"
            )
    role = validate_role(str(vol.get("role", "")))
    encrypt = str(vol.get("encrypt", "none"))
    if encrypt not in VALID_ENCRYPT:
        raise StorageError(f"volume {name!r}: invalid encrypt mode {encrypt!r}")
    auto_mount = vol.get("auto_mount", True)
    if not isinstance(auto_mount, bool):
        raise StorageError(f"volume {name!r}: auto_mount must be a boolean")
    fs_uuid = str(vol.get("fs_uuid", "") or "")
    if fs_uuid and not UUID_RE.fullmatch(fs_uuid):
        raise StorageError(f"volume {name!r}: invalid fs_uuid {fs_uuid!r}")
    members = vol.get("members")
    if not isinstance(members, list) or not members:
        raise StorageError(f"volume {name!r}: members must be a non-empty list")
    if topology == "single" and len(members) != 1:
        raise StorageError(f"volume {name!r}: single topology requires exactly 1 member, got {len(members)}")
    if topology == "btrfs-array":
        min_members = 4 if raid == "raid10" else 2
        if len(members) < min_members:
            raise StorageError(
                f"volume {name!r}: btrfs-array with raid={raid!r} requires at least {min_members} members, got {len(members)}"
            )

    clean_members: list[dict[str, str]] = []
    seen_part_uuids: set[str] = set()
    seen_luks_uuids: set[str] = set()
    seen_mappers: set[str] = set()
    for idx, mem in enumerate(members):
        if not isinstance(mem, dict):
            raise StorageError(f"volume {name!r}: member {idx} must be a JSON object")
        part_uuid = str(mem.get("part_uuid", "") or "")
        luks_uuid = str(mem.get("luks_uuid", "") or "")
        mapper = str(mem.get("mapper", "") or "")
        keyfile = str(mem.get("keyfile", "") or "")
        if part_uuid and not UUID_RE.fullmatch(part_uuid):
            raise StorageError(f"volume {name!r}: member {idx} has invalid part_uuid {part_uuid!r}")
        if luks_uuid and not UUID_RE.fullmatch(luks_uuid):
            raise StorageError(f"volume {name!r}: member {idx} has invalid luks_uuid {luks_uuid!r}")
        if not part_uuid and not luks_uuid and not fs_uuid:
            raise StorageError(
                f"volume {name!r}: member {idx} must identify a device via part_uuid, luks_uuid, or volume fs_uuid"
            )
        if encrypt == "luks":
            if not luks_uuid and not part_uuid:
                raise StorageError(f"volume {name!r}: LUKS member {idx} requires luks_uuid or part_uuid")
            if not mapper or "/" in mapper or ".." in mapper:
                raise StorageError(f"volume {name!r}: LUKS member {idx} has invalid mapper name {mapper!r}")
            if keyfile and not os.path.isabs(keyfile):
                raise StorageError(f"volume {name!r}: LUKS member {idx} keyfile must be absolute: {keyfile!r}")
            if mapper in seen_mappers:
                raise StorageError(f"volume {name!r}: duplicate mapper name {mapper!r}")
            seen_mappers.add(mapper)
        if part_uuid:
            pu_low = part_uuid.lower()
            if pu_low in seen_part_uuids:
                raise StorageError(f"volume {name!r}: duplicate member part_uuid {part_uuid!r}")
            seen_part_uuids.add(pu_low)
        if luks_uuid:
            lu_low = luks_uuid.lower()
            if lu_low in seen_luks_uuids:
                raise StorageError(f"volume {name!r}: duplicate member luks_uuid {luks_uuid!r}")
            seen_luks_uuids.add(lu_low)
        clean_members.append(
            {
                "part_uuid": part_uuid,
                "luks_uuid": luks_uuid,
                "mapper": mapper,
                "keyfile": keyfile,
            }
        )

    mount_options = str(vol.get("mount_options", "") or "")
    if any(ch in mount_options for ch in ("\n", "\r", "\x00")):
        raise StorageError(f"volume {name!r}: invalid characters in mount_options")
    created_at = str(vol.get("created_at", "") or _utc_now())
    return {
        "name": name,
        "topology": topology,
        "fs": fs,
        "raid": raid,
        "role": role,
        "encrypt": encrypt,
        "auto_mount": auto_mount,
        "fs_uuid": fs_uuid,
        "members": clean_members,
        "mount_options": mount_options,
        "created_at": created_at,
    }


def validate_config(data: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(data, dict):
        raise StorageError("volumes config must be a JSON object")
    schema = data.get("schema")
    if schema != SCHEMA_VERSION:
        raise StorageError(f"unsupported volumes.json schema: {schema!r} (expected {SCHEMA_VERSION})")
    raw_vols = data.get("volumes")
    if not isinstance(raw_vols, list):
        raise StorageError("volumes.json 'volumes' must be a list")
    clean: list[dict[str, Any]] = []
    seen_names: set[str] = set()
    for item in raw_vols:
        vol = validate_volume_entry(item)
        if vol["name"] in seen_names:
            raise StorageError(f"duplicate volume name: {vol['name']!r}")
        seen_names.add(vol["name"])
        clean.append(vol)
    return {"schema": SCHEMA_VERSION, "volumes": clean}


def load_config(root: str | os.PathLike[str] | None = None) -> dict[str, Any]:
    vfile = volumes_file_path(root=root)
    if not vfile.is_file():
        return {"schema": SCHEMA_VERSION, "volumes": []}
    try:
        raw = json.loads(vfile.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise StorageError(f"cannot read {vfile}: {exc}") from exc
    return validate_config(raw)


def save_config(cfg: dict[str, Any], root: str | os.PathLike[str] | None = None) -> Path:
    validated = validate_config(cfg)
    vfile = volumes_file_path(root=root)
    payload = (json.dumps(validated, indent=2, sort_keys=False) + "\n").encode("utf-8")
    _atomic_write_bytes(vfile, payload, mode=0o644)
    return vfile


def get_volume(name: str, root: str | os.PathLike[str] | None = None) -> dict[str, Any] | None:
    validate_volume_name(name)
    cfg = load_config(root=root)
    for vol in cfg["volumes"]:
        if vol["name"] == name:
            return vol
    return None


def volume_mountpoint(name: str, root: str | os.PathLike[str] | None = None) -> Path:
    validate_volume_name(name)
    return storage_root_path(root=root) / name


def volume_drives_path(name: str, root: str | os.PathLike[str] | None = None) -> Path:
    validate_volume_name(name)
    return drives_dir_path(root=root) / name


# ---- rc.conf helpers ----

def read_rc_value(key: str, root: str | os.PathLike[str] | None = None) -> str:
    path = rc_conf_path(root=root)
    if not path.is_file():
        return ""
    found = ""
    pat = re.compile(rf'^[ \t]*{re.escape(key)}="([^"\n]*)"')
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pat.match(line)
        if m:
            found = m.group(1)
    return found


def write_rc_value(key: str, value: str | None, root: str | os.PathLike[str] | None = None) -> None:
    if not re.fullmatch(r"[A-Za-z0-9_]+", key):
        raise StorageError(f"invalid rc.conf key: {key!r}")
    if value is not None and any(ch in value for ch in ('"', "\n", "\r", "\x00")):
        raise StorageError(f"invalid rc.conf value for {key}: {value!r}")
    path = rc_conf_path(root=root)
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines() if path.is_file() else []
    pat = re.compile(rf"^[ \t]*{re.escape(key)}=")
    out: list[str] = []
    replaced = False
    for line in lines:
        if pat.match(line):
            if value is not None and not replaced:
                out.append(f'{key}="{value}"')
                replaced = True
            continue
        out.append(line)
    if value is not None and not replaced:
        out.append(f'{key}="{value}"')
    text = ("\n".join(out) + ("\n" if out else "")).encode("utf-8")
    _atomic_write_bytes(path, text, mode=0o644)


# ---- Block device discovery & UUID/PARTUUID resolution ----

def _probe_blkid_export(path: str) -> dict[str, str]:
    """Low-level direct superblock & partition probe (bypasses udev DB and blkid cache)."""
    if not path or not os.path.exists(path):
        return {}
    try:
        proc = _run(["blkid", "-p", "-o", "export", path], check=False, timeout=15)
    except StorageError:
        return {}
    if proc.returncode != 0:
        return {}
    props: dict[str, str] = {}
    for line in proc.stdout.decode("utf-8", errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            props[k.strip()] = v.strip()
    return props


def _probe_sfdisk_parts(disk_path: str) -> dict[str, dict[str, str]]:
    """Read GPT partition UUIDs/labels directly from disk via sfdisk (pre-udev fallback)."""
    if not disk_path or not os.path.exists(disk_path):
        return {}
    try:
        proc = _run(["sfdisk", "--json", disk_path], check=False, timeout=15)
    except StorageError:
        return {}
    if proc.returncode != 0:
        return {}
    try:
        data = json.loads(proc.stdout.decode("utf-8", errors="replace"))
    except ValueError:
        return {}
    out: dict[str, dict[str, str]] = {}
    pt = data.get("partitiontable") or {}
    for part in pt.get("partitions") or []:
        node = str(part.get("node") or "")
        if not node:
            continue
        out[node] = {
            "partuuid": str(part.get("uuid") or ""),
            "partlabel": str(part.get("name") or ""),
        }
    return out


def _read_sysfs_partname(kname: str) -> str:
    if not kname:
        return ""
    uevent = Path(SYS_BLOCK_DIR) / kname / "uevent"
    try:
        for line in uevent.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("PARTNAME="):
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def _enrich_blockdevices_blkid(tree: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Fill missing UUID/PARTUUID/FSTYPE/LABEL/PARTLABEL before systemd-udevd starts.

    init runs `locachy-storage mount-all` before starting udev-client, so lsblk's
    udev DB lookup returns nulls. Only probes nodes that actually lack metadata,
    leaving populated udev results untouched.
    """
    sfdisk_cache: dict[str, dict[str, dict[str, str]]] = {}

    def _walk(node: dict[str, Any], top_disk: str) -> None:
        dev_type = str(node.get("type") or "")
        path = str(node.get("path") or node.get("name") or "")
        kname = str(node.get("kname") or (os.path.basename(path) if path else ""))
        children = node.get("children") or []
        cur_top = path if dev_type == "disk" else top_disk

        need_blkid = False
        if path.startswith("/dev/"):
            if dev_type == "part":
                need_blkid = not node.get("uuid") or not node.get("partuuid") or not node.get("fstype")
            elif dev_type in ("disk", "crypt") and not children:
                need_blkid = not node.get("uuid") and not node.get("fstype")

        if need_blkid:
            props = _probe_blkid_export(path)
            if props:
                if not node.get("uuid") and props.get("UUID"):
                    node["uuid"] = props["UUID"]
                if not node.get("fstype") and props.get("TYPE"):
                    node["fstype"] = props["TYPE"]
                if not node.get("label") and props.get("LABEL"):
                    node["label"] = props["LABEL"]
                if not node.get("partuuid") and props.get("PART_ENTRY_UUID"):
                    node["partuuid"] = props["PART_ENTRY_UUID"]
                if not node.get("partlabel") and props.get("PART_ENTRY_NAME"):
                    node["partlabel"] = props["PART_ENTRY_NAME"]

        if dev_type == "part" and path.startswith("/dev/"):
            if (not node.get("partuuid") or not node.get("partlabel")) and cur_top:
                if cur_top not in sfdisk_cache:
                    sfdisk_cache[cur_top] = _probe_sfdisk_parts(cur_top)
                pinfo = sfdisk_cache[cur_top].get(path) or {}
                if not node.get("partuuid") and pinfo.get("partuuid"):
                    node["partuuid"] = pinfo["partuuid"]
                if not node.get("partlabel") and pinfo.get("partlabel"):
                    node["partlabel"] = pinfo["partlabel"]
            if not node.get("partlabel") and kname:
                pname = _read_sysfs_partname(kname)
                if pname:
                    node["partlabel"] = pname

        for child in children:
            _walk(child, cur_top)

    for root_node in tree:
        _walk(root_node, "")
    return tree


def probe_lsblk(fixture_path: str | os.PathLike[str] | None = None) -> list[dict[str, Any]]:
    """Return raw lsblk blockdevices tree (or injectable JSON fixture for tests)."""
    env_fix = fixture_path or os.environ.get("LOCACHY_STORAGE_LSBLK_JSON", "").strip()
    if env_fix:
        raw = json.loads(Path(env_fix).read_text(encoding="utf-8"))
        tree = list(raw.get("blockdevices", [])) if isinstance(raw, dict) else list(raw)
        if os.environ.get("LOCACHY_STORAGE_ENRICH_FIXTURE") == "1":
            _enrich_blockdevices_blkid(tree)
        return tree
    proc = _run(
        [
            "lsblk",
            "--json",
            "--bytes",
            "--paths",
            "--output",
            "PATH,NAME,KNAME,TYPE,PKNAME,PARTUUID,PARTLABEL,UUID,FSTYPE,LABEL,SIZE,START,LOG-SEC,MODEL,SERIAL,TRAN,"
            "MOUNTPOINTS,RO,RM",
        ],
        check=True,
        timeout=30,
    )
    parsed = json.loads(proc.stdout.decode("utf-8", errors="replace"))
    tree = list(parsed.get("blockdevices", []))
    _enrich_blockdevices_blkid(tree)
    return tree


def flatten_blockdevices(tree: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Flatten lsblk blockdevices tree, annotating each node with its top disk."""
    flat: list[dict[str, Any]] = []

    def _walk(node: dict[str, Any], top_disk: str, parent_path: str) -> None:
        item = dict(node)
        children = item.pop("children", None) or []
        path = str(item.get("path") or item.get("name") or "")
        dev_type = str(item.get("type") or "")
        cur_top = path if dev_type == "disk" else top_disk
        item["path"] = path
        item["_top_disk"] = cur_top or path
        item["_parent_path"] = parent_path
        mps = item.get("mountpoints")
        if mps is None:
            mp = item.get("mountpoint")
            mps = [mp] if mp else []
        item["mountpoints"] = [m for m in mps if m]
        flat.append(item)
        for child in children:
            _walk(child, cur_top or path, path)

    for root_node in tree:
        _walk(root_node, "", "")
    return flat


def _parse_mountinfo() -> list[dict[str, Any]]:
    """Parse /proc/self/mountinfo (or injectable test fixture)."""
    mi_path = os.environ.get("LOCACHY_STORAGE_MOUNTINFO", "/proc/self/mountinfo")
    entries: list[dict[str, Any]] = []
    try:
        lines = Path(mi_path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return entries
    for line in lines:
        sep = line.find(" - ")
        if sep < 0:
            continue
        left = line[:sep].split()
        right = line[sep + 3 :].split()
        if len(left) < 5 or len(right) < 2:
            continue
        mp = _unescape_mount_path(left[4])
        fstype = right[0]
        source = _unescape_mount_path(right[1])
        opts = right[2] if len(right) > 2 else ""
        entries.append(
            {
                "mount_id": left[0],
                "parent_id": left[1],
                "majmin": left[2],
                "root": _unescape_mount_path(left[3]),
                "mountpoint": mp,
                "mount_opts": left[5] if len(left) > 5 else "",
                "fstype": fstype,
                "source": source,
                "super_opts": opts,
            }
        )
    return entries


def _unescape_mount_path(raw: str) -> str:
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), raw)


def is_mountpoint(path: Path | str) -> bool:
    target = os.path.normpath(str(path))
    for entry in _parse_mountinfo():
        if os.path.normpath(entry["mountpoint"]) == target:
            return True
    return False


def mountinfo_for(path: Path | str) -> dict[str, Any] | None:
    target = os.path.normpath(str(path))
    found = None
    for entry in _parse_mountinfo():
        if os.path.normpath(entry["mountpoint"]) == target:
            found = entry
    return found


def root_is_encrypted(blockdevices: list[dict[str, Any]] | None = None) -> bool:
    """Return True if '/' is backed by a dm-crypt / crypt node."""
    tree = blockdevices if blockdevices is not None else probe_lsblk()

    def _has_root_mp(node: dict[str, Any]) -> bool:
        mps = node.get("mountpoints") or ([node.get("mountpoint")] if node.get("mountpoint") else [])
        if "/" in mps:
            return True
        return any(_has_root_mp(c) for c in (node.get("children") or []))

    def _walk(node: dict[str, Any], inside_crypt: bool) -> bool:
        now_crypt = inside_crypt or node.get("type") == "crypt" or node.get("fstype") == "crypto_LUKS"
        mps = node.get("mountpoints") or ([node.get("mountpoint")] if node.get("mountpoint") else [])
        if "/" in mps and now_crypt:
            return True
        for child in node.get("children") or []:
            if _walk(child, now_crypt):
                return True
        return False

    for dev in tree:
        if _has_root_mp(dev) and _walk(dev, False):
            return True
    return False


def _disk_system_reasons(
    tree: list[dict[str, Any]],
    volumes: list[dict[str, Any]],
    winmaint_uuids: set[str] | frozenset[str] = frozenset(),
) -> dict[str, list[str]]:
    """Return mapping of top-level disk path -> list of reasons it is held/system.

    "volume:NAME" (a registered volume) and WINMAINT_REASON (Maintenance Windows
    partitions) hold a disk without making it a system disk.
    """
    managed_part_uuids: dict[str, str] = {}
    managed_luks_uuids: dict[str, str] = {}
    managed_fs_uuids: dict[str, str] = {}
    for vol in volumes:
        if vol.get("fs_uuid"):
            managed_fs_uuids[vol["fs_uuid"].lower()] = vol["name"]
        for mem in vol.get("members", []):
            if mem.get("part_uuid"):
                managed_part_uuids[mem["part_uuid"].lower()] = vol["name"]
            if mem.get("luks_uuid"):
                managed_luks_uuids[mem["luks_uuid"].lower()] = vol["name"]

    reasons: dict[str, list[str]] = {}

    def _add(disk: str, msg: str) -> None:
        lst = reasons.setdefault(disk, [])
        if msg not in lst:
            lst.append(msg)

    def _inspect(node: dict[str, Any], top_disk: str) -> None:
        mps = node.get("mountpoints") or ([node.get("mountpoint")] if node.get("mountpoint") else [])
        for mp in mps:
            if not mp:
                continue
            if mp == "[SWAP]":
                _add(top_disk, "active swap")
            elif mp in ("/", "/efi", "/boot"):
                _add(top_disk, f"system mount {mp}")
            elif mp.startswith("/run/archiso") or mp.startswith("/run/initramfs"):
                _add(top_disk, f"live medium {mp}")
        pu = str(node.get("partuuid") or "").lower()
        uu = str(node.get("uuid") or "").lower()
        if pu and pu in managed_part_uuids:
            _add(top_disk, f"volume:{managed_part_uuids[pu]}")
        if uu and uu in managed_luks_uuids:
            _add(top_disk, f"volume:{managed_luks_uuids[uu]}")
        if uu and uu in managed_fs_uuids:
            _add(top_disk, f"volume:{managed_fs_uuids[uu]}")
        if is_winmaint_partition(node, winmaint_uuids):
            _add(top_disk, WINMAINT_REASON)
        for child in node.get("children") or []:
            _inspect(child, top_disk)

    for dev in tree:
        if dev.get("type") != "disk":
            continue
        dpath = str(dev.get("path") or dev.get("name") or "")
        _inspect(dev, dpath)
    return reasons


def _is_candidate_disk(dev: dict[str, Any]) -> bool:
    if dev.get("type") != "disk":
        return False
    path = str(dev.get("path") or "")
    base = os.path.basename(path)
    if base.startswith(("loop", "ram", "zram", "sr", "fd", "dm-", "md")):
        return False
    size = int(dev.get("size") or 0)
    if size <= 0:
        return False
    # Exclude removable USB/SD media (managed by drives.c); allow internal nvme/sata/sas/virtio/scsi.
    rm = dev.get("rm")
    if rm in (True, 1, "1"):
        return False
    tran = str(dev.get("tran") or "").lower()
    if tran in ("usb", "mmc"):
        return False
    return True


def list_disks(
    root: str | os.PathLike[str] | None = None,
    *,
    blockdevices: list[dict[str, Any]] | None = None,
    include_system: bool = False,
) -> list[dict[str, Any]]:
    """List internal block disks with partitions, system-use status, and managed state."""
    tree = blockdevices if blockdevices is not None else probe_lsblk()
    cfg = load_config(root=root)
    winmaint_uuids = winmaint_part_uuids(root)
    sys_reasons = _disk_system_reasons(tree, cfg["volumes"], winmaint_uuids)
    managed_parts: dict[str, str] = {}
    managed_uuids: dict[str, str] = {}
    for vol in cfg["volumes"]:
        if vol.get("fs_uuid"):
            managed_uuids[vol["fs_uuid"].lower()] = vol["name"]
        for mem in vol["members"]:
            if mem.get("part_uuid"):
                managed_parts[mem["part_uuid"].lower()] = vol["name"]
            if mem.get("luks_uuid"):
                managed_uuids[mem["luks_uuid"].lower()] = vol["name"]

    out: list[dict[str, Any]] = []
    for dev in tree:
        if not _is_candidate_disk(dev):
            continue
        dpath = str(dev.get("path") or "")
        reasons = list(sys_reasons.get(dpath, []))
        is_system = any(not r.startswith("volume:") and r != WINMAINT_REASON for r in reasons)
        managed_by = sorted({r.split(":", 1)[1] for r in reasons if r.startswith("volume:")})
        if is_system and not include_system:
            continue
        parts: list[dict[str, Any]] = []
        for child in dev.get("children") or []:
            if child.get("type") != "part":
                continue
            pu = str(child.get("partuuid") or "")
            uu = str(child.get("uuid") or "")
            mvol = managed_parts.get(pu.lower()) or managed_uuids.get(uu.lower()) or ""
            mps = [m for m in (child.get("mountpoints") or []) if m]
            winmaint = is_winmaint_partition(child, winmaint_uuids)
            parts.append(
                {
                    "path": str(child.get("path") or ""),
                    "size": int(child.get("size") or 0),
                    "fstype": str(child.get("fstype") or ""),
                    "label": str(child.get("label") or ""),
                    "part_uuid": pu,
                    "uuid": uu,
                    "mountpoints": mps,
                    "managed_volume": mvol,
                    "maintenance_windows": winmaint,
                    "attachable": (
                        not is_system
                        and not mvol
                        and not mps
                        and not winmaint
                        and str(child.get("fstype") or "") in (*VALID_FS, "crypto_LUKS")
                    ),
                }
            )
        in_use_reason = ", ".join(reasons)
        out.append(
            {
                "path": dpath,
                "model": str(dev.get("model") or "").strip(),
                "serial": str(dev.get("serial") or "").strip(),
                "size": int(dev.get("size") or 0),
                "log_sec": int(dev.get("log-sec") or 512),
                "free_tail_bytes": _free_tail_bytes(dev),
                "tran": str(dev.get("tran") or "").strip(),
                "ro": bool(dev.get("ro")),
                "system": is_system,
                "managed_volumes": managed_by,
                "maintenance_windows": WINMAINT_REASON in reasons,
                "in_use": bool(reasons),
                "in_use_reason": in_use_reason,
                "available_for_format": not reasons and not bool(dev.get("ro")),
                "partitions": parts,
            }
        )
    return out


def resolve_member_device(
    member: dict[str, str],
    *,
    encrypt: str,
    fs_uuid: str = "",
    flat_devices: list[dict[str, Any]],
) -> dict[str, Any] | None:
    """Resolve one volume member to its current block device node.

    Matches strictly by LUKS UUID / GPT PARTUUID / filesystem UUID (never by
    /dev/nvme* or /dev/sd* name). Fails closed (raises StorageError) if
    multiple distinct block devices match the same identifier (e.g., cloned disk).
    """
    luks_uuid = (member.get("luks_uuid") or "").lower()
    part_uuid = (member.get("part_uuid") or "").lower()
    vol_fs_uuid = (fs_uuid or "").lower()

    matches: list[dict[str, Any]] = []
    if encrypt == "luks":
        if luks_uuid:
            matches = [
                d
                for d in flat_devices
                if (d.get("uuid") or "").lower() == luks_uuid
                and d.get("type") in ("part", "disk")
            ]
        if not matches and part_uuid:
            matches = [
                d
                for d in flat_devices
                if (d.get("partuuid") or "").lower() == part_uuid
                and d.get("type") in ("part", "disk")
            ]
    else:
        if part_uuid:
            matches = [
                d
                for d in flat_devices
                if (d.get("partuuid") or "").lower() == part_uuid
                and d.get("type") in ("part", "disk")
            ]
        elif vol_fs_uuid:
            matches = [
                d
                for d in flat_devices
                if (d.get("uuid") or "").lower() == vol_fs_uuid
                and d.get("type") in ("part", "disk", "crypt")
            ]

    # Deduplicate by canonical path in case lsblk lists an alias.
    by_path: dict[str, dict[str, Any]] = {}
    for m in matches:
        p = str(m.get("path") or "")
        if p:
            by_path[p] = m
    unique = list(by_path.values())
    if len(unique) > 1:
        ident = luks_uuid or part_uuid or vol_fs_uuid
        paths = ", ".join(sorted(by_path.keys()))
        raise StorageError(f"Ambiguous member UUID {ident}: matched multiple devices ({paths})")
    if not unique:
        return None
    found = unique[0]
    if encrypt == "luks":
        # Verify LUKS header type when fstype is reported.
        fst = str(found.get("fstype") or "")
        if fst and fst != "crypto_LUKS":
            raise StorageError(
                f"member {found['path']} expected crypto_LUKS, found fstype={fst!r}"
            )
    else:
        # If both part_uuid and fs_uuid are recorded, cross-check fs_uuid when reported.
        if part_uuid and vol_fs_uuid:
            dev_uuid = str(found.get("uuid") or "").lower()
            if dev_uuid and dev_uuid != vol_fs_uuid:
                raise StorageError(
                    f"member {found['path']} UUID mismatch: expected {vol_fs_uuid}, found {dev_uuid}"
                )
    return found


def resolve_volume_members(
    vol: dict[str, Any],
    *,
    blockdevices: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """Resolve all members of a volume and return per-member resolution dicts."""
    tree = blockdevices if blockdevices is not None else probe_lsblk()
    flat = flatten_blockdevices(tree)
    results: list[dict[str, Any]] = []
    for idx, mem in enumerate(vol["members"]):
        dev = resolve_member_device(
            mem,
            encrypt=vol["encrypt"],
            fs_uuid=vol.get("fs_uuid", ""),
            flat_devices=flat,
        )
        mapper = mem.get("mapper", "")
        mapper_path = f"/dev/mapper/{mapper}" if mapper else ""
        mapper_open = bool(mapper_path and os.path.exists(mapper_path))
        results.append(
            {
                "index": idx,
                "part_uuid": mem.get("part_uuid", ""),
                "luks_uuid": mem.get("luks_uuid", ""),
                "mapper": mapper,
                "mapper_path": mapper_path,
                "mapper_open": mapper_open,
                "keyfile": mem.get("keyfile", ""),
                "keyfile_present": bool(mem.get("keyfile") and Path(mem["keyfile"]).is_file()),
                "present": dev is not None,
                "device_path": dev["path"] if dev else "",
                "parent_disk": dev.get("_top_disk", "") if dev else "",
                "size": int(dev.get("size") or 0) if dev else 0,
                "fstype": str(dev.get("fstype") or "") if dev else "",
            }
        )
    return results


# ---- Syscall mount / ~/Drives projection ----

def _libc() -> ctypes.CDLL:
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_ulong,
        ctypes.c_void_p,
    ]
    libc.mount.restype = ctypes.c_int
    libc.umount2.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.umount2.restype = ctypes.c_int
    return libc


def _syscall_mount(
    source: str | None,
    target: str,
    fstype: str | None,
    flags: int,
    data: str | None = None,
) -> None:
    libc = _libc()
    rc = libc.mount(
        source.encode("utf-8") if source is not None else None,
        target.encode("utf-8"),
        fstype.encode("utf-8") if fstype is not None else None,
        ctypes.c_ulong(flags),
        data.encode("utf-8") if data is not None else None,
    )
    if rc != 0:
        err = ctypes.get_errno()
        raise StorageError(f"mount({source!r} -> {target!r}, {fstype!r}): {os.strerror(err)}")


def _syscall_umount2(target: str, flags: int = UMOUNT_NOFOLLOW) -> None:
    libc = _libc()
    rc = libc.umount2(target.encode("utf-8"), ctypes.c_int(flags))
    if rc != 0:
        err = ctypes.get_errno()
        raise StorageError(f"umount2({target!r}): {os.strerror(err)}")


def ensure_drives_top(root: str | os.PathLike[str] | None = None) -> Path:
    """Ensure ~/Drives exists as a root-owned 0755 shared mountpoint.

    Idempotent with drives.c:setup_top(), so whichever runs first establishes
    the shared mountpoint and containers see projections via rslave propagation.
    """
    top = drives_dir_path(root=root)
    if not top.parent.is_dir():
        raise StorageError(f"user home directory does not exist: {top.parent}")
    try:
        os.mkdir(top, 0o755)
    except FileExistsError:
        pass
    fd = os.open(top, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        st_before = os.fstat(fd)
        if os.geteuid() == 0:
            os.fchown(fd, 0, 0)
        os.fchmod(fd, 0o755)
        at = f"/proc/self/fd/{fd}"
        if not is_mountpoint(top):
            _syscall_mount(at, at, None, MS_BIND | MS_REC, None)
    finally:
        os.close(fd)

    fd2 = os.open(top, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        st_after = os.fstat(fd2)
        if (st_after.st_dev, st_after.st_ino) != (st_before.st_dev, st_before.st_ino):
            raise StorageError(f"{top} changed unexpectedly during mount setup")
        at2 = f"/proc/self/fd/{fd2}"
        _syscall_mount(None, at2, None, MS_SHARED, None)
    finally:
        os.close(fd2)
    return top


def _mountinfo_vfs_flags(path: Path) -> int:
    """Read current per-mount VFS flags from mountinfo so MS_BIND|MS_REMOUNT preserves locked flags."""
    mi = mountinfo_for(path)
    if not mi:
        return 0
    opts = set(tok.strip() for tok in mi.get("mount_opts", "").split(",") if tok.strip())
    flags = 0
    if "ro" in opts:
        flags |= MS_RDONLY
    if "nosuid" in opts:
        flags |= MS_NOSUID
    if "nodev" in opts:
        flags |= MS_NODEV
    if "noexec" in opts:
        flags |= MS_NOEXEC
    if "noatime" in opts:
        flags |= MS_NOATIME
    if "nodiratime" in opts:
        flags |= MS_NODIRATIME
    if "relatime" in opts:
        flags |= MS_RELATIME
    if "strictatime" in opts:
        flags |= MS_STRICTATIME
    return flags


def project_to_drives(name: str, src_mount: Path) -> Path:
    """Bind-project /srv/storage/<name> into ~/Drives/<name> safely (O_NOFOLLOW)."""
    validate_volume_name(name)
    top = ensure_drives_top()
    topfd = os.open(top, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        try:
            os.mkdir(name, 0o755, dir_fd=topfd)
        except FileExistsError:
            pass
        leaf_path = top / name
        if not is_mountpoint(leaf_path):
            leaf_fd = os.open(name, os.O_PATH | os.O_NOFOLLOW | os.O_DIRECTORY | os.O_CLOEXEC, dir_fd=topfd)
            try:
                _syscall_mount(str(src_mount), f"/proc/self/fd/{leaf_fd}", None, MS_BIND, None)
            finally:
                os.close(leaf_fd)
            # Re-open onto the newly stacked bind mount so MS_REMOUNT targets it,
            # preserving existing mount flags (required in user namespaces too).
            remount_fd = os.open(name, os.O_PATH | os.O_NOFOLLOW | os.O_DIRECTORY | os.O_CLOEXEC, dir_fd=topfd)
            try:
                cur_flags = _mountinfo_vfs_flags(leaf_path)
                _syscall_mount(
                    None,
                    f"/proc/self/fd/{remount_fd}",
                    None,
                    cur_flags | MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV,
                    None,
                )
            except Exception:
                try:
                    _syscall_umount2(str(leaf_path), UMOUNT_NOFOLLOW)
                except StorageError:
                    pass
                raise
            finally:
                os.close(remount_fd)
        return leaf_path
    finally:
        os.close(topfd)


def unproject_from_drives(name: str) -> None:
    """Remove ~/Drives/<name> bind projection and empty directory if present."""
    validate_volume_name(name)
    try:
        top = drives_dir_path()
    except StorageError:
        return
    if not top.is_dir():
        return
    leaf = top / name
    if is_mountpoint(leaf):
        _syscall_umount2(str(leaf), UMOUNT_NOFOLLOW)
    try:
        topfd = os.open(top, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError:
        return
    try:
        try:
            os.rmdir(name, dir_fd=topfd)
        except FileNotFoundError:
            pass
        except OSError as exc:
            if exc.errno not in (errno.ENOTEMPTY, errno.EBUSY):
                raise StorageError(f"cannot remove projected dir {leaf}: {exc}") from exc
    finally:
        os.close(topfd)


# ---- Mount options & filesystem helpers ----

def build_mount_args(vol: dict[str, Any], *, degraded: bool = False) -> tuple[str, str]:
    """Return (kernel_fstype, mount_options_string) for a volume."""
    fs = vol["fs"]
    uid, gid = _primary_user_uid_gid()
    extra = vol.get("mount_options", "").strip()

    if fs == "btrfs":
        fstype = "btrfs"
        opts = ["noatime", "compress=zstd", "discard=async", "nosuid", "nodev"]
        if degraded:
            opts.append("degraded")
    elif fs == "ext4":
        fstype = "ext4"
        opts = ["noatime", "nosuid", "nodev"]
    elif fs == "xfs":
        fstype = "xfs"
        opts = ["noatime", "nosuid", "nodev"]
    elif fs == "ntfs":
        fstype = "ntfs3"
        opts = [f"uid={uid}", f"gid={gid}", "dmask=022", "fmask=133", "nosuid", "nodev"]
    elif fs == "exfat":
        fstype = "exfat"
        opts = [f"uid={uid}", f"gid={gid}", "dmask=022", "fmask=133", "nosuid", "nodev"]
    elif fs == "vfat":
        fstype = "vfat"
        opts = [
            f"uid={uid}",
            f"gid={gid}",
            "umask=022",
            "showexec",
            "utf8",
            "shortname=mixed",
            "flush",
            "nosuid",
            "nodev",
        ]
    else:
        raise StorageError(f"unsupported filesystem type: {fs!r}")

    if extra:
        for tok in extra.split(","):
            tok = tok.strip()
            if tok and tok not in opts:
                opts.append(tok)
    return fstype, ",".join(opts)


def btrfs_metadata_profile(raid: str, member_count: int) -> str:
    """Choose safe Btrfs metadata profile for multi-disk arrays.

    Always protects metadata with RAID1 (or RAID1C3 on >=3 disks with RAID1
    data) even when data profile is single or raid0.
    """
    if raid not in VALID_RAIDS:
        raise StorageError(f"invalid btrfs raid profile: {raid!r}")
    if member_count < 2:
        raise StorageError("btrfs-array requires at least 2 member disks")
    if raid == "raid10" and member_count < 4:
        raise StorageError("btrfs raid10 requires at least 4 member disks")
    if raid == "raid1" and member_count >= 3:
        return "raid1c3"
    return "raid1"


def _parse_dev_majmin(path: Path) -> tuple[int, int] | None:
    try:
        raw = path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None
    parts = raw.split(":", 1)
    if len(parts) != 2 or not parts[0].isdigit() or not parts[1].isdigit():
        return None
    return int(parts[0]), int(parts[1])


def _find_dm_for_mapper(mapper: str) -> tuple[str, int, int] | None:
    """Locate an active device-mapper device by dm/name in sysfs."""
    if not mapper:
        return None
    sdir = Path(SYS_BLOCK_DIR)
    if not sdir.is_dir():
        return None
    for entry in sorted(sdir.glob("dm-*")):
        try:
            dm_name = (entry / "dm" / "name").read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        if dm_name != mapper:
            continue
        mm = _parse_dev_majmin(entry / "dev")
        if mm is not None:
            return entry.name, mm[0], mm[1]
    return None


def _ensure_dm_ready() -> None:
    """Prepare device-mapper control node and lock dir before udev-client starts."""
    misc_dev = Path(SYS_DM_MISC_DEV)
    if not misc_dev.is_file() and shutil.which("modprobe"):
        for mod in ("dm_mod", "dm_crypt"):
            try:
                _run(["modprobe", mod], check=False, timeout=15)
            except StorageError:
                pass
    try:
        os.makedirs(DEV_MAPPER_DIR, mode=0o755, exist_ok=True)
    except OSError:
        pass
    try:
        os.makedirs(RUN_CRYPTSETUP_DIR, mode=0o700, exist_ok=True)
        os.chmod(RUN_CRYPTSETUP_DIR, 0o700)
    except OSError:
        pass
    ctrl = os.path.join(DEV_MAPPER_DIR, "control")
    if not os.path.exists(ctrl) and misc_dev.is_file():
        mm = _parse_dev_majmin(misc_dev)
        if mm is not None:
            try:
                os.mknod(ctrl, stat.S_IFCHR | 0o600, os.makedev(mm[0], mm[1]))
            except (FileExistsError, PermissionError, OSError):
                pass


def _ensure_mapper_node(mapper: str) -> str:
    """Ensure /dev/dm-N and /dev/mapper/<mapper> exist when udev has not run yet."""
    found = _find_dm_for_mapper(mapper)
    if found is None:
        return ""
    dm_kname, maj, min_no = found
    try:
        os.makedirs(DEV_MAPPER_DIR, mode=0o755, exist_ok=True)
    except OSError:
        pass
    dm_node = os.path.join(DEV_DIR, dm_kname)
    mapper_path = os.path.join(DEV_MAPPER_DIR, mapper)
    dev_id = os.makedev(maj, min_no)
    for node in (dm_node, mapper_path):
        if not os.path.exists(node):
            try:
                os.mknod(node, stat.S_IFBLK | 0o600, dev_id)
            except (FileExistsError, PermissionError, OSError):
                pass
    return mapper_path if os.path.exists(mapper_path) else (dm_node if os.path.exists(dm_node) else "")


def _unlock_luks_member(dev_path: str, mapper: str, keyfile: str, passphrase_file: str = "") -> str:
    mapper_path = os.path.join(DEV_MAPPER_DIR, mapper)
    if os.path.exists(mapper_path):
        return mapper_path
    existing = _ensure_mapper_node(mapper)
    if existing:
        return existing
    _ensure_dm_ready()
    cmd = ["cryptsetup", "open", "--type", "luks2", "--allow-discards", "--disable-external-tokens"]
    if keyfile and Path(keyfile).is_file():
        cmd.extend(["--key-file", keyfile])
    elif passphrase_file and Path(passphrase_file).is_file():
        cmd.extend(["--key-file", passphrase_file])
    else:
        raise StorageError(f"LUKS keyfile missing for {mapper}: {keyfile!r}")
    cmd.extend([dev_path, mapper])
    _run(cmd, check=True, timeout=120)
    if not os.path.exists(mapper_path):
        # Before udev-client runs, libdevmapper creates no node: make it from
        # sysfs, as init's ensure_mapper_nodes does for the root.
        created = _ensure_mapper_node(mapper)
        if created:
            return created
        raise StorageError(f"cryptsetup opened {mapper}, but {mapper_path} did not appear")
    return mapper_path


def _close_luks_member(mapper: str) -> None:
    if not mapper:
        return
    mapper_path = os.path.join(DEV_MAPPER_DIR, mapper)
    if not os.path.exists(mapper_path) and _find_dm_for_mapper(mapper) is None:
        return
    _run(["cryptsetup", "close", mapper], check=True, timeout=60)


def mount_volume(
    name: str,
    *,
    degraded: bool = False,
    passphrase_file: str = "",
) -> dict[str, Any]:
    """Unlock (if LUKS), mount /srv/storage/<name>, and project to ~/Drives if role permits."""
    vol = get_volume(name)
    if vol is None:
        raise StorageError(f"volume not found: {name}")

    mp = volume_mountpoint(name)
    if is_mountpoint(mp):
        projected = None
        if vol["role"] in ("data", "shared"):
            projected = str(project_to_drives(name, mp))
        return {"name": name, "mountpoint": str(mp), "drives_path": projected or "", "already_mounted": True}

    resolved = resolve_volume_members(vol)
    present = [m for m in resolved if m["present"]]
    missing = [m for m in resolved if not m["present"]]

    if missing and not (vol["topology"] == "btrfs-array" and degraded and present):
        missing_ids = ", ".join(m["luks_uuid"] or m["part_uuid"] or f"#{m['index']}" for m in missing)
        hint = " (pass --degraded explicitly to mount a degraded btrfs array)" if vol["topology"] == "btrfs-array" and present else ""
        raise StorageError(f"volume {name!r}: missing {len(missing)}/{len(resolved)} member(s): {missing_ids}{hint}")

    dev_paths: list[str] = []
    newly_opened: list[str] = []
    try:
        for m in present:
            if vol["encrypt"] == "luks":
                was_open = m["mapper_open"]
                mpath = _unlock_luks_member(m["device_path"], m["mapper"], m["keyfile"], passphrase_file=passphrase_file)
                if not was_open:
                    newly_opened.append(m["mapper"])
                dev_paths.append(mpath)
            else:
                dev_paths.append(m["device_path"])

        if vol["topology"] == "btrfs-array":
            _run(["btrfs", "device", "scan", *dev_paths], check=True, timeout=60)

        mp.mkdir(parents=True, exist_ok=True)
        os.chmod(mp, 0o755)
        fstype, opts = build_mount_args(vol, degraded=degraded)
        _run(["mount", "-t", fstype, "-o", opts, dev_paths[0], str(mp)], check=True, timeout=60)

        projected = ""
        if vol["role"] in ("data", "shared"):
            projected = str(project_to_drives(name, mp))
        return {"name": name, "mountpoint": str(mp), "drives_path": projected, "already_mounted": False}
    except Exception:
        if is_mountpoint(mp):
            try:
                _run(["umount", str(mp)], check=False, timeout=30)
            except StorageError:
                pass
        for mapper in newly_opened:
            try:
                _close_luks_member(mapper)
            except StorageError:
                pass
        raise


def umount_volume(name: str, *, close_luks: bool = True) -> dict[str, Any]:
    """Unproject ~/Drives/<name>, unmount /srv/storage/<name>, and close LUKS mappers."""
    vol = get_volume(name)
    if vol is None:
        raise StorageError(f"volume not found: {name}")

    unproject_from_drives(name)
    mp = volume_mountpoint(name)
    if is_mountpoint(mp):
        _run(["umount", str(mp)], check=True, timeout=60)

    if close_luks and vol["encrypt"] == "luks":
        for mem in vol["members"]:
            _close_luks_member(mem.get("mapper", ""))

    return {"name": name, "unmounted": True}


def mount_all() -> dict[str, Any]:
    """Mount all volumes with auto_mount=True; non-fatal per volume.

    Retries briefly (bounded, default 2 x 0.5 s) only when a volume reports a
    missing member device, covering slow async NVMe/SATA namespace discovery
    right after init loads kernel modules.
    """
    cfg = load_config()
    mounted: list[str] = []
    skipped: list[str] = []
    pending: list[str] = []
    for vol in cfg["volumes"]:
        if not vol.get("auto_mount", True):
            skipped.append(vol["name"])
        else:
            pending.append(vol["name"])

    try:
        retry_sleep = float(os.environ.get("LOCACHY_STORAGE_MOUNT_RETRY_S", "0.5"))
    except ValueError:
        retry_sleep = 0.5
    max_attempts = 3 if retry_sleep > 0 else 1

    errors: dict[str, str] = {}
    for attempt in range(max_attempts):
        if not pending:
            break
        if attempt > 0:
            time.sleep(retry_sleep)
        next_pending: list[str] = []
        for vname in pending:
            try:
                mount_volume(vname)
                errors.pop(vname, None)
                mounted.append(vname)
            except Exception as exc:
                msg = str(exc)
                errors[vname] = msg
                if "missing " in msg and "member(s)" in msg:
                    next_pending.append(vname)
        pending = next_pending
    return {"mounted": mounted, "skipped": skipped, "errors": errors}


def umount_all() -> dict[str, Any]:
    """Unmount all managed volumes and close their LUKS mappers; best-effort per volume."""
    cfg = load_config()
    unmounted: list[str] = []
    errors: dict[str, str] = {}
    for vol in reversed(cfg["volumes"]):
        try:
            umount_volume(vol["name"], close_luks=True)
            unmounted.append(vol["name"])
        except Exception as exc:
            errors[vol["name"]] = str(exc)
    return {"unmounted": unmounted, "errors": errors}


# ---- Partitioning, formatting, attachment, creation ----

def _partition_path(disk: str, part_num: int = 1) -> str:
    sep = "p" if re.search(r"[0-9]$", disk) else ""
    return f"{disk}{sep}{part_num}"


def _blkid_value(dev: str, tag: str) -> str:
    proc = _run(["blkid", "-o", "value", "-s", tag, dev], check=False, timeout=15)
    return proc.stdout.decode("utf-8", errors="replace").strip() if proc.returncode == 0 else ""


def _wait_for_device(path: str, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.1)
    if not os.path.exists(path):
        raise StorageError(f"partition device did not appear: {path}")


def _settle_partitions(disk: str) -> None:
    if shutil.which("partprobe"):
        _run(["partprobe", disk], check=False, timeout=30)
    if shutil.which("udevadm"):
        _run(["udevadm", "settle", "--timeout=10"], check=False, timeout=15)


def _ensure_safe_target_disks(
    devs: list[str],
    *,
    mode: str,
    cfg: dict[str, Any],
    blockdevices: list[dict[str, Any]],
    winmaint_uuids: set[str] | frozenset[str] = frozenset(),
) -> tuple[list[dict[str, Any]], set[str]]:
    """Validate that candidate devices are real, non-system, unmanaged, and distinct.

    A disk holding Maintenance Windows partitions is never formatted, and those
    partitions are never attached; its other partitions can be attached.
    """
    flat = flatten_blockdevices(blockdevices)
    by_path = {d["path"]: d for d in flat if d.get("path")}
    sys_reasons = _disk_system_reasons(blockdevices, cfg["volumes"], winmaint_uuids)

    resolved_nodes: list[dict[str, Any]] = []
    parent_disks: set[str] = set()
    seen_devs: set[str] = set()

    for raw in devs:
        dpath = os.path.realpath(raw) if os.path.exists(raw) else raw
        node = by_path.get(dpath) or by_path.get(raw)
        if node is None:
            raise StorageError(f"block device not found: {raw}")
        canonical = node["path"]
        if canonical in seen_devs:
            raise StorageError(f"duplicate device specified: {canonical}")
        seen_devs.add(canonical)

        top_disk = node.get("_top_disk") or canonical
        reasons = sys_reasons.get(top_disk, [])
        if mode in ("format", "array"):
            if node.get("type") != "disk":
                raise StorageError(f"mode={mode!r} requires a whole disk device, got {canonical} (type={node.get('type')})")
            if WINMAINT_REASON in reasons:
                raise StorageError(
                    f"refusing to format {canonical}: it holds Maintenance Windows partitions (locachy-winmaint); "
                    "formatting would erase Windows"
                )
            if reasons:
                raise StorageError(f"refusing to format protected/in-use disk {canonical}: {', '.join(reasons)}")
            if bool(node.get("ro")):
                raise StorageError(f"refusing to format read-only disk {canonical}")
        elif mode == "attach":
            if node.get("type") not in ("part", "disk"):
                raise StorageError(f"mode='attach' requires a partition or disk, got {canonical} (type={node.get('type')})")
            if is_winmaint_partition(node, winmaint_uuids) or (
                node.get("type") == "disk" and WINMAINT_REASON in reasons
            ):
                raise StorageError(
                    f"refusing to attach {canonical}: it is or holds a Maintenance Windows partition (locachy-winmaint)"
                )
            # Allow attaching an unrelated partition on a non-system disk only; refuse system disks & already-managed members.
            non_vol_reasons = [r for r in reasons if not r.startswith("volume:") and r != WINMAINT_REASON]
            if non_vol_reasons:
                raise StorageError(f"refusing to attach partition on system disk {top_disk}: {', '.join(non_vol_reasons)}")
            # Check if this specific partition is already registered in any volume.
            pu = str(node.get("partuuid") or "").lower()
            uu = str(node.get("uuid") or "").lower()
            for v in cfg["volumes"]:
                if uu and v.get("fs_uuid", "").lower() == uu:
                    raise StorageError(f"device {canonical} is already registered in volume {v['name']!r}")
                for mem in v["members"]:
                    if pu and mem.get("part_uuid", "").lower() == pu:
                        raise StorageError(f"device {canonical} is already registered in volume {v['name']!r}")
                    if uu and mem.get("luks_uuid", "").lower() == uu:
                        raise StorageError(f"device {canonical} is already registered in volume {v['name']!r}")

        if mode == "array" and top_disk in parent_disks:
            raise StorageError(
                f"refusing to put two members from the same physical disk ({top_disk}) into a btrfs-array"
            )
        parent_disks.add(top_disk)
        resolved_nodes.append(node)

    return resolved_nodes, parent_disks


def _free_tail_bytes(dev: dict[str, Any]) -> int | None:
    """Unallocated bytes after the last partition, before the backup GPT.

    lsblk START is in 512-byte units (sysfs convention). Returns None when a
    partition's start is unknown, so the figure is never overstated.
    """
    log = int(dev.get("log-sec") or 512)
    size = int(dev.get("size") or 0)
    entries = GPT_ENTRY_BYTES // log
    total = size // log
    end_excl = total - (entries + 1)
    max_end = 2 + entries
    for child in dev.get("children") or []:
        if child.get("type") != "part":
            continue
        if child.get("start") is None:
            return None
        start_lba = int(child["start"]) * 512 // log
        max_end = max(max_end, start_lba + int(child.get("size") or 0) // log)
    return max(0, end_excl - max_end) * log


def format_plan(size_bytes: int, log_sec: int | None, reserve_mib: int = 0) -> dict[str, Any]:
    """Partition layout for mode=format: the whole disk, or all but a reserved tail.

    reserve_mib == 0 keeps the full-disk partition (sgdisk -n 1:0:0). Otherwise
    partition 1 ends before the last reserve_mib MiB of usable space, aligned to
    1 MiB, and must leave at least 1 GiB for the volume. Validation happens here,
    before any device is written.
    """
    if not isinstance(reserve_mib, int) or isinstance(reserve_mib, bool):
        raise StorageError("--reserve-tail-mib must be an integer")
    if reserve_mib < 0:
        raise StorageError("--reserve-tail-mib must be 0 or more")
    log = int(log_sec or 512)
    if log not in (512, 4096):
        raise StorageError(f"unsupported logical sector size: {log}")
    total = int(size_bytes) // log
    align = PART_ALIGN_BYTES // log
    entries = GPT_ENTRY_BYTES // log
    first = align
    last_usable = total - (entries + 1) - 1
    if reserve_mib == 0:
        end = last_usable
        full_disk = True
    else:
        reserved = reserve_mib * PART_ALIGN_BYTES // log
        end_excl = ((last_usable + 1 - reserved) // align) * align
        end = end_excl - 1
        full_disk = False
    size_sectors = end - first + 1
    if size_sectors * log < MIN_VOLUME_BYTES:
        raise StorageError(
            f"reserving {reserve_mib} MiB leaves less than 1 GiB for the volume on a "
            f"{size_bytes}-byte disk"
        )
    return {
        "full_disk": full_disk,
        "start_lba": first,
        "end_lba": end,
        "size_sectors": size_sectors,
        "size_bytes": size_sectors * log,
        "reserved_tail_bytes": (total - (end + 1) - (entries + 1)) * log,
        "log_sec": log,
    }


def _partition_single_gpt(disk: str, part_name: str, plan: dict[str, Any] | None = None) -> tuple[str, str]:
    """Create a fresh GPT table with one Linux data partition (the whole disk unless a plan says otherwise).

    Returns (part_path, part_uuid).
    """
    if shutil.which("wipefs"):
        _run(["wipefs", "--all", "--force", disk], check=True, timeout=60)
    full = plan is None or plan["full_disk"]
    if shutil.which("sgdisk"):
        _run(["sgdisk", "--zap-all", disk], check=True, timeout=60)
        span = ["1:0:0"] if full else [f"1:{plan['start_lba']}:{plan['end_lba']}"]
        _run(
            ["sgdisk", "-n", span[0], "-t", f"1:{GPT_TYPE_LINUX_DATA}", "-c", f"1:{part_name}", disk],
            check=True,
            timeout=60,
        )
    elif shutil.which("sfdisk"):
        if full:
            script = f"label: gpt\ntype={GPT_TYPE_LINUX_DATA}, name=\"{part_name}\"\n"
        else:
            script = (
                f"label: gpt\nstart={plan['start_lba']}, size={plan['size_sectors']}, "
                f"type={GPT_TYPE_LINUX_DATA}, name=\"{part_name}\"\n"
            )
        _run(["sfdisk", "--wipe", "always", disk], input_bytes=script.encode("utf-8"), check=True, timeout=60)
    else:
        raise StorageError("neither sgdisk nor sfdisk is installed for GPT partitioning")

    _settle_partitions(disk)
    part_path = _partition_path(disk, 1)
    _wait_for_device(part_path)
    part_uuid = _blkid_value(part_path, "PARTUUID")
    if not part_uuid:
        raise StorageError(f"failed to read PARTUUID for new partition {part_path}")
    return part_path, part_uuid


def _format_luks_member(
    part_path: str,
    mapper: str,
    keyfile_host: Path,
    *,
    label: str,
    passphrase_file: str = "",
) -> str:
    """Format a partition as LUKS2 with a 64-byte random keyfile (and optional recovery passphrase)."""
    key_bytes = secrets.token_bytes(64)
    _write_keyfile(keyfile_host, key_bytes)

    _run(
        [
            "cryptsetup",
            "luksFormat",
            "--type",
            "luks2",
            "--batch-mode",
            "--label",
            label[:47],
            "--key-file",
            str(keyfile_host),
            part_path,
        ],
        check=True,
        timeout=180,
    )
    if passphrase_file:
        pfile = Path(passphrase_file)
        if not pfile.is_file():
            raise StorageError(f"passphrase file not found: {passphrase_file}")
        _run(
            [
                "cryptsetup",
                "luksAddKey",
                "--batch-mode",
                "--key-file",
                str(keyfile_host),
                part_path,
                str(pfile),
            ],
            check=True,
            timeout=180,
        )
    luks_uuid = _run(["cryptsetup", "luksUUID", part_path], check=True, timeout=30).stdout.decode("utf-8").strip()
    if not luks_uuid:
        raise StorageError(f"failed to read LUKS UUID from {part_path}")
    _unlock_luks_member(part_path, mapper, str(keyfile_host))
    return luks_uuid


def _chown_fresh_filesystem(dev_path: str, fstype: str) -> None:
    """Chown root directory of freshly formatted ext4/btrfs volume to uid:gid 1000:1000."""
    uid, gid = _primary_user_uid_gid()
    with tempfile.TemporaryDirectory(prefix="locachy-vol-init-") as tmp:
        _run(["mount", "-t", fstype, dev_path, tmp], check=True, timeout=60)
        try:
            os.chown(tmp, uid, gid)
            os.chmod(tmp, 0o755)
        finally:
            _run(["umount", tmp], check=True, timeout=60)


def create_volume(
    *,
    name: str,
    mode: str,
    role: str,
    devs: list[str],
    fs: str = "ext4",
    raid: str = "raid1",
    encrypt: str = "auto",
    passphrase_file: str = "",
    keyfile: str = "",
    root: str | os.PathLike[str] | None = None,
    no_mount: bool = False,
    auto_mount: bool = True,
    mount_options: str = "",
    blockdevices: list[dict[str, Any]] | None = None,
    reserve_tail_mib: int = 0,
) -> dict[str, Any]:
    """Create (format/array) or attach an existing partition as a LOCachy storage volume."""
    validate_volume_name(name)
    validate_role(role)
    if mode not in VALID_MODES:
        raise StorageError(f"invalid mode {mode!r}; expected one of {', '.join(VALID_MODES)}")
    if not devs:
        raise StorageError("at least one device must be provided")
    if reserve_tail_mib and mode != "format":
        raise StorageError("--reserve-tail-mib applies only to --mode format")
    if reserve_tail_mib < 0:
        raise StorageError("--reserve-tail-mib must be 0 or more")

    vfile = volumes_file_path(root=root)
    lock_fd = _lock_volumes(vfile)
    try:
        cfg = load_config(root=root)
        if any(v["name"] == name for v in cfg["volumes"]):
            raise StorageError(f"volume already exists: {name}")

        tree = blockdevices if blockdevices is not None else probe_lsblk()
        nodes, _ = _ensure_safe_target_disks(devs, mode=mode, cfg=cfg, blockdevices=tree,
                                             winmaint_uuids=winmaint_part_uuids(root))

        enc_resolved = encrypt
        if enc_resolved == "auto":
            enc_resolved = "luks" if root_is_encrypted(tree) else "none"
        if enc_resolved not in VALID_ENCRYPT:
            raise StorageError(f"invalid encrypt mode {encrypt!r}; expected none, luks, or auto")

        is_target_install = root is not None and str(Path(root)) != "/"
        kdir_host = keys_dir_path(root=root)
        opened_mappers: list[str] = []

        try:
            if mode == "attach":
                if len(nodes) != 1:
                    raise StorageError("mode='attach' accepts exactly one partition/device")
                node = nodes[0]
                dev_path = node["path"]
                detected_fs = str(node.get("fstype") or "")
                part_uuid = str(node.get("partuuid") or "")
                dev_uuid = str(node.get("uuid") or "")

                if detected_fs == "crypto_LUKS":
                    mapper = f"locachy-vol-{name}-0"
                    key_record_path = f"{DEFAULT_KEYS_DIR}/vol-{name}-0.key"
                    key_host_path = kdir_host / f"vol-{name}-0.key"
                    if keyfile:
                        kf = Path(keyfile)
                        if not kf.is_file():
                            raise StorageError(f"keyfile not found: {keyfile}")
                        _write_keyfile(key_host_path, kf.read_bytes())
                        unlock_key = str(key_host_path)
                    elif passphrase_file:
                        pf = Path(passphrase_file)
                        if not pf.is_file():
                            raise StorageError(f"passphrase file not found: {passphrase_file}")
                        # Enroll a fresh 64-byte auto-unlock keyfile using the supplied passphrase.
                        _write_keyfile(key_host_path, secrets.token_bytes(64))
                        _run(
                            [
                                "cryptsetup",
                                "luksAddKey",
                                "--batch-mode",
                                "--key-file",
                                str(pf),
                                dev_path,
                                str(key_host_path),
                            ],
                            check=True,
                            timeout=180,
                        )
                        unlock_key = str(key_host_path)
                    else:
                        raise StorageError(
                            f"attaching LUKS2 partition {dev_path} requires --keyfile or --passphrase-file"
                        )
                    mpath = _unlock_luks_member(dev_path, mapper, unlock_key)
                    opened_mappers.append(mapper)
                    inner_fs = _blkid_value(mpath, "TYPE")
                    inner_uuid = _blkid_value(mpath, "UUID")
                    if inner_fs not in VALID_FS:
                        raise StorageError(
                            f"unsupported filesystem {inner_fs!r} inside LUKS2 partition {dev_path}"
                        )
                    vol = validate_volume_entry(
                        {
                            "name": name,
                            "topology": "single",
                            "fs": inner_fs,
                            "raid": "",
                            "role": role,
                            "encrypt": "luks",
                            "auto_mount": auto_mount,
                            "fs_uuid": inner_uuid,
                            "members": [
                                {
                                    "part_uuid": part_uuid,
                                    "luks_uuid": dev_uuid,
                                    "mapper": mapper,
                                    "keyfile": key_record_path if is_target_install else str(key_host_path),
                                }
                            ],
                            "mount_options": mount_options,
                            "created_at": _utc_now(),
                        }
                    )
                else:
                    if detected_fs not in VALID_FS:
                        raise StorageError(
                            f"cannot attach {dev_path}: unsupported or missing filesystem ({detected_fs!r})"
                        )
                    vol = validate_volume_entry(
                        {
                            "name": name,
                            "topology": "single",
                            "fs": detected_fs,
                            "raid": "",
                            "role": role,
                            "encrypt": "none",
                            "auto_mount": auto_mount,
                            "fs_uuid": dev_uuid,
                            "members": [
                                {
                                    "part_uuid": part_uuid,
                                    "luks_uuid": "",
                                    "mapper": "",
                                    "keyfile": "",
                                }
                            ],
                            "mount_options": mount_options,
                            "created_at": _utc_now(),
                        }
                    )

            elif mode == "format":
                if len(nodes) != 1:
                    raise StorageError("mode='format' accepts exactly one disk")
                if fs not in VALID_FORMAT_FS:
                    raise StorageError(f"mode='format' supports fs in {', '.join(VALID_FORMAT_FS)}, got {fs!r}")
                disk = nodes[0]["path"]
                plan = format_plan(int(nodes[0].get("size") or 0), nodes[0].get("log-sec"), reserve_tail_mib)
                part_path, part_uuid = _partition_single_gpt(disk, f"locachy-{name}", plan)
                mapper = ""
                luks_uuid = ""
                key_record = ""
                target_dev = part_path
                if enc_resolved == "luks":
                    mapper = f"locachy-vol-{name}-0"
                    key_host = kdir_host / f"vol-{name}-0.key"
                    key_record = f"{DEFAULT_KEYS_DIR}/vol-{name}-0.key" if is_target_install else str(key_host)
                    luks_uuid = _format_luks_member(
                        part_path,
                        mapper,
                        key_host,
                        label=f"locachy-vol-{name}",
                        passphrase_file=passphrase_file,
                    )
                    opened_mappers.append(mapper)
                    target_dev = f"/dev/mapper/{mapper}"

                if fs == "ext4":
                    _run(["mkfs.ext4", "-F", "-L", name[:16], target_dev], check=True, timeout=300)
                else:
                    _run(["mkfs.btrfs", "-f", "-L", name[:255], target_dev], check=True, timeout=300)
                fs_uuid = _blkid_value(target_dev, "UUID")
                if not fs_uuid:
                    raise StorageError(f"failed to read filesystem UUID on {target_dev}")
                _chown_fresh_filesystem(target_dev, fs)

                vol = validate_volume_entry(
                    {
                        "name": name,
                        "topology": "single",
                        "fs": fs,
                        "raid": "",
                        "role": role,
                        "encrypt": enc_resolved,
                        "auto_mount": auto_mount,
                        "fs_uuid": fs_uuid,
                        "members": [
                            {
                                "part_uuid": part_uuid,
                                "luks_uuid": luks_uuid,
                                "mapper": mapper,
                                "keyfile": key_record,
                            }
                        ],
                        "mount_options": mount_options,
                        "created_at": _utc_now(),
                    }
                )

            else:  # mode == "array"
                meta_profile = btrfs_metadata_profile(raid, len(nodes))
                members: list[dict[str, str]] = []
                btrfs_devs: list[str] = []
                for idx, node in enumerate(nodes):
                    disk = node["path"]
                    part_path, part_uuid = _partition_single_gpt(disk, f"locachy-{name}-{idx}")
                    mapper = ""
                    luks_uuid = ""
                    key_record = ""
                    dev_for_fs = part_path
                    if enc_resolved == "luks":
                        mapper = f"locachy-vol-{name}-{idx}"
                        key_host = kdir_host / f"vol-{name}-{idx}.key"
                        key_record = (
                            f"{DEFAULT_KEYS_DIR}/vol-{name}-{idx}.key"
                            if is_target_install
                            else str(key_host)
                        )
                        luks_uuid = _format_luks_member(
                            part_path,
                            mapper,
                            key_host,
                            label=f"locachy-vol-{name}-{idx}",
                            passphrase_file=passphrase_file,
                        )
                        opened_mappers.append(mapper)
                        dev_for_fs = f"/dev/mapper/{mapper}"
                    members.append(
                        {
                            "part_uuid": part_uuid,
                            "luks_uuid": luks_uuid,
                            "mapper": mapper,
                            "keyfile": key_record,
                        }
                    )
                    btrfs_devs.append(dev_for_fs)

                _run(
                    [
                        "mkfs.btrfs",
                        "-f",
                        "-L",
                        name[:255],
                        "-d",
                        raid,
                        "-m",
                        meta_profile,
                        *btrfs_devs,
                    ],
                    check=True,
                    timeout=300,
                )
                fs_uuid = _blkid_value(btrfs_devs[0], "UUID")
                if not fs_uuid:
                    raise StorageError(f"failed to read btrfs filesystem UUID on {btrfs_devs[0]}")
                _run(["btrfs", "device", "scan", *btrfs_devs], check=False, timeout=60)
                _chown_fresh_filesystem(btrfs_devs[0], "btrfs")

                vol = validate_volume_entry(
                    {
                        "name": name,
                        "topology": "btrfs-array",
                        "fs": "btrfs",
                        "raid": raid,
                        "role": role,
                        "encrypt": enc_resolved,
                        "auto_mount": auto_mount,
                        "fs_uuid": fs_uuid,
                        "members": members,
                        "mount_options": mount_options,
                        "created_at": _utc_now(),
                    }
                )

            cfg["volumes"].append(vol)
            save_config(cfg, root=root)
        except Exception:
            for mapper in opened_mappers:
                try:
                    _close_luks_member(mapper)
                except StorageError:
                    pass
            raise

        # Close opened mappers when building into an offline install target or --no-mount.
        if no_mount or is_target_install:
            for mapper in opened_mappers:
                try:
                    _close_luks_member(mapper)
                except StorageError:
                    pass
    finally:
        _unlock_fd(lock_fd)

    if not no_mount and not is_target_install:
        mount_volume(name)
    return vol


# ---- Mutating operations ----

def set_role(name: str, new_role: str, *, root: str | os.PathLike[str] | None = None) -> dict[str, Any]:
    validate_volume_name(name)
    validate_role(new_role)
    vfile = volumes_file_path(root=root)
    lock_fd = _lock_volumes(vfile)
    try:
        cfg = load_config(root=root)
        target = None
        for vol in cfg["volumes"]:
            if vol["name"] == name:
                target = vol
                break
        if target is None:
            raise StorageError(f"volume not found: {name}")
        # Prevent downgrading the active compute_storage volume to role='data'.
        active_compute = read_rc_value("compute_storage", root=root)
        if active_compute == name and new_role == "data":
            raise StorageError(
                f"volume {name!r} is configured as compute_storage; clear compute_storage before setting role='data'"
            )
        target["role"] = new_role
        save_config(cfg, root=root)
    finally:
        _unlock_fd(lock_fd)

    if root is None or str(Path(root)) == "/":
        mp = volume_mountpoint(name)
        if is_mountpoint(mp):
            if new_role in ("data", "shared"):
                project_to_drives(name, mp)
            else:
                unproject_from_drives(name)
    return target


def set_auto_mount(name: str, enabled: bool, *, root: str | os.PathLike[str] | None = None) -> dict[str, Any]:
    validate_volume_name(name)
    vfile = volumes_file_path(root=root)
    lock_fd = _lock_volumes(vfile)
    try:
        cfg = load_config(root=root)
        target = None
        for vol in cfg["volumes"]:
            if vol["name"] == name:
                target = vol
                break
        if target is None:
            raise StorageError(f"volume not found: {name}")
        target["auto_mount"] = bool(enabled)
        save_config(cfg, root=root)
        return target
    finally:
        _unlock_fd(lock_fd)


def set_compute_storage(name: str, *, root: str | os.PathLike[str] | None = None) -> dict[str, str]:
    """Configure compute_storage in rc.conf, enforcing role in ('compute', 'shared')."""
    if name in ("", "none", "off"):
        write_rc_value("compute_storage", None, root=root)
        return {"compute_storage": ""}
    vol = get_volume(name, root=root)
    if vol is None:
        raise StorageError(f"volume not found: {name}")
    if vol["role"] not in ("compute", "shared"):
        raise StorageError(
            f"volume {name!r} has role={vol['role']!r}; compute_storage requires role 'compute' or 'shared'"
        )
    write_rc_value("compute_storage", name, root=root)
    return {"compute_storage": name}


def remove_volume(
    name: str,
    *,
    wipe: bool = False,
    delete_key: bool = False,
    root: str | os.PathLike[str] | None = None,
    blockdevices: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Detach (or wipe) a registered volume.

    By default (detach), LUKS keyfiles are PRESERVED on disk so the user cannot
    accidentally lose access to encrypted data; pass wipe=True or delete_key=True
    to delete keyfiles (and wipefs member partitions when wipe=True).
    """
    validate_volume_name(name)
    is_live = root is None or str(Path(root)) == "/"
    vfile = volumes_file_path(root=root)
    lock_fd = _lock_volumes(vfile)
    try:
        cfg = load_config(root=root)
        idx = next((i for i, v in enumerate(cfg["volumes"]) if v["name"] == name), -1)
        if idx < 0:
            raise StorageError(f"volume not found: {name}")
        vol = cfg["volumes"][idx]

        resolved_before: list[dict[str, Any]] = []
        if wipe and is_live:
            tree = blockdevices if blockdevices is not None else probe_lsblk()
            resolved_before = resolve_volume_members(vol, blockdevices=tree)
            nodes = {d["path"]: d for d in flatten_blockdevices(tree) if d.get("path")}
            recorded = winmaint_part_uuids(root)
            for m in resolved_before:
                node = nodes.get(m["device_path"]) if m["present"] else None
                if node and is_winmaint_partition(node, recorded):
                    raise StorageError(f"refusing to wipe {m['device_path']}: it is a Maintenance Windows partition")

        active_compute = read_rc_value("compute_storage", root=root)
        if active_compute == name:
            write_rc_value("compute_storage", None, root=root)

        if is_live:
            umount_volume(name, close_luks=True)

        wiped_devs: list[str] = []
        if wipe and is_live:
            for m in resolved_before:
                if m["present"] and m["device_path"]:
                    _run(["wipefs", "--all", "--force", m["device_path"]], check=True, timeout=60)
                    wiped_devs.append(m["device_path"])

        keys_deleted: list[str] = []
        keys_kept: list[str] = []
        for mem in vol["members"]:
            kf = mem.get("keyfile", "")
            if not kf:
                continue
            kf_path = _target_subpath(root, kf) if not is_live else Path(kf)
            if wipe or delete_key:
                if kf_path.is_file():
                    kf_path.unlink()
                    keys_deleted.append(str(kf_path))
            else:
                if kf_path.is_file():
                    keys_kept.append(str(kf_path))

        del cfg["volumes"][idx]
        save_config(cfg, root=root)
        return {
            "name": name,
            "removed": True,
            "wiped_devices": wiped_devs,
            "keys_deleted": keys_deleted,
            "keys_kept": keys_kept,
        }
    finally:
        _unlock_fd(lock_fd)


# ---- Status & health reporting ----

def _statvfs_usage(path: Path) -> dict[str, int]:
    try:
        st = os.statvfs(path)
        total = st.f_frsize * st.f_blocks
        avail = st.f_frsize * st.f_bavail
        free = st.f_frsize * st.f_bfree
        used = max(0, total - free)
        return {"total_bytes": total, "used_bytes": used, "avail_bytes": avail}
    except OSError:
        return {"total_bytes": 0, "used_bytes": 0, "avail_bytes": 0}


def _btrfs_device_stats(mp: Path) -> dict[str, Any] | None:
    if not shutil.which("btrfs"):
        return None
    proc = _run(["btrfs", "device", "stats", str(mp)], check=False, timeout=15)
    if proc.returncode != 0:
        return None
    stats: dict[str, dict[str, int]] = {}
    total_errors = 0
    for line in proc.stdout.decode("utf-8", errors="replace").splitlines():
        m = re.match(r"^\[([^\]]+)\]\.([A-Za-z0-9_]+)\s+([0-9]+)\s*$", line.strip())
        if not m:
            continue
        dev, counter, raw_val = m.group(1), m.group(2), int(m.group(3))
        stats.setdefault(dev, {})[counter] = raw_val
        total_errors += raw_val
    return {"devices": stats, "total_errors": total_errors}


def status_report(
    root: str | os.PathLike[str] | None = None,
    *,
    blockdevices: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Build full structured JSON status report of volumes, disks, and compute_storage."""
    tree = blockdevices if blockdevices is not None else probe_lsblk()
    cfg = load_config(root=root)
    enc_root = root_is_encrypted(tree)
    active_compute = read_rc_value("compute_storage", root=root)
    is_live = root is None or str(Path(root)) == "/"

    vol_reports: list[dict[str, Any]] = []
    for vol in cfg["volumes"]:
        name = vol["name"]
        mp = volume_mountpoint(name, root=root)
        mounted = is_live and is_mountpoint(mp)
        drives_p = ""
        drives_projected = False
        if is_live:
            try:
                dp = volume_drives_path(name)
                drives_p = str(dp)
                drives_projected = is_mountpoint(dp)
            except StorageError:
                drives_p = ""
        error_msg = ""
        members_info: list[dict[str, Any]] = []
        try:
            members_info = resolve_volume_members(vol, blockdevices=tree)
        except StorageError as exc:
            error_msg = str(exc)

        present_count = sum(1 for m in members_info if m["present"])
        total_count = len(vol["members"])
        degraded = bool(members_info) and 0 < present_count < total_count
        missing_all = bool(members_info) and present_count == 0

        if error_msg:
            health = "error"
        elif missing_all:
            health = "missing"
        elif degraded:
            health = "degraded"
        else:
            health = "healthy"

        usage = _statvfs_usage(mp) if mounted else {"total_bytes": 0, "used_bytes": 0, "avail_bytes": 0}
        btrfs_stats = _btrfs_device_stats(mp) if (mounted and vol["fs"] == "btrfs") else None
        if btrfs_stats and btrfs_stats.get("total_errors", 0) > 0 and health == "healthy":
            health = "io-errors"

        vol_reports.append(
            {
                "name": name,
                "topology": vol["topology"],
                "fs": vol["fs"],
                "raid": vol["raid"],
                "role": vol["role"],
                "encrypt": vol["encrypt"],
                "auto_mount": vol["auto_mount"],
                "fs_uuid": vol["fs_uuid"],
                "mount_options": vol["mount_options"],
                "created_at": vol["created_at"],
                "mountpoint": str(mp),
                "mounted": mounted,
                "drives_path": drives_p if vol["role"] in ("data", "shared") else "",
                "drives_projected": drives_projected,
                "health": health,
                "degraded": degraded,
                "error": error_msg,
                "members_present": present_count,
                "members_total": total_count,
                "members": members_info,
                "usage": usage,
                "btrfs_stats": btrfs_stats,
                "keyfile_on_encrypted_root": enc_root if vol["encrypt"] == "luks" else None,
            }
        )

    avail_disks = list_disks(root=root, blockdevices=tree, include_system=False)
    return {
        "schema": SCHEMA_VERSION,
        "root_encrypted": enc_root,
        "compute_storage": active_compute,
        "storage_root": str(storage_root_path(root=root)),
        "volumes": vol_reports,
        "available_disks": avail_disks,
    }


# ---- Compute integration helpers ----

def resolve_compute_volume(
    name: str,
    *,
    require_mounted: bool = True,
    root: str | os.PathLike[str] | None = None,
) -> tuple[dict[str, Any], Path]:
    """Validate that a named volume exists, allows compute access, and is mounted."""
    validate_volume_name(name)
    vol = get_volume(name, root=root)
    if vol is None:
        raise StorageError(f"storage volume not found: {name!r}")
    if vol["role"] not in ("shared", "compute"):
        raise StorageError(
            f"storage volume {name!r} has role={vol['role']!r}; compute access requires role 'shared' or 'compute'"
        )
    mp = volume_mountpoint(name, root=root)
    if require_mounted and not is_mountpoint(mp):
        raise StorageError(f"storage volume {name!r} is not mounted at {mp}")
    return vol, mp


def parse_compute_volume_spec(spec: str) -> dict[str, str]:
    """Parse NAME[:CONTAINER_PATH[:ro|rw]] for locachy-compute --volume."""
    if not spec or not isinstance(spec, str):
        raise StorageError("empty volume spec")
    parts = spec.split(":")
    if len(parts) > 3:
        raise StorageError(f"invalid volume spec {spec!r}; expected NAME[:CONTAINER_PATH[:ro|rw]]")
    name = validate_volume_name(parts[0])
    target = parts[1] if len(parts) >= 2 and parts[1] else f"/volumes/{name}"
    mode = parts[2] if len(parts) == 3 else "rw"
    if mode not in ("ro", "rw"):
        raise StorageError(f"invalid volume mode {mode!r}; expected ro or rw")
    if not os.path.isabs(target):
        raise StorageError(f"container path must be absolute: {target!r}")
    norm_target = os.path.normpath(target)
    if norm_target == "/" or ".." in norm_target.split("/"):
        raise StorageError(f"invalid container path: {target!r}")
    return {"name": name, "target": norm_target, "mode": mode}


# ---- Installer CLI spec parser ----

def parse_install_spec(spec: str) -> dict[str, Any]:
    """Parse an installer --volume CLI specification string.

    Supported formats:
      name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,encrypt=auto
      name=archive,mode=attach,dev=/dev/nvme1n1p1,role=shared
      name=pool,mode=array,devs=/dev/nvme1n1+/dev/nvme2n1,raid=raid1,role=compute,encrypt=auto
    """
    if not isinstance(spec, str) or not spec.strip():
        raise StorageError("empty install volume spec")
    kv: dict[str, str] = {}
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise StorageError(f"invalid install spec token {item!r}; expected key=value")
        k, v = item.split("=", 1)
        k = k.strip()
        v = v.strip()
        if not k:
            raise StorageError(f"empty key in install spec: {spec!r}")
        if k in kv:
            raise StorageError(f"duplicate key {k!r} in install spec: {spec!r}")
        kv[k] = v

    allowed_keys = {
        "name", "mode", "dev", "devs", "fs", "raid", "role", "encrypt", "auto_mount", "mount_options",
        "reserve_tail_mib",
    }
    unknown = set(kv.keys()) - allowed_keys
    if unknown:
        raise StorageError(f"unknown key(s) in install spec: {', '.join(sorted(unknown))}")

    name = validate_volume_name(kv.get("name", ""))
    mode = kv.get("mode", "")
    if mode not in VALID_MODES:
        raise StorageError(f"install spec {name!r}: invalid mode {mode!r}; expected format, attach, or array")
    role = validate_role(kv.get("role", "shared"))

    devs: list[str] = []
    if "dev" in kv and "devs" in kv:
        raise StorageError(f"install spec {name!r}: specify either dev= or devs=, not both")
    if "dev" in kv:
        if not kv["dev"].startswith("/"):
            raise StorageError(f"install spec {name!r}: dev must be an absolute path: {kv['dev']!r}")
        devs = [kv["dev"]]
    elif "devs" in kv:
        devs = [d.strip() for d in kv["devs"].split("+") if d.strip()]
        if not devs or any(not d.startswith("/") for d in devs):
            raise StorageError(f"install spec {name!r}: devs must be '+'-separated absolute paths")
    else:
        raise StorageError(f"install spec {name!r}: dev= or devs= is required")

    fs = kv.get("fs", "btrfs" if mode == "array" else ("ext4" if mode == "format" else ""))
    raid = kv.get("raid", "raid1" if mode == "array" else "")
    encrypt = kv.get("encrypt", "none" if mode == "attach" else "auto")
    if encrypt not in (*VALID_ENCRYPT, "auto"):
        raise StorageError(f"install spec {name!r}: invalid encrypt={encrypt!r}")

    auto_raw = kv.get("auto_mount", "true").lower()
    if auto_raw in ("1", "true", "on", "yes"):
        auto_mount = True
    elif auto_raw in ("0", "false", "off", "no"):
        auto_mount = False
    else:
        raise StorageError(f"install spec {name!r}: invalid auto_mount={auto_raw!r}")

    if mode == "format":
        if len(devs) != 1:
            raise StorageError(f"install spec {name!r}: mode=format requires exactly one device")
        if fs not in VALID_FORMAT_FS:
            raise StorageError(f"install spec {name!r}: mode=format requires fs in {', '.join(VALID_FORMAT_FS)}")
        raid = ""
    elif mode == "attach":
        if len(devs) != 1:
            raise StorageError(f"install spec {name!r}: mode=attach requires exactly one device")
        raid = ""
    else:  # array
        fs = "btrfs"
        btrfs_metadata_profile(raid, len(devs))

    reserve_tail_mib = 0
    if "reserve_tail_mib" in kv:
        if mode != "format":
            raise StorageError(f"install spec {name!r}: reserve_tail_mib applies only to mode=format")
        if not kv["reserve_tail_mib"].isdigit():
            raise StorageError(f"install spec {name!r}: reserve_tail_mib must be an integer MiB count >= 0")
        reserve_tail_mib = int(kv["reserve_tail_mib"])

    return {
        "name": name,
        "mode": mode,
        "devs": devs,
        "fs": fs,
        "raid": raid,
        "role": role,
        "encrypt": encrypt,
        "auto_mount": auto_mount,
        "mount_options": kv.get("mount_options", ""),
        "reserve_tail_mib": reserve_tail_mib,
    }
