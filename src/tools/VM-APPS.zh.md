# 受管虚拟机应用（`vm="1"`）

[English](VM-APPS.md)

虚拟机应用指在 `/etc/locachy/containers/NAME.conf` 中声明 `vm="1"` 的 KVM 虚拟机。Home 会将它与容器一并列出，由 `locachy-app` 在共享的 `labwc` Wayland 合成器和独立的 `/sys/fs/cgroup/app_NAME` cgroup 中统一管理其启动、恢复、冻结、停止和移除。

虚拟机应用没有 OverlayFS 层，也没有容器软件包构建流程。`locachy-app build`、`install` 和 `rollback` 会直接拒绝它；`locachy-pkg` 在规划容器更新时会跳过它；`packages` 界面也不会列出它。在 **设置 → 计算与虚拟机 → 虚拟机** 中可直接列出并管理非 `winmaint` 虚拟机应用（回车在桌面启动、`e` 打开设置表单、`s` 关机、连按两次 Delete 提交 `app-remove`），也可通过 **新建虚拟机应用** 在 `/var/lib/locachy/vm-disks/` 创建空白 qcow2 磁盘或挂载已有 `vm_base`/`vm_disk` 镜像。

## 配置键

所有 `vm_*` 键来自封闭集合（`link/locachy-app` 中的 `VM_KEYS`）。启动时遇到未定义的 `vm_*` 键会直接报错拒绝；与所选 `vm_kind` 不相干的键同样会被拒绝；没有任何配置键可以透传原始 QEMU 参数。

| 键 | 取值 | 说明 |
| --- | --- | --- |
| `vm` | `"1"` | 将该条目声明为受管虚拟机应用。 |
| `vm_kind` | `winmaint`、`compute`、`custom` | 后端类型，留空默认 `custom`。 |
| `vm_name` | 名称 | `vm_kind="compute"` 对应的目标虚拟机名（默认取 `NAME`；用于其他类型时直接拒绝）。 |
| `vm_freeze` | `qmp`、`none` | 冻结策略（默认 `qmp`）。设为 `none` 时 `freeze` 为空操作。 |
| `vm_display` | `gtk`、`sdl`、`none` | QEMU 显示后端（默认 `gtk`）。 |
| `vm_memory` | 整数 MiB | 客户机内存（`custom` 默认 `4096`；传给 `winmaint`；用于 `compute` 时拒绝）。 |
| `vm_cpus` | 整数 | vCPU 核数（`custom` 默认 `4`；传给 `winmaint`；用于 `compute` 时拒绝）。 |
| `vm_iso` | 绝对路径 | 安装光盘镜像（`cd0`），必须是普通文件（不能是目录或设备）；传给 `winmaint`；用于 `compute` 时拒绝。默认优先引导（`bootindex=1`），但由 `locachy-winiso` 提供的 Windows 镜像只在磁盘为空时优先引导。Windows 客户机未设置时，若有已核对的镜像，`locachy-winiso path windows` 会提供它（见 [WINISO.zh.md](WINISO.zh.md)）。 |
| `vm_base` | 绝对路径 | `custom` 的只读基础镜像（`raw` 或 `qcow2`），首次启动时自动创建写时复制叠加层。 |
| `vm_disk` | 绝对路径 | `custom` 直接读写使用的磁盘镜像（`raw` 或 `qcow2`）。 |
| `vm_uefi` | `"0"`、`"1"` | 启用 OVMF UEFI 固件（仅 `custom`；`vm_os="windows"` 默认 `"1"`，`linux` 默认 `"0"`）。 |
| `vm_tpm` | `"0"`、`"1"` | 通过 `swtpm` 模拟 TPM 2.0（仅 `custom`；`windows` 且已安装 `swtpm` 时默认 `"1"`，否则 `"0"`）。 |
| `vm_share_home` | `"0"`、`"1"` | 通过 `virtiofs` 将 uid 1000 的整个主目录共享给客户机，挂载标签 `home`（仅 `custom`；默认 `"0"`）。 |
| `vm_virtio_iso` | 绝对路径 | 第二张只读光盘镜像（`cd1`），例如 `virtio-win.iso`（`custom`），必须是普通文件；在 `winmaint` 下则传给 `--virtio-iso` 用于整理驱动。Windows 客户机未设置时，若已安装软件包，会取 `locachy-winiso path virtio-win` 提供的镜像。有 virtio-win 时，驱动（`viostor`、`NetKVM`）还会整理进 `drivers.img` 的 `$WinPEDriver$` 目录，并以 `usb-storage,drive=drv0,bus=xhci.0,removable=on` 挂载；标记为可移动设备后，Windows 安装程序会自动搜索其中的 `$WinPEDriver$` 并加载驱动。 |
| `vm_os` | `linux`、`windows` | 客户机系统档位（仅 `custom`；默认 `linux`），决定 CPU 标志和默认设备。 |
| `vm_nic` | `virtio`、`e1000e` | `network="1"` 时的网卡型号（`custom`：`linux` 默认 `virtio`；`windows` 在由安装程序配合驱动种子装入的空白磁盘上为 `virtio`，其余情况为 `e1000e`；`winmaint` 默认 `e1000e`）。Windows 的默认值一经选定即写入配置。 |
| `vm_gpu` | `2d`、`gl` | 显卡模式（仅 `custom`）：`2d`（`virtio-vga`、`gl=off`，默认）或 `gl`（`virtio-vga-gl`、`gl=on`，仅限 Linux 客户机）。 |
| `vm_disk_bus` | `virtio`、`nvme`、`sata` | 磁盘总线（`custom` 支持三者；`winmaint` 支持 `virtio` 与 `nvme`）。默认 `virtio`。`windows` 客户机：只有在由安装程序配合驱动种子装入的空白磁盘上才用 `virtio`；其余情况用 `nvme`，即 Windows 无需额外驱动即可启动的总线（已有数据的磁盘可能装着没有 `viostor` 的 Windows）。Windows 的默认值一经选定即写入配置。 |
| `vm_audio` | `"0"`、`"1"` | 通过 PulseAudio 挂载 `ich9-intel-hda` 与 `hda-duplex` 声卡（仅 `custom`；默认 `"0"`）；只有 `/run/pulse/restricted/native` 存在（即声音服务正在运行）时才允许开启。 |
| `network` | `"0"`、`"1"` | `"1"` 加入 `locachy-net`（`custom` 下为 `-nic user,model=<网卡型号>`）；未设置或 `"0"` 时仅提供回环网络命名空间，并传 `-nic none`。 |

### 各类型的键检查

- `vm_kind="winmaint"` 只接受 `vm_freeze`、`vm_display`、`vm_memory`、`vm_cpus`、`vm_iso`、`vm_nic`、`vm_disk_bus` 与 `vm_virtio_iso`（分别透传为 `--memory`、`--cpus`、`--iso`、`--nic`、`--disk-bus`、`--virtio-iso`）；`vm_name` 以及其他仅 `custom` 可用的键（`vm_base`、`vm_disk`、`vm_uefi`、`vm_tpm`、`vm_share_home`、`vm_os`、`vm_gpu`、`vm_audio`）都会被拒绝。
- `vm_kind="compute"` 只接受 `vm_name`、`vm_freeze` 与 `vm_display`；`vm_memory`、`vm_cpus`、`vm_iso` 以及所有仅 `custom` 可用的键都会被拒绝（计算型虚拟机保留自身的硬件定义）。
- `vm_kind="custom"`（或省略 `vm_kind`）时，`vm_name` 会被拒绝。

### 网络策略

配置 `network="1"` 时，`locachy-app` 通过 `nsenter --net` 进入 `/run/netns/locachy-net`，让虚拟机继承 LOCachy 的网络栈（含 Tor、Clash 与 Tailscale 规则）。如果 `/run/netns/locachy-net` 不存在，行为会收紧：回退到只有回环接口的私有网络命名空间，绝不暴露宿主机网络。在 `custom` 虚拟机上，`network="1"` 始终传入 `-nic user,model=<网卡型号>`；未设置或为 `"0"` 时传入 `-nic none`，并保留在只有回环接口的命名空间内。

### 声明式 `custom` 虚拟机与 `command=`

对于 `vm_kind="custom"`（或未填写 `vm_kind`）：

- 未设置 `command=` 时，必须在 `vm_base` 和 `vm_disk` 中**二选一**。同时填写或两者皆空都会被拒绝。
- `command=`（可配 `arguments=`）是唯一的自定义入口，用于指定独立的启动程序（不是直接传给 QEMU 的参数）。`command=` 与 `vm_base` 或 `vm_disk` 同时配置会被拒绝。
- `vm_base`、`vm_disk`、`vm_iso` 与 `vm_virtio_iso` 的路径必须是绝对路径，不能包含 `.` 或 `..` 路径段，不能位于 `/var/lib/locachy/vms` 之下（该目录会在 `remove` 时删除），且文件必须已经存在。`vm_iso` 与 `vm_virtio_iso` 必须是普通文件（不能是目录或设备），即使转交给 `winmaint` 也是如此。
- LOCachy 绝不对用户提供的镜像执行 `chmod` 或 `chown`。`vm_base`、`vm_iso` 与 `vm_virtio_iso` 需对 uid 1000 可读，`vm_disk` 需对 uid 1000 可读可写。
- KVM 根据 `/dev/kvm` 自动检测（可读可写时启用，否则回退到 TCG 软件模拟），磁盘 I/O 引擎为 `aio=io_uring`。

## 后端类型

- **`winmaint`**：执行 `locachy-winmaint vm --display <display> --qmp-sock <sock>`（按需附带 `--memory`、`--cpus`、`--iso`、`--nic`、`--disk-bus`、`--virtio-iso`）。沿用兼容裸机双启动 `stornvme.sys` 的 NVMe 虚拟盘封装与四层 UEFI 引导保护，详见 [WINMAINT.zh.md](WINMAINT.zh.md)。
- **`compute`**：在 `app_NAME` 内以前台方式运行 `locachy-compute vm run <vm_name> --display <display> --qmp-sock <sock>`，详见 [COMPUTE.zh.md](COMPUTE.zh.md)。
- **`custom`**：根据 `vm_base`/`vm_disk` 与各项 `vm_*` 键声明式拼装 QEMU/KVM 启动命令，或执行显式配置的 `command=`/`arguments=`。

## 远程 NVIDIA 实验管理

`vm_kind="compute"` 虚拟机是需要独立内核时才使用的可选后端，不是普通 NVIDIA 实验的必要条件。首选管理路径仍是普通 LOCachy 用户通过 **Tailscale 上的 OpenSSH** 访问：用 `ssh` 执行 `locachy-compute`，用 `scp`/`sftp` 传入和取回文件。可选的 HTTPS/mTLS API 不是这条路径的前置条件；Thunderbolt 是独立的可选异构链路（见 [COMPUTE.zh.md](COMPUTE.zh.md)）。

SSH 服务只能监听节点的 tailnet 地址，并须对 uid 1000 用户使用公钥认证；不要允许 root 登录、SSH 密码登录，也不要用 Tailscale SSH 代替认证。SSH 用户密钥、SSH 主机密钥、tailnet 身份、可选的计算 mTLS 凭据和显示 PSK 是不同身份，必须分别信任和撤销。现有的密码认证 `sudo` 仍是特权边界；SSH 密钥不会带来免密码 sudo。接受服务器加入 `known_hosts` 前，先通过可信渠道核对主机密钥指纹。

`service_tailnet_ssh` 首次启动时，如果节点还没有 Ed25519 主机密钥，就会生成并保存一份。管理员按 [COMPUTE.zh.md](COMPUTE.zh.md) 将操作人员的公钥登记到 `/etc/locachy/tailnet-ssh/authorized_keys/<用户>`。

模型权重、数据集、检查点、日志和结果应放在已挂载的 `compute` 或 `shared` 存储卷中，并位于受管虚拟机状态目录之外。复制文件前用 `findmnt` 确认挂载，只绑定计算环境或虚拟机所需的路径；`scp`/`sftp` 不会验证底层磁盘。分离运行的原生容器作业必须在独立的计算 cgroup 中运行，能在 SSH 注销后继续，保留标准输出/错误和终态退出码，并在取消后不残留工作负载进程。没有退出码的记录应视为未核实；完整流程见 [COMPUTE.zh.md](COMPUTE.zh.md)。虚拟机本身仍由 `locachy-app` 管理，不是 SSH 分离作业。

## 权限模型

- 在真实宿主机 root 环境下，`compute` 和 `custom` 虚拟机不会以 root 运行。QEMU 以及辅助进程（`qemu-img`、`swtpm`、`virtiofsd`）均通过 `setpriv` 降权到 uid 1000，附带 `kvm` 附加组（若存在）并开启 `no_new_privs`。`locachy-app` 只将共享 Wayland 套接字（`locachy-0`）和受限音频目录绑定挂载到一个归 uid 1000 所有的私有 tmpfs 运行时目录，使 QEMU 无需访问 root 独占（`0700`）的 `/run/user/0`。
- 只有 `vm_kind="winmaint"` 保留 root 权限，因为 `locachy-winmaint vm` 需要 `dmsetup` 与 `losetup` 组装 `dm-linear` 虚拟盘。
- 在非特权测试命名空间中（调用方不是真实宿主机 root），`locachy-app` 不切换 uid。

## 状态目录与移除

每台虚拟机的受管状态存放在 `/var/lib/locachy/vms/NAME`：

- 父目录 `/var/lib/locachy/vms` 权限为 `0711`；`/var/lib/locachy/vms/NAME` 在真实宿主机 root 下为 uid 1000 所有、权限 `0700`。
- 创建状态目录时会尽力执行 `chattr +C`，在 Btrfs 上关闭写时复制（进而避免对虚拟机镜像做 Btrfs 校验和与压缩）；其他文件系统会静默忽略该调用。
- 目录内保存 `disk.qcow2`（首次启动由 `qemu-img create -f qcow2 -b <vm_base>` 生成的叠加盘，`vm_base` 本身始终只读）、`drivers.img`（`vm_os="windows"` 且具备 virtio-win 时生成的 FAT 驱动种子镜像，内含 `$WinPEDriver$`，以只读方式挂载；缺失时生成，virtio-win ISO 更新后重建）、`ovmf-vars.fd`（私有可写 OVMF 变量文件）、`swtpm/`（TPM 状态）以及运行时套接字（`qmp.sock`、`swtpm.sock`、`vfs-home.sock`）。

虚拟机仍在运行时，`locachy-app remove NAME` 会拒绝执行。停止后再移除会删除 `/etc/locachy/containers/NAME.conf`、`/run/locachy/apps/NAME*` 以及 `/var/lib/locachy/vms/NAME`，但绝不删除用户提供的 `vm_base` 或 `vm_disk` 文件，也不会删除维护用 Windows 分区。

## 辅助进程与生命周期（冻结、恢复、停止）

辅助守护进程（`swtpm`、`virtiofsd`）都在启动器加入 `/sys/fs/cgroup/app_NAME` **之后**拉起，与 QEMU 共处同一 cgroup，因此冻结和停止会一并覆盖它们。

- **冻结（`vm_freeze="qmp"`，默认）**：先同步剪贴板（若可用），再通过 QMP 发送 `{"execute":"stop"}` 在安全边界暂停所有 vCPU（保持客户机时钟与 virtio 队列一致），写入 `/run/locachy/apps/NAME/vm-paused`，随后经由合成器把关的剪贴板交接（`clipboard freeze`）冻结 cgroup。不存在未加保护的 `cgroup.freeze` 回退：若剪贴板交接不可用或被拒绝，`locachy-app` 会发送 QMP `cont`、清除 `vm-paused`，并使本次冻结失败。
- **冻结（`vm_freeze="none"`）**：直接返回 0，不暂停 vCPU，也不冻结 cgroup（适合安装系统或 Windows Update 期间保持后台运行）。
- **恢复**：取消内存回收，写 `cgroup.freeze=0` 并等待 `frozen 0`；若存在 `vm-paused` 标记，再发送 QMP `{"execute":"cont"}`。
- **停止**：先解冻 cgroup；若处于暂停状态则发送 QMP `cont`，使客户机能够响应 ACPI；接着发送 QMP `{"execute":"system_powerdown"}` 并等待最多约 8 秒；若仍未退出，则向 PID 1 收割器发送 `SIGTERM` 并等待最多约 3 秒（让 `locachy-winmaint` 的 `finally:` 干净回收 `dm-linear`、`losetup` 和 `swtpm`）；只有进程仍然存在时才以 `cgroup.kill` 兜底。
- **单实例语义**：对运行中的虚拟机再次执行 `start`、`open`、`again` 或 `new` 不会对同一块磁盘启动第二个 QEMU 进程；若已冻结则直接恢复。
- **辅助进程启动失败**：退出时的清理在启动第一个辅助进程之前就已设好。某个辅助进程启动失败或超时时，已启动的辅助进程会被停止，虚拟机不会启动，也不会生成就绪标记，Home 因此不会显示一个没有窗口的“运行中”应用。

## 共享主目录（`vm_share_home="1"`）

开启 `vm_share_home="1"` 后，`locachy-app` 会查找 `virtiofsd`（顺序：`/usr/lib/virtiofsd`、`/usr/libexec/virtiofsd`、`PATH`；测试时可用 `LOCACHY_VIRTIOFSD` 指定），以 `--sandbox=namespace --cache=auto` 共享 uid 1000 的**整个主目录**，并在 QEMU 侧配置共享 memfd 内存（`-object memory-backend-memfd,id=mem,size=...M,share=on`、`-machine ...,memory-backend=mem`）与 `-device vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=home`。

客户机内部不会自动挂载，需手动配置：

- **Linux 客户机**：
  ```sh
  mount -t virtiofs home /mnt/home
  ```
- **Windows 客户机**：需要用户在 Windows 内自行安装 `virtio-win`（`vm_virtio_iso`）中的 `viofs` 驱动、安装 WinFsp，并启用/启动 `VirtioFsSvc` 服务。

## 窗口模型

当前首版中，QEMU 的 GTK（或 SDL）窗口**就是**该应用的窗口，窗口内显示什么完全由客户机系统决定。本批次不含 guest agent，也不做无缝单应用窗口转发。如果希望打开虚拟机就直接呈现单个全屏程序，需由用户在基础镜像内自行配置自动登录与开机自启。

## 窗口归属

Home 中的运行状态，以及 `labwc-control` 的切换、关闭、焦点跟踪和窗口列表，靠 QEMU 的窗口标题和 Wayland `app_id` 找到虚拟机的窗口。所有 QEMU 窗口的 `app_id` 都是 `qemu`，因此标题里要带上启动器通过 `-name` 给虚拟机设的标签：

- `custom`：`locachy-vm[NAME]`。GTK 标题为 `QEMU (locachy-vm[NAME])`，QEMU 需要时会在后面加上 ` [Paused]` 或 `: 标签`；SDL 标题为 `QEMU (locachy-vm[NAME]-N)`。
- `compute`：`locachy-compute[VMNAME]`（`vm_name`），由 `locachy-compute` 设定。
- `winmaint`：`locachy-winmaint`。

窗口标题带有某台虚拟机的标签、且 `app_id` 以 `qemu` 开头时，才算属于这台虚拟机。方括号保证标签不会混淆：应用名里不可能有方括号，所以 `debian` 不会认领 `debian-12` 的窗口（其标签为 `locachy-vm[debian-12]`）。标题带有虚拟机标签的窗口，不会凭 `app_id` 被算到普通容器或 `command=` 虚拟机名下；除此之外，`command=` 虚拟机仍按其命令的 `app_id` 匹配。

## 关闭窗口

关闭虚拟机的窗口（窗口自己的关闭按钮、任务栏或概览）不会停止虚拟机。显示参数带有 `window-close=off`，窗口关掉后 QEMU 照常运行。要停止虚拟机，请在 Home 中停止，或在客户机系统里正常关机。

## Windows 客户机

设置 `vm_os="windows"` 时会自动启用：

- KVM CPU 标志：`host,hv-relaxed,hv-vapic,hv-spinlocks=0x1fff,hv-time,hv-vpindex,hv-synic,hv-stimer,hv-stimer-direct,hv-reset,hv-frequencies,hv-tlbflush,hv-ipi`（与 `winmaint` 一致；Linux 客户机使用普通 `host`；TCG 回退模式使用 `max`）。
- 默认设备：空白磁盘、且安装程序能从驱动种子盘自行加载驱动（有 virtio-win，来自 `vm_virtio_iso` 或 `locachy-winiso`）时，默认 `vm_disk_bus="virtio"`（带独立 IO 线程与 io_uring）和 `vm_nic="virtio"`。其他 Windows 磁盘默认 `vm_disk_bus="nvme"` 与 `vm_nic="e1000e"`，Windows 不装额外驱动也能启动和联网。选定的值会写入配置。另外默认 `vm_uefi="1"`（带 `smm=on`），已安装 `swtpm` 时默认 `vm_tpm="1"`。
- 安装镜像：`vm_iso`，或用 `locachy-winiso fetch windows` 从微软下载，并按微软公布的 SHA-256 核对（见 [WINISO.zh.md](WINISO.zh.md)）。磁盘为空时安装镜像优先引导；Windows 装好之后，镜像仍挂着，但不再优先引导。

安装 Windows 时的注意事项：

- 可用 `vm_virtio_iso="/path/to/virtio-win.iso"` 挂载 `virtio-win` 光盘，在客户机中安装 `viostor`、`NetKVM`、`viogpudo` 和 `viofs` 驱动。
- 当具备 `virtio-win`（来自 `vm_virtio_iso` 或 `locachy-winiso`）时，`locachy-app` 会挂载 `drivers.img`（`usb-storage,drive=drv0,bus=xhci.0,removable=on`），Windows 安装程序会自动加载 `viostor` 与 `NetKVM`，无需手动选驱动。对于已装有 Windows 但不含 `viostor` 的磁盘，应保持 `vm_disk_bus="nvme"` 或 `vm_disk_bus="sata"`（两者无需额外存储驱动即可启动）以及 `vm_nic="e1000e"`，待在客户机内安装 `viostor` 与 `NetKVM` 后再切为 `vm_disk_bus="virtio"` / `vm_nic="virtio"`。若缺少 `virtio-win` 或无法生成种子镜像，使用 `vm_disk_bus="virtio"` 则需在安装界面手动加载 `viostor`。
- `vm_os="windows"` 下禁止使用 `vm_gpu="gl"`（`virtio-vga-gl` / virgl）。
- 裸机双启动维护用 Windows（`vm_kind="winmaint"`）的设计与防护机制见 [WINMAINT.zh.md](WINMAINT.zh.md)。

## 配置示例

基于只读基础镜像的 Linux 桌面虚拟机：

```conf
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="/home/user/VMs/debian-base.qcow2"
vm_memory="4096"
vm_cpus="4"
vm_uefi="1"
vm_share_home="1"
network="1"
```

挂载 `virtio-win` 驱动光盘的 Windows 虚拟机：

```conf
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="/home/user/VMs/win11.qcow2"
vm_virtio_iso="/home/user/Downloads/virtio-win.iso"
vm_memory="8192"
vm_cpus="4"
vm_nic="e1000e"
network="1"
```

## 测试接口

集成测试中，`locachy-app` 会检查一组固定的仅限 root 使用的覆盖环境变量（每个都必须为绝对路径，且要求 uid 0）。该列表中的十五项为八个通用覆盖项（`LOCACHY_CGROUP_BASE`、`LOCACHY_CONT_BASE`、`LOCACHY_CONT_CONF`、`LOCACHY_APP_RUNTIME`、`LOCACHY_APP_INIT`、`LOCACHY_CLIPBOARD_TOOL`、`LOCACHY_TOR_ENFORCE`、`LOCACHY_RC_CONF`）与以下七个虚拟机专用覆盖变量：

- `LOCACHY_WINMAINT_TOOL`：覆盖 `/usr/local/bin/locachy-winmaint`
- `LOCACHY_COMPUTE_TOOL`：覆盖 `/usr/local/bin/locachy-compute`
- `LOCACHY_VM_STATE`：重定向 `/var/lib/locachy/vms`
- `LOCACHY_QEMU_BIN`：覆盖 `qemu-system-x86_64`
- `LOCACHY_VIRTIOFSD`：覆盖 `virtiofsd`
- `LOCACHY_SWTPM`：覆盖 `swtpm`
- `LOCACHY_WINISO_TOOL`：覆盖 `/usr/local/bin/locachy-winiso`

## 测试覆盖范围与需在实机上确认的部分

自动化测试脚本为 `src/tools/test-app-vm.sh`。它在 Linux 上以普通用户身份运行，处于独立的用户与挂载命名空间中，用 cgroup 夹具文件、模拟 QMP 的替身代替 QEMU，`swtpm`、`virtiofsd` 和启动器也用替身。叠加盘用真实的 `qemu-img` 创建；`/dev/kvm` 可用时，还会让真实的 `qemu-system-x86_64` 接受实际参数并通过 QMP 控制它。覆盖内容：拒绝未知键和用错类型的键、路径与 ISO 检查、Linux、Windows、SDL 与 GTK 虚拟机的启动参数、窗口标签与 `window-close=off`、冻结/恢复/停止的顺序、辅助进程启动失败、Windows 客户机在有无 virtio-win 时的默认值（写入配置，显式设置优先），以及由 `locachy-winiso` 提供的镜像。

只能在实机上确认的部分：`labwc` 中的 QEMU GTK 窗口本身（包括关闭窗口的效果）、真实的 `virtiofsd` 守护进程与客户机挂载、真实的 `swtpm`、`vm_gpu="gl"`（virgl）、磁盘与网络吞吐、装上 virtio 驱动的 Windows 安装过程，以及真实宿主机 root 下降权到 uid 1000（测试以普通用户运行，降权只在参数拼装层面检查）。


