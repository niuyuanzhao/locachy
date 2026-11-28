# LOCachy 多盘存储卷

[English](STORAGE.md)

`locachy-storage` 负责管理副内置磁盘（NVMe、SATA、SAS、VirtIO）以及多盘 Btrfs 阵列，不依赖内核探测顺序（`/dev/nvmeXn1` 或 `/dev/sdX`）。每个存储卷都在 `/etc/locachy/volumes.json` 中通过 GPT `PARTUUID` 与 LUKS2/文件系统 `UUID` 登记，统一挂载到 `/srv/storage/<名称>`，并按角色向桌面与计算子系统开放访问。

## 组成

| 文件 | 作用 |
|---|---|
| `link/locachy_storage.py` | 共享纯 Python 库：配置校验、UUID/PARTUUID 解析、分区/LUKS2/格式化、挂载与卸载、`~/Drives` 绑定投影、`rc.conf` 的 `compute_storage` 策略，以及安装器规格解析。 |
| `link/locachy-storage` | 命令行工具（`list-disks`、`status`、`create`、`set-role`、`set-auto`、`mount`、`umount`、`mount-all`、`umount-all`、`remove`、`set-compute-storage`）。 |
| `init/init.c` | 驱动加载完成后调用 `locachy-storage mount-all`（副盘缺失时仅记录日志，不阻断启动）；关机前在清零交换区和只读重挂根分区之前执行带 5 秒限时等待的 `locachy-storage umount-all`。 |
| `link/locachy-compute` | 对 `--volume` 绑定与 `compute_storage` 计算目录路由执行角色校验。 |

## 配置文件（`/etc/locachy/volumes.json`）

结构版本为 `1`，以 `0644` 权限原子写入。配置文件从不把 `/dev/nvme*` 或 `/dev/sd*` 作为成员标识保存：

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

- `name`：匹配 `^[a-z0-9][a-z0-9._-]{0,31}$`（卷名唯一，不能是 `.` 或 `..`）。
- `topology`：`single`（单成员）或 `btrfs-array`（至少 2 个成员，`raid10` 至少 4 个成员）。
- `fs`：支持 `ext4`、`btrfs`、`xfs`、`ntfs`、`exfat`、`vfat`。新建格式化支持 `ext4` 与 `btrfs`；`btrfs-array` 必须使用 `btrfs`。
- `raid`：`single` 拓扑下为空；`btrfs-array` 下为 `single`、`raid0`、`raid1` 或 `raid10`。
  - 多盘 Btrfs 阵列的元数据始终受冗余保护（`-m raid1`，`raid1` 且 3 盘及以上时使用 `-m raid1c3`），即使数据配置为 `single` 或 `raid0`。
  - 拒绝把同一物理盘上的两个成员加入同一个 `btrfs-array`。

## 角色与访问边界

| 角色 | `/srv/storage/<名称>` | `~/Drives/<名称>` | `locachy-compute`（`--volume`、`compute_storage`） |
|---|---|---|---|
| `data` | 已挂载 | 绑定投影（`MS_NOSUID\|MS_NODEV`） | 拒绝 |
| `compute` | 已挂载 | 不投影 | 允许 |
| `shared` | 已挂载 | 绑定投影（`MS_NOSUID\|MS_NODEV`） | 允许 |

- `~/Drives` 与 `drives.c:setup_top()` 幂等地建立为 root 拥有、`0755` 权限的共享挂载点，容器通过 shared/rslave 传播看到投影目录。
- `drives.c` 会忽略 `DISK_SYSTEM` 磁盘，并拒绝弹出不在可移动磁盘表中的路径，因此用户不会在文件管理器里误弹出内置存储卷。
- `locachy-storage set-role 名称 角色` 更新 `volumes.json`，若该卷已挂载，还会即时创建或撤除 `~/Drives/<名称>` 绑定投影。若某卷正被设为 `compute_storage`，在清除 `compute_storage` 之前禁止将其降级为 `data`。

## 加密与密钥文件

- 指定 `--encrypt luks`（或在根分区已启用 LUKS2 时使用 `--encrypt auto`）时，每个成员盘会建立 GPT 分区、LUKS2 头，并在 `/etc/locachy/keys/vol-<名称>-<i>.key` 生成 64 字节随机密钥文件（目录 `0700`，密钥文件 `0600`）。
- 传入 `--passphrase-file 路径` 会额外添加一个恢复口令槽，以便在救援模式下手动解锁该盘。
- `status --json` 对加密卷返回 `keyfile_on_encrypted_root`，提示密钥文件是否存放在已加密的根文件系统上。
- 默认执行 `remove 名称`（解绑）会保留 LUKS 密钥文件，避免误操作导致加密数据无法恢复；仅在显式指定 `--delete-key` 或 `--wipe` 时才删除密钥文件。

## 多 NVMe 设备解析与安全规则

- 成员设备每次都从 `lsblk` 按 LUKS2 `UUID` 与 GPT `PARTUUID` 动态查找，重启后 `/dev/nvme0n1` 与 `/dev/nvme1n1` 编号互换不影响挂载。
- 在启动早期（`init.c` 在 `udev-client` 启动前调用 `locachy-storage mount-all`，此时 `/run/udev` 为空），`probe_lsblk()` 会直接通过 `blkid -o export`、`sfdisk -J` 与 `/sys/class/block/*/uevent` 补齐空的 `PARTUUID`/`UUID`/`FSTYPE`/`PARTLABEL`；LUKS 解锁会确保 `/dev/mapper/control` 与 `/run/cryptsetup` 存在、按需加载 `dm_mod`/`dm_crypt`、传入 `--disable-external-tokens`，并在 udev 尚未创建节点时根据 sysfs 直接补建 `/dev/dm-N` 与 `/dev/mapper/<名称>`。若副盘在 `coldplug()` 之后稍晚完成枚举，`mount-all` 还会进行有界重试。
- 如果多个块设备报告相同的 `UUID` 或 `PARTUUID`（例如同时接入了整盘克隆盘），解析直接报错中止（`Ambiguous member UUID`），不做猜测。
- 凡是承载 `/`、`/efi`、`/boot`、交换区、Live 介质（`/run/archiso`、`/run/initramfs`）、已登记存储卷或维护用 Windows（`locachy-winmaint`）的磁盘，都禁止格式化。维护用 Windows 分区（按 GPT 分区名 `LOCACHY_WIN_ESP`、`LOCACHY_WIN_MSR`、`LOCACHY_WIN_C` 或 `/var/lib/locachy/winmaint/state.json` 中记录的 `PARTUUID` 识别）绝不可附加，也会被 `remove --wipe` 拒绝；共享盘上的其他分区仍可附加，即使移除了该盘上的存储卷，该盘仍禁止格式化。
- 多盘 Btrfs 阵列会先解锁全部在位 LUKS 成员并执行 `btrfs device scan`，再执行挂载。成员缺失时从不静默降级挂载；`status` 会报告 `degraded`，需由管理员显式执行 `mount 名称 --degraded`。

## 命令行参考

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

`create` 支持 `--root 目标根 --no-mount`，供安装器在离线目标根下完成分区、格式化或附加，并将配置与密钥写入 `目标根/etc/locachy/volumes.json` 与 `目标根/etc/locachy/keys/*.key`。`locachy_storage.parse_install_spec()` 用于解析安装器的 `--volume` 规格字符串（`name=...,mode=format|attach|array,...`）。

## 为维护用 Windows 预留的末尾空间

`create --mode format --reserve-tail-mib N`（或安装器规格中的 `reserve_tail_mib=N`）分区时，让第 1 分区在可用空间末尾之前留出 `N` MiB。尾部保持未分配状态，`locachy-winmaint create --disk ...` 会把维护用 Windows 放在那里。`N` 须为 0 或更大，且卷至少保留 1 GiB。分区按 1 MiB 对齐。`list-disks` 与 `status --json` 为磁盘报告 `serial`、`log_sec`、`free_tail_bytes`（最后一个分区之后未分配的字节数，不含备份 GPT；若某分区起始位置未知则为 `null`，以免高估），并在磁盘和分区上报告 `maintenance_windows`。含有维护用 Windows 分区的磁盘会在 `in_use_reason` 中标明 `Maintenance Windows` 且 `available_for_format` 为 `false`，各个 Windows 分区的 `attachable` 为 `false`。

## 挂载选项

| 文件系统 | 内核类型 | 默认选项 |
|---|---|---|
| `btrfs` | `btrfs` | `noatime,compress=zstd,discard=async,nosuid,nodev` |
| `ext4` | `ext4` | `noatime,nosuid,nodev` |
| `xfs` | `xfs` | `noatime,nosuid,nodev` |
| `ntfs` | `ntfs3` | `uid=1000,gid=1000,dmask=022,fmask=133,nosuid,nodev` |
| `exfat` | `exfat` | `uid=1000,gid=1000,dmask=022,fmask=133,nosuid,nodev` |
| `vfat` | `vfat` | `uid=1000,gid=1000,umask=022,showexec,utf8,shortname=mixed,flush,nosuid,nodev` |

新格式化的 `ext4` 与 `btrfs` 根目录会自动把属主设为 `1000:1000`。

## 设置界面入口

**设置 → 存储 → 存储卷** 列出已登记的存储卷与未纳管的内置磁盘。默认计算存储卷（`compute_storage`）在 **设置 → 计算与虚拟机 → 计算节点 → 计算存储卷** 中选择。

## 测试

```sh
src/tools/test-storage.sh   # 架构校验、NVMe 编号互换、重复 UUID 报错中止、--root 离线写入、密钥权限、真实 mkfs.ext4/btrfs RAID1、实时 ~/Drives 投影
src/tools/test-compute.sh   # 包含 --volume 角色校验与 compute_storage 未挂载拒退回验证
```

测试接缝使用 `LOCACHY_*` 环境变量：`LOCACHY_VOLUMES_FILE`、`LOCACHY_KEYS_DIR`、`LOCACHY_STORAGE_ROOT`、`LOCACHY_DRIVES_DIR`、`LOCACHY_STORAGE_RC`（未设置或为空时改用 `LOCACHY_RC_CONF`；两者都未设置或为空时为 `/etc/locachy/rc.conf`；设置时必须是绝对路径）、`LOCACHY_STORAGE_LSBLK_JSON` 与 `LOCACHY_STORAGE_MOUNTINFO`。
