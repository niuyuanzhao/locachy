# 维护用尾部 Windows 分区

[English](WINMAINT.md)

`locachy-winmaint` 用于在磁盘尾部（或指定的第二块磁盘上）按需创建、管理和随时在线回收一组独立的裸机 Windows 分区。该方案面向偶发的厂商固件刷写、硬件诊断或仅支持 Windows 的专用工具，既不把主系统变成脆弱的双启动，也不影响 LOCachy 的安全启动与 TPM 磁盘自动解锁。

## 分区布局

标准 LOCachy GPT 磁盘包含 `p1`（LOCachy ESP，512 MiB，FAT32 卷标 `LOCACHY_EFI`）、可选的 `p2`（交换分区）以及占满剩余空间的根分区（`p3`，无交换分区时为 `p2`，内含 ext4 `locachy-root`，启用全盘加密时外层为 LUKS2 `locachy-crypt`）。

维护 Windows 在磁盘尾部追加三个分区，同时通过 GPT 分区名称和 `/var/lib/locachy/winmaint/state.json` 中记录的 GUID 进行双重核对：

| 槽位 | GPT 分区名 | 大小 | GPT 类型 GUID | 用途 |
| --- | --- | --- | --- | --- |
| ESP | `LOCACHY_WIN_ESP` | 128 MiB | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | 仅供 Windows Boot Manager 使用的独立 FAT32 ESP（卷标 `WINMAINT_EP`）；绝不保留 `\EFI\BOOT\BOOTX64.EFI` |
| MSR | `LOCACHY_WIN_MSR` | 16 MiB | `E3C9E316-0B5C-4DB8-817D-F92DF00215AE` | Microsoft 保留分区（清零） |
| Windows | `LOCACHY_WIN_C` | 40–4096 GiB（默认 48 GiB） | `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` | Windows `C:` NTFS 分区 |

进程 1（`init.c`）在启动时会收集所有 FAT32 分区，并在挂载 vfat 后优先选择带有 `LOCACHY_EFI` 卷标及 `/efi/EFI/locachy/locachy.efi` 的主 ESP，因此尾部的 `LOCACHY_WIN_ESP` 不会干扰 LOCachy 挂载 `/efi`。

## 按需创建与零重启回收

### 创建（`locachy-winmaint create --size-gib N [--disk /dev/...]`）

最小容量 40 GiB，默认 48 GiB。若需收缩 LOCachy 根分区，至少为 `locachy-root` 保留 8 GiB 可用空间。

- **在线创建（零重启）：** 目标为第二块磁盘，或主磁盘尾部已有足够未分配空间时，`locachy-winmaint` 直接通过 `sfdisk --append` 创建三个尾部分区，格式化 `LOCACHY_WIN_ESP` 为 FAT32、清零 `LOCACHY_WIN_MSR` 并写入 `/var/lib/locachy/winmaint/state.json`。
- **一次性离线收缩（重启一次）：** 当 `locachy-root` 已占满主磁盘时，`locachy-winmaint` 会通过 `dumpe2fs` 与 `resize2fs -P` 校验可用空间，并在根文件系统内部写入纯 `key=value` 格式的收缩计划 `/var/lib/locachy/winmaint/shrink-plan.conf`（启用加密时位于 LUKS2 内部，绝不放在未加密的 `/efi` 上）。`locachy-boot` 检测到该计划后，仅在本次构建的 initramfs 中临时打入 `e2fsck`、`resize2fs`、`sfdisk` 和 `partx`。下次冷启动时（休眠恢复时绝不执行），进程 1 核验根分区 `PARTUUID` 后依次执行 `e2fsck -f -p`、`resize2fs`、`cryptsetup resize`（如为 LUKS2）、收缩根分区并追加尾部三分区，随后消耗掉 `shrink-plan.conf`。进入系统后由 `locachy-winmaint finalize-create` 格式化 `LOCACHY_WIN_ESP`、保存 `state.json`，并再次调用 `locachy-boot` 恢复精简的常规 initramfs。

### 回收（`locachy-winmaint reclaim`）

回收全程在线完成，无需重启：

1. 获取互斥锁 `/run/locachy/winmaint.lock`（若虚拟机正在运行或尾部分区仍挂载则拒绝执行）。
2. 删除维护 Windows 的 UEFI 启动项及指向它的 `BootNext`。
3. 对照 `state.json` 严格核验磁盘 GUID 与三个尾部分区的 GUID、类型、名称，并确认它们位于磁盘最尾部。
4. 使用 `sfdisk --delete` 删除这三个尾部分区。
5. 若位于主系统盘且紧邻 `locachy-root`，则直接扩容根分区的 GPT 表项（`sfdisk -N`）、通知内核更新分区表（`partx -u`），再在线执行 `cryptsetup resize locachy-crypt`（如启用 LUKS2）与在线 `resize2fs`。
6. 清理 `state.json` 及虚拟机私有 OVMF 变量文件，并确认 `LOCachy` 仍位于 `BootOrder` 首位。

## 系统内 QEMU/KVM 安装与更新

`locachy-winmaint vm [--iso Win11.iso] [--memory 4096] [--cpus 4]` 在 LOCachy 运行期间通过虚拟机完成 Windows 安装与常规更新，同时对 LOCachy 自身分区做到完全物理隔离：

- **`dm-linear` 隔离虚拟 NVMe 磁盘（`/dev/mapper/locachy-winmaint-disk`）：** 纯 Python GPT 构造器生成 1 MiB 头部镜像（保护性 MBR + 主 GPT）和 1 MiB 尾部镜像（备份 GPT），其中填入与物理磁盘**完全一致的 Disk GUID** 以及三个尾部分区**完全一致的 Partition GUID**，通过 5 段 `dm-linear` 映射表（`head + p4 + p5 + p6 + tail`）拼装出虚拟磁盘。LOCachy 的 `p1`/`p2`/`p3` 没有任何一个扇区被映射进虚拟机。由于 GPT 下的 Windows BCD 依赖 `(DiskGUID, PartitionGUID)` 而非物理 LBA 偏移定位系统分区，在虚拟机内写入的 BCD 在裸机启动时可直接识别。
- **统一的存储驱动（`stornvme.sys`）：** 虚拟机通过 QEMU NVMe 控制器挂载虚拟盘（`-device nvme,drive=nvme0,serial=locachy-winmaint`），与裸机 NVMe 启动共用同一驱动栈，避免切换介质导致蓝屏。
- **独立 OVMF 与模拟 TPM 2.0：** 虚拟机使用只读 `OVMF_CODE` 与私有 `/var/lib/locachy/winmaint/ovmf-vars.fd`，并启动 `swtpm` 模拟 TPM 2.0，因此安装与 Windows Update 不会改写物理主板 UEFI NVRAM。
- **无人值守加固种子盘（`seed.img`）：** 传入 `--iso` 或 `--seed` 时挂载只读 FAT12 USB 磁盘，内含 `autounattend.xml` 与首启脚本：
  - 跳过在线微软账户引导（`BypassNRO`）；
  - 禁用自动 BitLocker / 设备加密（`PreventDeviceEncryption=1`）；
  - 关闭休眠与快速启动（`powercfg /h off`、`HiberbootEnabled=0`）；
  - 将硬件实时时钟设为 UTC（`RealTimeIsUniversal=1`），避免返回 LOCachy 后时间偏移；
  - 安装本地组策略关机脚本及 Event 1074 计划任务（`C:\ProgramData\LOCachy\locachy-win-shutdown.ps1`），在 Windows 关机或重启时确保 `LOCachy` 保持在固件启动顺序首位。

## 四层启动顺序保护

1. **第一层（禁止 `p4` 回退劫持）：** `LOCACHY_WIN_ESP` 绝不保留 `\EFI\BOOT\BOOTX64.EFI`。每次虚拟机退出、每次 `boot-once` 前以及 LOCachy 启动后的 `boot-guard` 都会调用 `sanitize-esp` 清除该文件。
2. **第二层（更新走虚拟 NVRAM）：** 安装和常规 Windows Update 在 QEMU/KVM 内对着私有 `ovmf-vars.fd` 完成，不接触物理主板 NVRAM。
3. **第三层（仅使用一次性 `BootNext`）：** `locachy-winmaint boot-once` 在虚拟机锁被占用时直接拒绝，校验 `p4:\EFI\Microsoft\Boot\bootmgfw.efi` 存在后，通过 `locachy-efi --bootnext-entry`（内部使用 `efibootmgr --create-only`，绝不写入 `BootOrder`）登记 `LOCachy Maintenance Windows` 并设置一次性 `BootNext`。重启一次后自动回到 LOCachy。
4. **第四层（双向自愈 `BootOrder`）：** `locachy-efi --ensure-first`（由 `heal-boot` 与 `boot-guard` 调用）把 `LOCachy` 放回 `BootOrder` 首位，并把维护 Windows 条目从 `BootOrder` 移除；Windows 侧关机脚本 `locachy-win-shutdown.ps1` 也通过 `bcdedit /set {fwbootmgr} displayorder` 同步维护同一顺序。

## 安全启动与 TPM 兼容性

- 使用 `locachy-tpm secure-boot --enroll --microsoft` 同时导入本机自有密钥与微软第三方 UEFI 密钥，使 LOCachy 签名 UKI 和微软签名的 `bootmgfw.efi` 都能在安全启动开启时正常验证。
- LOCachy 自有密钥模式将 LUKS2 绑定到 PCR 7 与签名 PCR 11（不绑定 PCR 1），设置一次性 `BootNext` 不会导致回到 LOCachy 时 TPM 解锁失效。
- 裸机 Windows 默认关闭 BitLocker / 设备加密，不会抢占或重配置物理 TPM。

## 配置与设置项对接

设置界面的分区行与不带 `--size-gib` 的 `create` 使用的 `rc.conf` 配置键：

- `winmaint_size_gib="48"` — 尾部 Windows 分区的首选容量（40–4096 GiB）。Windows 虚拟机的内存（`vm --memory`，4096 MiB）与虚拟核心数（`vm --cpus`，4）只能通过命令行参数指定，设置界面暂无对应行。

## 自动化测试

在 Linux 开发机上运行无需 root 权限的测试脚本：

```sh
bash tools/test-winmaint.sh
```

在有 `/dev/kvm` 与 OVMF 的机器上，`bash tools/test-winmaint-ovmf.sh 运行根目录 新输出目录` 在真实 OVMF 固件中运行启动保护的 `efibootmgr` 写入与 `sanitize-esp`（私有 NVRAM 变量，一次性磁盘上的 FAT32 分区），以 PASS 行为准。

测试覆盖纯 Python GPT 构造与 `sfdisk -V` 校验、物理盘与虚拟 `dm-linear` 布局间的 Disk/Partition GUID 一致性及 `p1`/`p2`/`p3` 完全隔离、稀疏镜像上的在线创建/清理/回收、收缩计划计算与同时使用 `gcc` / `musl-gcc -static` 编译的 `init.c` 解析器与多 ESP 判别测试、`autounattend.xml` 及种子脚本校验、替身 `efibootmgr` 下的 `BootNext`/`--ensure-first` 测试、虚拟机互斥锁拦截以及 `locachy-boot` 精简/收缩两态 initramfs 打包检查。
