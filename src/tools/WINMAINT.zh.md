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

在多盘机器上，内核每次启动时的磁盘编号顺序可能互换（例如 `/dev/nvme0n1` 与 `/dev/nvme1n1`）。因此 `status`、`vm`、`boot-once`、`boot-default`、`boot-guard`、`sanitize-esp` 与 `reclaim` 会读取全部整盘，并使用带有已记录 `disk_guid` 的那块盘，不论它当前的设备名是什么。若没有任何磁盘带有该 GUID，或有两块（例如同时接入了整盘克隆盘），则 `locachy-winmaint` 直接拒绝操作。

选择磁盘：`locachy-winmaint status --json` 会列出 `candidate_disks`（除 live 介质所在盘以外的整盘 GPT 磁盘，字段为 `path`、`model`、`serial`、`size_bytes`、`is_root`、`free_tail_gib`、`disk_guid`、`has_maintenance_windows`），并报告已配置的维护 Windows 的 `resolved_disk` 与 `on_root_disk`。在无原始块设备读权限的非特权进程（例如 `settings`）下，`status` 仅以只读方式回退读取 `lsblk` 提供的 GPT 元数据；涉及修改或启动操作的特权子命令（`create`、`vm`、`reclaim`、`boot-once`、`boot-default`、`boot-guard`、`sanitize-esp`）始终直接读取磁盘上的原始 GPT 扇区。`create --disk PATH` 将分区三元组放到指定磁盘（默认为系统盘）。`create --root DIR --disk PATH` 为正在安装的系统写入 `DIR/var/lib/locachy/winmaint/state.json`，此时必须指定 `--disk`。若 `--disk` 上就是 `DIR` 的根分区所在盘，则与本系统一样记录 `on_root_disk` 与该根分区的 `root_part_uuid`，这样之后 `reclaim` 才会把根分区扩展到释放出的空间。`DIR` 必须是该根分区的挂载点（通过 `findmnt` 查找）；否则会给出警告说未找到根分区，并把 `--disk` 当作另一块盘记录。`--root` 绝不收缩任何根分区，因此只有尾部空闲空间才算数。尾部可用空间不足的磁盘会被拒绝。

安装期间通过 `locachy-install --winmaint-disk` 指定磁盘时，目标必须是可写、使用 512 字节逻辑扇区的整盘（GPT 解析按 512 字节扇区处理），不能位于 live 介质或当前运行系统所在盘，也不能与 Btrfs 阵列（`array`）共用同一块盘。预留尾部大小为 `winmaint_gib * 1024 + 256` MiB：在系统盘上由分区步骤直接预留；若与副盘上新建格式化的附加数据卷（`mode=format`）共用，则由 `extra-volumes` 阶段的 `locachy-storage --reserve-tail-mib` 在盘尾预留（数据卷至少保留 1 GiB 加上 GPT 开销）；若副盘不格式化，则必须是 GPT 盘或空白盘（拒绝已有裸文件系统、MBR/未知分区表或已有维护 Windows 分区的磁盘），且最后一个分区之后已有至少该大小的未分配尾部空间（分区之间的空隙不计入）。在写入任何内容之前，安装器还会拒绝 `--disk` 或 `--format-root` 指定的、含有或本身就是维护 Windows 分区的目标，以及位于这类磁盘上的附加数据卷（格式化或建阵列）或位于这类分区上的附加数据卷（挂载接入）。`winmaint` 阶段在 `extra-volumes`、`boot`、`secure-boot-enroll` 与 UEFI 登记之后执行，调用 `locachy-winmaint create --root DIR --disk PATH --size-gib N`；续装时，一旦目标系统已记录维护 Windows 就会原样保留，不能再改盘或改容量。安装完成后，`locachy-storage` 会按 GPT 分区名与 `state.json` 中记录的 PARTUUID 识别维护 Windows 分区：包含这些分区的整盘拒绝格式化和建阵列，这些分区（以及包含它们的整盘）拒绝挂载接入，清理数据卷时也拒绝擦除它们（非系统盘上的其他无关分区仍可单独接入）。


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

`locachy-winmaint vm [--iso Win11.iso] [--virtio-iso virtio-win.iso] [--memory 4096] [--cpus 4] [--disk-bus nvme|virtio] [--nic e1000e|virtio] [--share-dir 路径[:标签]]` 在 LOCachy 运行期间通过虚拟机完成 Windows 安装与常规更新，同时对 LOCachy 自身分区做到完全物理隔离：

- **`dm-linear` 隔离虚拟 NVMe 磁盘（`/dev/mapper/locachy-winmaint-disk`）：** 纯 Python GPT 构造器生成 1 MiB 头部镜像（保护性 MBR + 主 GPT）和 1 MiB 尾部镜像（备份 GPT），其中填入与物理磁盘**完全一致的 Disk GUID** 以及三个尾部分区**完全一致的 Partition GUID**，通过 5 段 `dm-linear` 映射表（`head + p4 + p5 + p6 + tail`）拼装出虚拟磁盘。LOCachy 的 `p1`/`p2`/`p3` 没有任何一个扇区被映射进虚拟机。由于 GPT 下的 Windows BCD 依赖 `(DiskGUID, PartitionGUID)` 而非物理 LBA 偏移定位系统分区，在虚拟机内写入的 BCD 在裸机启动时可直接识别。
- **Hyper-V 虚拟化加速与 `io_uring` 存储（默认 `stornvme.sys`）：** QEMU 显式启用 Hyper-V 特性集（`hv-relaxed`、`hv-vapic`、`hv-spinlocks=0x1fff`、`hv-time`、`hv-vpindex`、`hv-synic`、`hv-stimer`、`hv-stimer-direct`、`hv-reset`、`hv-frequencies`、`hv-tlbflush`、`hv-ipi`，不使用 `hv-passthrough` 或 `hv-default`）。虚拟磁盘以 `cache=none,aio=io_uring` 挂载；默认 `--disk-bus nvme`（`-device nvme,drive=disk0,serial=locachy-winmaint`），与裸机 NVMe 启动共用 Windows 自带的 `stornvme.sys`。`--disk-bus virtio` 改用带独立 `iothread` 和 `discard=on` 的 `virtio-blk-pci`（需 `viostor` 驱动）。从 `virtio-blk` 虚拟机切换回裸机 NVMe 启动的保护由下述 `locachy-storage.ps1` 负责，**尚未在真实 Windows 硬件上实测**，默认仍保持安全的 `nvme`。
- **网络、显示、熵源与 `virtiofs` 目录共享：**
  - `--nic e1000e`（默认，Windows 内置驱动）或 `--nic virtio`（`virtio-net-pci`，需 `NetKVM`）；宿主机网络后端在 `PATH` 存在 `passt` 且 QEMU 支持 `-netdev passt` 时优先使用 `passt`，否则回退到用户态 NAT（`-netdev user`）。
  - 启用 `-device virtio-rng-pci` 提供熵源；宿主机 QEMU 具备 `virtio-vga` 模块（Arch 的 `qemu-desktop` / `qemu-hw-display-virtio-vga`）时附加 `-vga none -device virtio-vga`，否则保留 QEMU 标准 VGA。`virtio-vga` 在 Windows 内的实际行为**尚未实测**。
  - `--share-dir 路径[:标签]`（可重复）通过 `virtiofs` 共享宿主机目录（`memory-backend-memfd,share=on` + `vhost-user-fs-pci` + 宿主机 `virtiofsd`；Windows 内需 `viofs` / WinFsp，裸机启动时即插即用保持休眠，客户机挂载**尚未实测**）。
- **QMP 控制套接字与 Wayland 显示：** 当传入 `--qmp-sock 路径` 或设置 `LOCACHY_VM_QMP_SOCK` 时，`locachy-winmaint vm` 会在该 unix 套接字暴露 QEMU QMP 监视器（退出时清理）；`locachy-winmaint vm-stop` 通过它发送 `system_powerdown`（配合 `--quit-after` 在等待 `--wait` 秒后发送 `quit`）以干净关闭虚拟机。在 Wayland 会话下，GTK 显示以 `gtk,gl=off` 运行。所有 GTK 显示都带 `window-close=off`：关闭维护 Windows 的窗口（栏上的中键、概览里的关闭、窗口自己的按钮）不会让 QEMU 退出，从而不会在真实分区的 NTFS 尚未干净卸载时切断 Windows 的电源。停止 Windows 应从主屏幕的 QMP 停止，或在 Windows 内正常关机。
- **独立 OVMF 与模拟 TPM 2.0：** 虚拟机使用只读 `OVMF_CODE` 与私有 `/var/lib/locachy/winmaint/ovmf-vars.fd`，并启动 `swtpm` 模拟 TPM 2.0，因此安装与 Windows Update 不会改写物理主板 UEFI NVRAM。
- **无人值守加固种子盘（`seed.img`）：** 传入 `--iso`、`--seed` 或 `--virtio-iso` 时挂载只读 MBR 分区 FAT 可移动 USB 磁盘（`usb-storage,removable=on`；不含驱动时为 16 MiB FAT16；传入 `--virtio-iso` 时转为 FAT32，从 `--virtio-os w11|w10` 提取 `$WinPEDriver$\viostor` 与 `$WinPEDriver$\NetKVM`，并同时将该 ISO 挂为第二张光盘），内含 `autounattend.xml` 与首启脚本：
  - 跳过在线微软账户引导（`BypassNRO`）；
  - 禁用自动 BitLocker / 设备加密（`PreventDeviceEncryption=1`）；
  - 关闭休眠与快速启动（`powercfg /h off`、`HiberbootEnabled=0`）；
  - 将硬件实时时钟设为 UTC（`RealTimeIsUniversal=1`），避免返回 LOCachy 后时间偏移；
  - 将 Windows 侧脚本安装在 `%SystemRoot%\System32\LOCachy`，包括存储保护、识别 Windows-first 的关机脚本和 Return to LOCachy 脚本；
  - 把 `LOCachyStorageBoot` 注册为 SYSTEM 的 `ONSTART` 任务，并关闭电池状态下的启动/停止限制；它检查固件启动顺序，并清除 `StartOverride`、把 `stornvme` 与 `viostor` 设为 `Start=0`；
  - 把同步的本地组策略计算机关机脚本注册为只重复存储步骤（`Machine\Scripts\Shutdown\locachy-shutdown.cmd`）。注册时删除旧的 Event 1074 任务，因为任务计划程序可能在关机时终止它。
- **安装介质由 `locachy-winiso` 提供：** `vm` 没有给出 `--iso`（或 `--virtio-iso`），且 `LOCACHY_WIN_ESP` 上还没有 Windows（读不到 `\EFI\Microsoft\Boot\bootmgfw.efi`）时，若存在 `locachy-winiso`（与 `locachy-winmaint` 同目录，或 `/usr/local/bin`），就执行 `locachy-winiso path windows`（或 `path virtio-win`）。约定：标准输出只打印一个绝对路径并以 0 退出；没有这个 ISO 时以 3 退出。遇到 3、其他输出（会在标准错误给出警告），或命令不存在，都不挂载任何 ISO，与之前一致。`--iso`、`--virtio-iso` 给出的路径，以及 `locachy-winiso` 给出的路径，都必须是普通文件：目录与设备会被拒绝（root 身份的 QEMU 会读取这些路径）。命令行上给出的选项始终优先；Windows 装好之后不会擅自挂载任何介质。`create` 不接收 ISO，因此不去询问。

## 驱动种子盘（`driver-seed`）

`locachy-winmaint driver-seed --virtio-iso 路径 --out 镜像 [--virtio-os w11|w10]` 生成一个小型 MBR 分区 FAT16 镜像（virtio-win 0.1.302 约 30 MiB），分区根目录下只有 `$WinPEDriver$\viostor` 与 `$WinPEDriver$\NetKVM`，即所选 Windows 版本的 amd64 目录，提取方式与种子盘相同。`路径` 必须是 virtio-win ISO 普通文件（块设备与目录会被拒绝）。该命令不需要 root（依赖 bsdtar、sfdisk、mkfs.fat 与 mtools），会整体替换 `镜像`，标准输出只打印镜像的绝对路径（成功退出码 0；错误写入标准错误并以 1 退出）。Windows 安装程序会从任何可移动磁盘上的 `$WinPEDriver$` 加载驱动，因此 `locachy-app` 可以把这个镜像以只读 `usb-storage,removable=on` 挂给自定义的 Windows 虚拟机。

## 四层启动顺序保护

1. **第一层（禁止 `p4` 回退劫持）：** `LOCACHY_WIN_ESP` 绝不保留 `\EFI\BOOT\BOOTX64.EFI`。每次虚拟机退出、每次 `boot-once` 前以及 LOCachy 启动后的 `boot-guard` 都会调用 `sanitize-esp` 清除该文件。
2. **第二层（更新走虚拟 NVRAM）：** 安装和常规 Windows Update 在 QEMU/KVM 内对着私有 `ovmf-vars.fd` 完成，不接触物理主板 NVRAM。
3. **第三层（仅使用一次性 `BootNext`）：** `locachy-winmaint boot-once` 在虚拟机锁被占用时直接拒绝，校验 `p4:\EFI\Microsoft\Boot\bootmgfw.efi` 存在后，通过 `locachy-efi --bootnext-entry`（内部使用 `efibootmgr --create-only`，绝不写入 `BootOrder`）登记 `LOCachy Maintenance Windows` 并设置一次性 `BootNext`。固件启动 Windows 时会消耗 `BootNext`，之后按常规 `BootOrder` 启动。
4. **第四层（双向修复 `BootOrder`）：** `locachy-efi --ensure-first`（由 `heal-boot` 与 `boot-guard` 调用）把 `LOCachy` 放回 `BootOrder` 首位，并把维护 Windows 条目从 `BootOrder` 移除；Windows 侧由开机任务 `LOCachyStorageBoot` 通过 BCDEdit 修复顺序。在 Windows 设为先启动时（见下文），两侧都保留这一顺序。Windows 关机脚本只修复存储驱动设置。

## 先由 Windows 启动（`boot-default`）

在裸机上装驱动时 Windows 会反复重启，只用 `boot-once` 不够。`locachy-winmaint boot-default windows [--reboot]` 让 Windows 在每次重启时都先启动，直到切回为止：

- 与 `boot-once` 相同的检查通过之后（锁空闲、存在 `bootmgfw.efi`、已清除 `\EFI\BOOT\BOOTX64.EFI`），`locachy-efi --windows-first` 会创建或复用名为 `LOCachy Maintenance Windows (default)` 的条目（指向 `p4:\EFI\Microsoft\Boot\bootmgfw.efi`），并把它放到 `BootOrder` 首位。`LOCachy`（及 `LOCachy (Previous)`）紧随其后，从不被删除；维护分区 ESP 上的其他条目则移出 `BootOrder`。在固件启动菜单里仍可一步选回 LOCachy。
- 之后才把模式写入 `state.json`：`"boot_default": "windows"` 与 `"boot_default_boot_id"`（本次启动的 `/proc/sys/kernel/random/boot_id`）。固件写入失败时什么都不记录。`--reboot` 则通过进程 1 立即重启。
- `status --json` 报告 `boot_default`（`"windows"` 或 `"locachy"`）、`boot_default_pending_reboot`（在本次启动中设置，下次重启才生效），以及 `boot_default_overridden`（之后的某次启动中 LOCachy 正在运行，说明是在固件启动菜单里选的 LOCachy）。普通 `status` 也给出同样的信息。
- "模式已设置"在两侧的判断标准一致：该条目在固件启动顺序中排在 `LOCachy` 之前。模式记录期间，LOCachy 自己的开机检查 `boot-guard` 与 `heal-boot` 调用 `locachy-efi --ensure-first --keep-windows-first`，保持这一顺序。若该条目已不在 `LOCachy` 之前，则照常把 `LOCachy` 放到首位，并以 3 退出；`locachy-winmaint` 随后删除该条目、清除模式。模式设置期间 `boot-once` 不做任何操作，因为 Windows 本来就会先启动。
- `locachy-winmaint boot-default locachy` 从 LOCachy 切回：先执行不带保持参数的 `--ensure-first`（LOCachy 回到首位，维护分区条目移出 `BootOrder`），再删除 `(default)` 条目（`--remove-entry --label "LOCachy Maintenance Windows (default)"`），最后清除模式。`reclaim` 会与该分区上的其他条目一并删除它。
- **Windows 侧。** `locachy-win-shutdown.ps1` 与 `locachy-return.ps1` 按分块结构与不变值解析 `bcdedit /enum firmware /v` 的输出，不依赖随语言变化的字段名（如英文的 `identifier` / `displayorder` / `description` 或中文的 `标识符` / `显示顺序` / `描述`）：以空行或 `---` 分隔线划分区块；区块内第一条以 `{GUID}` 结尾的行即为该条目的编号，`{fwbootmgr}` 同时按短名和完整 GUID `{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}` 识别；`{fwbootmgr}` 区块内紧跟在编号之后的首个键值行及其后缩进的 `{GUID}` 续行即为 `displayorder`；其余区块则通过右栏值与 `locachy-efi` 写入 NVRAM、不会被 Windows 翻译的三条精确 ASCII 描述（`LOCachy`、`LOCachy Maintenance Windows`、`LOCachy Maintenance Windows (default)`）逐字比对来识别条目。若描述为 `LOCachy Maintenance Windows (default)` 的条目在这一显示顺序中排在 `LOCachy` 之前，开机任务就不改动启动顺序，只执行存储步骤。全部用户桌面上的 **Return to LOCachy** 快捷方式运行 `locachy-return.ps1`：它自行提权（UAC），找到描述为 `LOCachy` 的条目，执行 `bcdedit /set {fwbootmgr} displayorder <编号> /addfirst`，把 `(default)` 条目移出显示顺序（`bcdedit /set {fwbootmgr} displayorder <编号> /remove`），立即运行一次守卫脚本让守卫重新生效，然后询问是否立即重启（`shutdown.exe /r /t 0`）。微软文档记载了这种写法：[BCD System Store Settings for UEFI](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcd-system-store-settings-for-uefi) 中的 `Bcdedit /set {fwbootmgr} displayorder {bootmgr} /addfirst`，以及 [BCDEdit Command-Line Options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdedit-command-line-options) 中对 `/displayorder` 的说明。`/remove` 的写法是守卫脚本原本就在用的，上述微软页面并未记载。下次 LOCachy 启动时，`boot-guard` 会发现已经解除，并清除模式。快捷方式与新脚本由种子盘的首次登录脚本写入，因此改动之前安装的 Windows 不会获得它们。
- **设置 → 系统 → 维护 Windows**，位于 "Boot Windows once" 之后，行名为 `Start in Windows by default (until switched back)`：显示当前模式（`[OFF] LOCachy starts first`、`[ON] Windows from the next restart`、`[ON] Windows starts first`，或"LOCachy 被固件启动菜单选中"）。模式关闭时连按两次回车打开，再按一次回车即切回。`Reboot into Windows now` 连按两次回车即设置模式并重启。两者都通过系统任务 `winmaint-boot-default` 执行（参数为 `windows`、`windows --reboot` 或 `locachy`），与 `winmaint-boot-once` 的走法相同。

## 安全启动与 TPM 兼容性

- 使用 `locachy-tpm secure-boot --enroll --microsoft` 同时导入本机自有密钥与微软第三方 UEFI 密钥，使 LOCachy 签名 UKI 和微软签名的 `bootmgfw.efi` 都能在安全启动开启时正常验证。
- LOCachy 自有密钥模式将 LUKS2 绑定到 PCR 7 与签名 PCR 11（不绑定 PCR 1），设置一次性 `BootNext` 不会导致回到 LOCachy 时 TPM 解锁失效。
- 裸机 Windows 默认关闭 BitLocker / 设备加密，不会抢占或重配置物理 TPM。

## 配置与设置项对接

设置界面的分区行与不带 `--size-gib` 的 `create` 使用的 `rc.conf` 配置键：

- `winmaint_size_gib="48"` — 尾部 Windows 分区的首选容量（40–4096 GiB）。Windows 虚拟机的内存（`vm --memory`，4096 MiB）与虚拟核心数（`vm --cpus`，4）只能通过命令行参数指定，设置界面暂无对应行。

## 验证状态（请先阅读）

- 自动化测试已验证：QEMU 命令行（CPU、存储、网络、显示、熵源、virtiofs、virtio-win 光盘）、在 KVM 下 QEMU 对该参数的接受情况（宿主机 QEMU 11.1.2）、通过替身程序检查的 `virtiofsd` 命令行、种子盘布局（`$WinPEDriver$`、脚本）以及客户机脚本文本。
- **已在 QEMU/KVM 的真实 Windows 11 26H2 客户机中验证：** 使用可移除的驱动种子盘并安装到 `virtio-blk` 后，同一安装盘切换到模拟 NVMe、网卡为 `e1000e`，能够在没有安装光盘和种子盘的情况下进入桌面。Windows 中 `stornvme` 与 `viostor` 均为 `Start=0` 且没有 `StartOverride`。开机任务在虚拟机重置后把 LOCachy 恢复到首位，且计划任务的两个电池限制均已关闭。另一次 Windows 重启保持了有意设置的 Windows-first 条目；Return to LOCachy 脚本也针对真实 BCDEdit 输出把 LOCachy 恢复到首位。
- **关机时的存储修复（Windows 11 26H2 虚拟机）：** 关闭开机任务后，正常关机脚本删除了预先注入的
  `viostor\StartOverride\0=3`；只记录执行、不修改注册表的对照脚本则保留了该值。两次均在客户机正常关机后、再次启动前离线读取 SYSTEM 配置单元。
- **本地组策略关机脚本的边界：** 关闭开机任务后，独立的 OVMF 重启边界仍会把 Windows Boot Manager 留在 LOCachy 前面。脚本确实以 Local System 令牌运行，但该令牌执行 `bcdedit` 时无法打开固件 BCD 存储。因此关机脚本现在只负责存储驱动；固件启动顺序在 Windows 开机后由开机任务修复。若要下一次重启直接进入 LOCachy，请先使用 **Return to LOCachy**。
- **尚未验证：** OMEN 裸机 NVMe 启动和真实固件行为、真实电池供电运行，以及 Windows 挂载 `virtiofs` 共享目录。OVMF 结果不能替代这些验证。

## 自动化测试

在 Linux 开发机上运行无需 root 权限的测试脚本：

```sh
bash tools/test-winmaint.sh
```

在有 `/dev/kvm` 与 OVMF 的机器上，`bash tools/test-winmaint-ovmf.sh 运行根目录 新输出目录` 在真实 OVMF 固件中运行启动保护的 `efibootmgr` 写入与 `sanitize-esp`（私有 NVRAM 变量，一次性磁盘上的 FAT32 分区），以 PASS 行为准。

测试覆盖纯 Python GPT 构造与 `sfdisk -V` 校验、物理盘与虚拟 `dm-linear` 布局间的 Disk/Partition GUID 一致性及 `p1`/`p2`/`p3` 完全隔离、稀疏镜像上的在线创建/清理/回收、收缩计划计算与同时使用 `gcc` / `musl-gcc -static` 编译的 `init.c` 解析器与多 ESP 判别测试、`autounattend.xml` 及种子脚本校验、替身 `efibootmgr` 下的 `BootNext`/`--ensure-first` 测试、虚拟机互斥锁拦截以及 `locachy-boot` 精简/收缩两态 initramfs 打包检查。此外还检查：原始块设备读取被拒绝时，非特权 `status` 经 `lsblk` 解析磁盘与根布局（模拟），而特权磁盘解析仍只读取原始 GPT。针对"先由 Windows 启动"，测试还包括：`test-efi.py` 在模拟 `efibootmgr` 下检查 `BootOrder` 的变化（Windows 先启动、保持、被 Return to LOCachy 解除、切回，LOCachy 条目从不被删除）；`boot-default` 的状态、`status` 字段及其守卫/修复/`boot-once` 行为；生成的 Windows 脚本与快捷方式（精确的 `bcdedit` 命令）；`driver-seed` 镜像的内容；以及 `vm` 对 `locachy-winiso` 的查询（用替身程序检查）。
