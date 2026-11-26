# LOCachy Multi-Disk Storage Volumes

[中文](STORAGE.zh.md)

`locachy-storage` manages secondary internal disks (NVMe, SATA, SAS, VirtIO) and multi-disk Btrfs arrays without depending on kernel probe order (`/dev/nvmeXn1` or `/dev/sdX`). Every volume is tracked in `/etc/locachy/volumes.json` by GPT `PARTUUID` and LUKS2/filesystem `UUID`, mounted under `/srv/storage/<name>`, and exposed to the desktop and compute subsystem according to its role.

## Components

| File | Role |
|---|---|
| `link/locachy_storage.py` | Shared pure-Python library: schema validation, UUID/PARTUUID resolution, partitioning/LUKS2/mkfs, mount/unmount, `~/Drives` bind projection, `rc.conf` `compute_storage` policy, and installer spec parsing. |
| `link/locachy-storage` | CLI executable (`list-disks`, `status`, `create`, `set-role`, `set-auto`, `mount`, `umount`, `mount-all`, `umount-all`, `remove`, `set-compute-storage`). |
| `init/init.c` | Runs `locachy-storage mount-all` after drivers load (non-fatal if a secondary disk is missing) and `locachy-storage umount-all` with a 5-second bounded wait at shutdown before swap wipe and root remount. |
| `link/locachy-compute` | Enforces volume role policy for `--volume` bindings and `compute_storage` compute base routing. |

## Configuration (`/etc/locachy/volumes.json`)

Schema version `1`, written atomically with mode `0644`. Kernel device paths (`/dev/nvme*`, `/dev/sd*`) are never stored as member identities:

```json
{
  "schema": 1,
  "volumes": [
    {
      "name": "data",
      "topology": "single",
      "fs": "ext4",
      "raid": "",
      "role": "shared",
      "encrypt": "luks",
      "auto_mount": true,
      "fs_uuid": "11111111-2222-3333-4444-555555555555",
      "members": [
        {
          "part_uuid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
          "luks_uuid": "ffffffff-1111-2222-3333-444444444444",
          "mapper": "locachy-vol-data-0",
          "keyfile": "/etc/locachy/keys/vol-data-0.key"
        }
      ],
      "mount_options": "",
      "created_at": "2026-11-24T00:00:00Z"
    }
  ]
}
```

- `name`: `^[a-z0-9][a-z0-9._-]{0,31}$` (unique, never `.` or `..`).
- `topology`: `single` (1 member) or `btrfs-array` (2+ members; 4+ required for `raid10`).
- `fs`: `ext4`, `btrfs`, `xfs`, `ntfs`, `exfat`, or `vfat`. Formatting supports `ext4` and `btrfs`; `btrfs-array` requires `btrfs`.
- `raid`: empty for `single`; `single`, `raid0`, `raid1`, or `raid10` for `btrfs-array`.
  - Btrfs arrays always protect metadata (`-m raid1`, or `-m raid1c3` when `raid1` has 3+ members), even when the data profile is `single` or `raid0`.
  - Adding two members from the same physical parent disk into a `btrfs-array` is rejected.

## Roles and Isolation

| Role | `/srv/storage/<name>` | `~/Drives/<name>` | `locachy-compute` (`--volume`, `compute_storage`) |
|---|---|---|---|
| `data` | Mounted | Bind-projected (`MS_NOSUID\|MS_NODEV`) | Refused |
| `compute` | Mounted | Not projected | Allowed |
| `shared` | Mounted | Bind-projected (`MS_NOSUID\|MS_NODEV`) | Allowed |

- `~/Drives` is prepared idempotently with `drives.c:setup_top()` as a root-owned `0755` shared mountpoint so containers see projections via shared/rslave propagation.
- Because `drives.c` ignores `DISK_SYSTEM` disks and rejects eject requests for paths not in its removable `drives[]` table, users cannot accidentally eject an internal volume.
- `locachy-storage set-role NAME ROLE` updates `volumes.json` and immediately adds or removes the `~/Drives/<name>` bind projection on a mounted volume. Downgrading the active `compute_storage` volume to `data` is refused until `compute_storage` is cleared.

## Encryption and Keyfiles

- With `--encrypt luks` (or `--encrypt auto` when `/` is on LUKS2), each member partition gets a GPT partition, a LUKS2 header, and a 64-byte random keyfile at `/etc/locachy/keys/vol-<name>-<i>.key` (directory `0700`, keyfiles `0600`).
- Passing `--passphrase-file PATH` adds an extra recovery passphrase slot so the volume can also be unlocked in rescue mode.
- `status --json` reports `keyfile_on_encrypted_root` for encrypted volumes so settings and operators can see whether keyfiles rest on an encrypted root filesystem.
- `remove NAME` detaches the volume and preserves its LUKS keyfiles by default so encrypted data is never lost by accident; pass `--delete-key` or `--wipe` explicitly to delete keyfiles.

## Multi-NVMe Resolution and Safety

- Members are resolved dynamically from `lsblk` by `UUID` (LUKS2) and `PARTUUID` (GPT). Reordering `/dev/nvme0n1` and `/dev/nvme1n1` across boots does not affect volume mapping.
- If multiple block devices report the same member `UUID` or `PARTUUID` (for example, a raw clone attached simultaneously), resolution fails closed with `Ambiguous member UUID` instead of guessing.
- Disks backing `/`, `/efi`, `/boot`, swap, live media (`/run/archiso`, `/run/initramfs`), an already-registered volume, or Maintenance Windows (`locachy-winmaint`) cannot be formatted. Maintenance Windows partitions (identified by GPT names `LOCACHY_WIN_ESP`, `LOCACHY_WIN_MSR`, `LOCACHY_WIN_C`, or by `PARTUUID`s in `/var/lib/locachy/winmaint/state.json`) are never attachable and are refused by `remove --wipe`; other partitions on a shared disk can still be attached, and the disk remains protected from formatting even after its storage volume is removed.
- Btrfs arrays unlock all present LUKS members and run `btrfs device scan` before mounting. Missing members are never silently mounted degraded; `status` reports `degraded`, and an operator must pass `mount NAME --degraded` explicitly.

## CLI Reference

```sh
locachy-storage list-disks [--json]
locachy-storage status [--json]
locachy-storage create --name data --mode format --role shared --dev /dev/nvme1n1 --fs ext4 --encrypt auto [--reserve-tail-mib N]
locachy-storage create --name archive --mode attach --role shared --dev /dev/nvme1n1p1
locachy-storage create --name pool --mode array --role compute --devs /dev/nvme1n1,/dev/nvme2n1 --raid raid1 --encrypt auto
locachy-storage set-role data compute
locachy-storage set-auto data off
locachy-storage mount pool [--degraded]
locachy-storage umount pool
locachy-storage mount-all
locachy-storage umount-all
locachy-storage set-compute-storage pool
locachy-storage remove pool [--wipe] [--delete-key]
```

`create` accepts `--root TARGET_ROOT --no-mount` so the installer can partition, format, or attach volumes and write `TARGET_ROOT/etc/locachy/volumes.json` and `TARGET_ROOT/etc/locachy/keys/*.key` offline. `locachy_storage.parse_install_spec()` parses installer `--volume` specifications (`name=...,mode=format|attach|array,...`).

## Reserved tail for Maintenance Windows

`create --mode format --reserve-tail-mib N` (or `reserve_tail_mib=N` in an installer spec) partitions the disk so that partition 1 ends `N` MiB before the end of the usable space. The tail stays unallocated, and `locachy-winmaint create --disk ...` places Maintenance Windows there. `N` must be 0 or more, and the volume keeps at least 1 GiB. Partitions are 1 MiB aligned. `list-disks` and `status --json` report `serial`, `log_sec`, `free_tail_bytes` (unallocated bytes after the last partition, excluding the backup GPT; `null` when a partition's start is unknown, so the figure is never overstated), and `maintenance_windows` on disks and partitions. A disk holding Maintenance Windows partitions reports `in_use_reason` containing `Maintenance Windows` and `available_for_format: false`, and each Windows partition reports `attachable: false`.

## Mount Options

| Filesystem | Kernel Type | Default Options |
|---|---|---|
| `btrfs` | `btrfs` | `noatime,compress=zstd,discard=async,nosuid,nodev` |
| `ext4` | `ext4` | `noatime,nosuid,nodev` |
| `xfs` | `xfs` | `noatime,nosuid,nodev` |
| `ntfs` | `ntfs3` | `uid=1000,gid=1000,dmask=022,fmask=133,nosuid,nodev` |
| `exfat` | `exfat` | `uid=1000,gid=1000,dmask=022,fmask=133,nosuid,nodev` |
| `vfat` | `vfat` | `uid=1000,gid=1000,umask=022,showexec,utf8,shortname=mixed,flush,nosuid,nodev` |

Freshly formatted `ext4` and `btrfs` root directories are chowned to uid/gid `1000:1000`.

## Tests

```sh
src/tools/test-storage.sh   # schema, NVMe swap, duplicate-UUID fail-closed, --root, key perms, mkfs.ext4/btrfs RAID1, live ~/Drives projection
src/tools/test-compute.sh   # includes --volume role enforcement and compute_storage fail-closed routing
```

Test seams use `LOCACHY_*` environment variables: `LOCACHY_VOLUMES_FILE`, `LOCACHY_KEYS_DIR`, `LOCACHY_STORAGE_ROOT`, `LOCACHY_DRIVES_DIR`, `LOCACHY_STORAGE_RC` (when unset or empty, `LOCACHY_RC_CONF` is used; when both are unset or empty, `/etc/locachy/rc.conf`; each must be an absolute path when set), `LOCACHY_STORAGE_LSBLK_JSON`, and `LOCACHY_STORAGE_MOUNTINFO`.
