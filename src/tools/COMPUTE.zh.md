# LOCachy 计算节点

[English](COMPUTE.md)

对 NVIDIA 实验，首选 **通过 Tailscale 使用普通 OpenSSH**：用 `ssh` 在节点上执行 `locachy-compute`，用 `scp` 或 `sftp` 传输文件。NVIDIA 工作负载使用原生 Linux 容器；需要独立内核时再选 KVM 微型虚拟机。无界面计算节点不依赖图形会话，也不会随桌面冻结/停止。带双向 TLS 的 HTTPS（mTLS）只是可选 API。Thunderbolt/USB4 是独立的可选主机间链路，不是 NVIDIA 实验的前置条件。

## 通过 SSH 进行 NVIDIA 实验

### 接入与信任

1. 让节点和客户端加入预期的 tailnet。在 [网络访问策略](https://tailscale.com/kb/1324/grants)
   中只允许指定操作者或设备访问节点的 `tcp:22`，并检查是否有更宽泛的规则也放行了这个端口。
   节点不要启用 Tailscale SSH：这里用的是普通 OpenSSH，tailnet 的 `ssh` 规则不能代替用户公钥登记。
2. 在设置中进入 **网络 → SSH 服务器（sshd）**，选择 **仅 Tailscale**（需要局域网访问时选 **局域网与 Tailscale**）。可选的 `service_tailnet_ssh` 在 `locachy-net` 中、独立 cgroup（`locachy-tailnet-ssh`）里运行 OpenSSH，跟随 `tailscale0` 的地址（`lan` 模式下还包括活动的非 `lo`、非 `mihomo`、非 `thunderbolt-net` 接口的全局地址）。按接口划分的 nftables 规则会丢弃从其他接口（包括回环接口）到达的流量；`lan` 模式下，`locachy-filter` 只放行精确绑定的 `(接口, 地址, 端口)` 三元组。默认监听 22 端口（`ssh_port`），只接受公钥认证；密码登录可选（`ssh_password="1"`，默认关闭），root 登录和空密码始终禁止。本地 TCP 转发（`AllowTcpForwarding local`）开放，用于 `ProxyJump` 和 `-L` 隧道；远程转发（`-R`）、代理转发、X11、隧道和用户环境保持禁用，内置 SFTP（当前 `scp` 也使用它）启用。
3. 操作者的**公钥**可以在设置（**网络 → SSH 服务器（sshd）→ sshd 授权公钥**）中登记，也可以由管理员在节点控制台上登记（将 `USER` 和源文件名替换为实际值）：

   ```sh
   sudo /usr/local/lib/locachy/tailnet-ssh-client --keys-add USER /path/to/operator.pub
   # 或从标准输入传入：
   sudo /usr/local/lib/locachy/tailnet-ssh-client --keys-add USER < /path/to/operator.pub
   # 查看暂存在 ~/.ssh/authorized_keys 中（例如通过 ssh-copy-id 写入）的候选公钥，不自动信任：
   /usr/local/lib/locachy/tailnet-ssh-client --keys-pending USER
   # 开启一次性临时接收地址，供对端用 curl -T ~/.ssh/id_ed25519.pub 发送：
   sudo /usr/local/lib/locachy/tailnet-ssh-client --receive-key USER --timeout 120
   # 或直接整体替换文件：
   sudo install -o root -g root -m 0644 /path/to/operator.pub \
     /etc/locachy/tailnet-ssh/authorized_keys/USER
   ```

   只有 root 能写入 `/etc/locachy/tailnet-ssh/authorized_keys/USER`；非特权的公钥查询需要读取它。删除密钥可以在设置中进行，也可以使用 `--keys-remove USER 序号`（或删除文件），之后即禁止登录。`sshd` 始终保持 `AuthorizedKeysFile none`，绝不自动信任用户的 `~/.ssh/authorized_keys`：`--keys-pending` 只以安全方式读取候选公钥供操作者显式批准，`--receive-key` 仅在当前监听地址上绑定临时端口（绝不绑定 `0.0.0.0`/`::`，由降权为 `nobody` 的短生命周期子进程接收，`lan` 模式下使用专用的 `ssh_enroll_v4`/`ssh_enroll_v6` nftables 集合），收到后只输出公钥与 SHA-256 指纹等待人工确认，不会自动写入 `authorized_keys`。私钥始终留在客户端。
4. 服务首次启动时会生成持久的 Ed25519 主机密钥；已有密钥不会被替换，除非你在设置（**sshd 主机密钥**）中或使用 `--host-key-regen` 明确重新生成（此时客户端需删除 `known_hosts` 中的旧条目）。在客户端接受前，先核对 SHA-256 指纹：

   ```sh
   sudo /usr/local/lib/locachy/tailnet-ssh-client --host-key-fingerprint
   ```

   tailnet 访问权限、SSH 用户公钥、SSH 主机密钥、mTLS 凭据和扩展显示 PSK 彼此独立。
5. 使用普通 LOCachy 用户（uid 1000）登录，不要使用 root。文件传输遵循该用户的普通权限。
   需要特权的计算操作使用现有的、以该用户密码认证的 `sudo`；SSH 密钥不会带来免密码 `sudo`。
   示例在执行远程 `sudo` 命令时使用交互式 SSH shell（需要时加 `ssh -t`）。不要把 sudo 密码
   放入脚本或命令行参数。

主机密钥在运行中的节点上生成，不随源码或 ISO 分发。

以下示例中的 `locachy-gpu` 是客户端 SSH 配置中的别名，指向节点的 tailnet IP 或 MagicDNS 名称，并使用普通用户。这个工作流不需要 `service_compute`、`locachy-pair` 或 Thunderbolt 线缆。

LOCachy 的交互式程序（`settings`、`packages`、`firmware`、`files`，以及构建时包含的 `edit` 和 `view`）和 `reboot`、`poweroff` 命令，即使本机物理屏幕正处于扩展显示（`extdisplay`）会话中，也可以直接通过 SSH 运行（见 [SSH-TUI.zh.md](SSH-TUI.zh.md)）。

### 设置界面入口（**计算与虚拟机 → 计算节点**）

「设置」在 **计算与虚拟机 → 计算节点** 下提供 6 行直接管理入口，由 `locachy-compute status --tsv` 驱动（即使 `compute_storage` 当前未挂载也不会失败）：

1. **Compute HTTPS API (optional)**：开关 `service_compute`，并实时显示端口与守护进程状态（`running` / `starting`）。
2. **Compute API over Thunderbolt**：开关 `compute_bind_tb` 并同步雷电放行规则（`tb-sync`）。
3. **Compute GPU passthrough**：开关 `compute_gpu`，并显示检测到的宿主机 NVIDIA 驱动版本与 GPU 设备数。
4. **Compute storage volume**：按 `Left`/`Right` 切换 `compute_storage` 存储卷，直接显示 `[MOUNTED]` 剩余空间或 `[UNMOUNTED]` 状态；按 `m` 可直接挂载或卸载当前卷。
5. **计算节点管理（Compute Node Manager）**：按 `Enter` 打开单页分组管理界面（无标签页，依次为**默认工作容器**、**容器**、**镜像**、**微型虚拟机**、**任务**、**存储与访问**），顶部独立状态区展示 API、存储挂载与空间、GPU/CUDA 状态、默认容器与资源计数。默认工作容器（`compute_default_env`）置顶：该行按 `Left`/`Right` 可在多个容器间切换（多于一个容器时），**容器**列表中按 `p` 可将某容器设为默认；该行可进入终端（`Enter`）、打开容器配置表单（`e`）、运行容器内 `nvidia-smi` GPU 测试（`t`），并可重置（连按两次 `x`，丢弃容器的可写改动，保留镜像与存储卷）。页面还包括内置原生基础镜像下载与导入界面（Ubuntu 24.04/22.04、Arch Linux、Alpine 官方源带 SHA-256 校验，或自定义 HTTPS 链接与本地归档）、完整的容器配置表单（创建与在线修改 GPU、联网 `locachy-net`/`none`、内存/CPU 限额、工作目录、存储卷勾选与宿主目录挂载）、微型虚拟机创建、启动、进入、停止、重置、删除与导出到主屏幕（`a`）、任务日志查看与停止、存储卷挂载/卸载、客户端 SSH 配置查看，以及客户端配对（`locachy-pair`）。
6. **Client SSH config**：按 `Enter` 打开可滚动的 `locachy-compute ssh-config` 预览（即使存储卷未挂载也会给出基础主机配置段），按 `c` 一键复制到剪贴板。

### 准备存储、运行并取回结果

使用有足够空间的 `compute` 或 `shared` 卷，容纳镜像、叠加层、数据集、模型权重、检查点、结果和日志。本例卷名为 `experiments`。放入文件前要确认卷确实已挂载；`scp` 和 `sftp` 本身不会检查路径是否位于预期磁盘。SSH 用户的暂存目录应位于 `locachy-compute/` 之外（那里保存受管状态），并且只绑定工作负载真正需要的目录。

从节点上的 SSH shell 执行 `findmnt --mountpoint /srv/storage/experiments`。如果输出没有显示预期的已挂载卷就停止。然后配置存储，并准备由 SSH 用户写入的目录：

```sh
sudo locachy-storage set-compute-storage experiments
sudo locachy-compute storage
sudo install -d -m 0700 -o "$(id -u)" -g "$(id -g)" \
  /srv/storage/experiments/work \
  /srv/storage/experiments/work/probe-001/in \
  /srv/storage/experiments/work/probe-001/out
```

本例使用包含 Python 和 CUDA 版 PyTorch、且与节点 NVIDIA 驱动兼容的可信 Linux rootfs 归档。在客户端保存下面的 `probe.py`：

```python
from pathlib import Path

import torch

x = torch.arange(1, 5, dtype=torch.float32, device="cuda").reshape(2, 2)
y = x @ x
torch.cuda.synchronize()
result = y.cpu().tolist()
assert y.is_cuda and result == [[7.0, 10.0], [15.0, 22.0]]
text = f"{torch.cuda.get_device_name(y.device)}: {result}\n"
print(text, end="")
output = Path("/output/probe.txt")
output.write_text(text, encoding="utf-8")
output.chmod(0o644)
```

```sh
scp probe.py locachy-gpu:/srv/storage/experiments/work/probe-001/in/
scp torch-rootfs.tar.zst locachy-gpu:/srv/storage/experiments/work/
ssh locachy-gpu
```

在节点上导入 rootfs 并创建环境。归档先放在同一个受管卷上，因此导入不会悄悄读取另一块磁盘：

```sh
sudo locachy-compute image import torch-rootfs /srv/storage/experiments/work/torch-rootfs.tar.zst
sudo locachy-compute env create torch-probe --image torch-rootfs --gpu --net none \
  --mount /srv/storage/experiments/work/probe-001/in:/input:ro \
  --mount /srv/storage/experiments/work/probe-001/out:/output:rw
```

由于本例预先传入全部输入，`--net none` 已足够。在节点上提交作业并保存返回的 `job_id`。每次运行使用新的作业 ID 和输出目录，避免把旧日志或结果误认成新结果。提交后可以断开连接，再连回去检查：

```sh
sudo locachy-compute run torch-probe --job-id probe-001 --detach -- python3 /input/probe.py
sudo locachy-compute job list
sudo locachy-compute job status probe-001
sudo locachy-compute job log probe-001
```

取消运行中的作业时使用 `sudo locachy-compute job stop JOB_ID`，然后检查状态。分离作业必须属于独立的计算 cgroup，能在 SSH 注销后继续运行，而不是随登录会话消失。提交响应不是工作负载结果：正常完成或失败时，必须有记录的终态和工作负载退出码，保存标准输出/错误，并且取消后不能残留工作负载进程。只有 `exited` 而没有退出码的记录不算通过。宿主机文件权限必须允许 SSH 用户取回结果，不能依赖特权文件传输；本例把可读结果写入该用户的私有输出目录。

作业成功后，在客户端取回结果（也可用 `sftp`）：

```sh
scp locachy-gpu:/srv/storage/experiments/work/probe-001/out/probe.txt .
```

## 组成

| 文件 | 作用 |
|---|---|
| `link/locachy-compute` | 命令行工具和守护进程：镜像、环境、原生容器、作业、KVM 微型虚拟机、`serve`，以及作为客户端的 `remote`。 |
| `link/tailnet-ssh-client` | 受管 OpenSSH 服务（`sshd`），运行在 `locachy-net` 中：仅 Tailscale 或局域网与 Tailscale、可配置端口、可选密码认证、受管 `authorized_keys`（支持 `.pub` 文件导入、`--keys-pending` 候选审核与一次性 `--receive-key` 接收），以及主机密钥生命周期。 |
| `link/compute-client` | 可选的 `service_compute` 启动脚本。用 `nsenter --net` 进入 `locachy-net`，再运行 `locachy-compute serve`。 |
| `link/locachy-pair` | 一次配对、永久信任：添加、列出、查看、撤销已配对设备，并导出令牌。 |
| `link/locachy_peers.py` | 共享的对端存储（`/etc/locachy/peers`），由 `locachy-pair`、`locachy-compute` 和 `extdisplay-broker` 共用。 |
| `link/network-client` | `locachy-net` 的入站过滤规则，包括雷电（Thunderbolt）允许名单（`tb_ifaces`、`tb_tcp_ports`、`tb_udp_ports`）以及局域网 SSH 与密钥接收集合（`ssh_lan_v4`、`ssh_lan_v6`、`ssh_enroll_v4`、`ssh_enroll_v6`）。 |
| `progs/extdisplay-broker*.c`、`progs/extdisplay-advertise.c` | 扩展显示代理：持久对端 PSK、仅已配对模式、共享链路模式。 |

## 原生容器

执行 `locachy-compute run 环境名 -- 命令` 时，作业按以下步骤启动：

1. 基础镜像是只读目录（`/var/lib/locachy/compute/images/名称/rootfs`）。它可以由目录导入，也可以由 `.tar`、`.tar.gz`、`.tar.xz`、`.tar.zst` 归档，或 Docker/OCI 保存的归档导入。运行时不会写入它。
2. 环境（`envs/名称/`）保存可写的 OverlayFS 层 `upper/` 和 `work/`。
3. `unshare --mount --pid --fork --kill-child` 创建命名空间，并以镜像作为 `lowerdir` 挂载覆盖文件系统。
4. 挂载 `/proc`、只读的 `/sys`、精简的 `/dev`，以及作为 tmpfs 的 `/tmp` 和 `/run`。配置好的主机路径以绑定挂载方式接入。
5. 进程在自身内部执行 `pivot_root`，再用 `umount2` 分离旧根。整个过程不调用客人系统里的程序，所以客人镜像不需要 `umount`、`rmdir`，也不需要与之匹配的 C 库。
6. 如果存在静态的 `/usr/local/lib/locachy/app-init`，它作为 1 号进程，负责回收子进程并转发信号；否则由内置的回收逻辑完成同样的工作，且不依赖客人系统的任何文件。

容器中的 Wayland、X11、DBus 和席位变量全部清除，相应的套接字也从不挂载。

## 交互式 shell 与自动 ProxyJump SSH

要像使用普通机器一样进入容器或微型虚拟机：

- `sudo locachy-compute shell 名称 [-- 命令...]`：对计算容器（`--kind env`）直接附加交互式 TTY；对运行中的微型虚拟机（`--kind vm`）通过其回环 SSH 端口连接。`--console` 则附加到 QEMU 串口套接字，按 `Ctrl+]` 脱离。
- 在 `--net user` 下，`vm create` 会自动分配仅绑定回环的 SSH 转发（`hostfwd=tcp:127.0.0.1:<端口>-:22`，在 `2222..2999` 中自动选取，或使用 `--ssh-port` 指定，`--ssh-port 0` 则禁用；可选 `--ssh-user`）。它从不绑定 `0.0.0.0`。
- `locachy-compute ssh-config [--host IP] [--user 用户] [--port 端口] [--prefix 前缀] [--no-sudo] [--json]` 输出可直接粘贴到客户端 `~/.ssh/config` 的配置片段：包括跳板主机条目、计算容器的 `RemoteCommand sudo locachy-compute shell 环境名` 条目，以及带 SSH 转发的虚拟机的 `ProxyJump` 条目。

## 重置与数据

- `locachy-compute env reset 名称` 会停止该环境的作业，并删除 `upper/` 和 `work/`。基础镜像和所有已挂载的数据集保持不变，因此重置在毫秒内完成。
- 主机数据用 `--mount 主机路径:容器路径[:ro|:rw]` 挂载，或在创建环境时用 `env create --mount` 写入配置。允许任何已存在的绝对路径，例如 `/mnt/nvme/...`、`/data/...`、角色为 `shared`/`compute` 的 `/srv/storage/<卷名>` 或 `<计算根目录>/data/...`。以下位置禁止挂载：`/`、`/proc`、`/sys`、`/dev`、`/srv/storage` 根目录及其下仅限桌面的 `data` 卷、对端存储、计算根目录（其 `data/` 子目录除外）、`/run/locachy/compute`，以及扩展显示代理的状态目录。
- 由 `locachy-storage` 管理的多盘存储卷（见 `tools/STORAGE.zh.md`）可在 `env create` 或 `run` 中按卷名绑定：`--volume 卷名[:容器路径[:ro|rw]]`（默认容器路径为 `/volumes/卷名`，默认 `rw`）。绑定时按 `/etc/locachy/volumes.json` 校验，仅允许角色为 `shared` 或 `compute` 且已挂载在 `/srv/storage/卷名` 的卷，仅桌面使用的 `data` 卷会被拒绝。
- 在 `/etc/locachy/rc.conf` 中设置 `compute_storage="卷名"`（通过 `locachy-storage set-compute-storage 卷名`）后，除非显式设置了 `LOCACHY_COMPUTE_BASE`，`images/`、`envs/`、`jobs/`、`vms/` 与 `data/` 会使用 `/srv/storage/卷名/locachy-compute`，而不是 `/var/lib/locachy/compute`。该设置不会迁移已有文件。如果所配置的卷未挂载，`locachy-compute` 直接报错中止，绝不写入根分区。`locachy-compute storage [--json]` 与可选 API 的 `GET /v1/health`（`storage_volume`、`compute_base` 字段）会报告当前使用的存储卷。
- 使用可选 API 的对端无法指定挂载或存储卷。通过 SSH 直接调用特权 CLI 的操作者使用与节点本地操作者相同的挂载控制。

## GPU

创建环境时加上 `--gpu`（且 `compute_gpu` 不为 `"0"`），若主机已加载 `nvidia` 内核模块，`locachy-compute` 会先确保主机上的 NVIDIA 字符设备节点（`/dev/nvidiactl`、`/dev/nvidia-modeset`、`/dev/nvidiaN`、`/dev/nvidia-uvm`、`/dev/nvidia-uvm-tools`）已就绪，再将这些节点、`/dev/nvidia-caps/*` 和 `/dev/dri/renderD*` 绑定进容器。主机 NVIDIA 用户态库（`libcuda.so*`、`libnvidia-*.so*`、`libnvcuvid.so*`、`libnvoptix.so*`、`libcudadebugger.so*`）只读绑定在 `/usr/local/lib/locachy-gpu` 的独立 tmpfs 上（写入 `/etc/ld.so.conf.d/locachy-gpu.conf`、设置 `LD_LIBRARY_PATH` 并在容器启动时刷新容器内 `ldconfig`），主机 `nvidia-smi` 也只读暴露到 `/usr/local/bin/nvidia-smi`。`locachy-compute gpu-probe` 列出主机当前已存在的设备。不存在的设备会被跳过。

## KVM 微型虚拟机

`locachy-compute vm create 名称 --disk 基础.qcow2 [--share 目录:标签[:ro|rw]] [--volume 卷名[:标签[:ro|rw]]] [--cloud-init 种子.img]` 会创建写时复制的覆盖层（`qemu-img create -b 基础 -F qcow2`）。`vm start` 运行 `qemu-system-x86_64`：若存在 `/dev/kvm`，加上 `-enable-kvm`，否则使用 TCG。它提供 `virtio-blk-pci` 磁盘、串口控制台套接字、监视器套接字、QMP 套接字，以及可选的 cloud-init。`vm reset` 丢弃覆盖层，恢复干净状态。每台虚拟机运行在自己的 cgroup 中：`/sys/fs/cgroup/locachy-compute/vm-名称`。

基础镜像继续是只读 qcow2，每台虚拟机保留一份写时复制覆盖层，通过独立 iothread 挂到 `virtio-blk-pci`（计算虚拟机不使用模拟 NVMe）：

```
-object iothread,id=iothread0
-drive file=/var/lib/locachy/compute/vms/名称/overlay.qcow2,format=qcow2,if=none,id=hd0,cache=none,aio=io_uring,discard=unmap
-device virtio-blk-pci,drive=hd0,iothread=iothread0
```

在禁用 io_uring 的主机上（例如 `kernel.io_uring_disabled`），可设 `LOCACHY_COMPUTE_AIO=threads`；默认使用 `io_uring`。

目录共享（`--share`）和存储卷（`--volume`，见 `tools/STORAGE.zh.md`）默认走 **virtiofs**。每个共享由 `locachy-compute` 以虚拟机自身的 uid 启动一个 `virtiofsd` 进程，QEMU 客户机内存改为共享 memfd。对于包含一个只读目录共享与一个存储卷的虚拟机，QEMU 参数为：

```
-object memory-backend-memfd,id=mem,size=1024M,share=on
-machine q35,accel=kvm,memory-backend=mem
-chardev socket,id=vfs0,path=/run/locachy/compute/vm-名称/vfs-0.sock
-device vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=datasets
-chardev socket,id=vfs1,path=/run/locachy/compute/vm-名称/vfs-1.sock
-device vhost-user-fs-pci,queue-size=1024,chardev=vfs1,tag=vol-shared
```

对应 `virtiofsd` 先于 QEMU 启动，且套接字就绪后才拉起 QEMU：

```
virtiofsd --socket-path=/run/locachy/compute/vm-名称/vfs-0.sock --shared-dir=/data/dataset --sandbox=namespace --cache=auto --readonly
virtiofsd --socket-path=/run/locachy/compute/vm-名称/vfs-1.sock --shared-dir=/srv/storage/vol-shared --sandbox=namespace --cache=auto
```

仅对 `:ro` 共享加上 `--readonly`。virtiofs 是唯一的目录共享方式（不使用 9p）。

- **身份映射**：非特权用户下 `--sandbox=namespace` 会由 virtiofsd 自行建立用户命名空间并将调用者 uid 做一对一映射（见上游 `virtiofsd/src/sandbox.rs`），不依赖 `/etc/subuid`，也不传 `--translate-uid`。客户机看到属主为 uid 1000 的文件仍是 uid 1000，其余主机 uid 显示为 `nobody`，客户机 root 写入不做重映射。尚未在真实硬件上跑过真正的 virtiofsd。
- **生命周期**：`vm run` 和 `vm start` 先启守护进程再启 QEMU，QEMU 退出后再停止守护进程；启动中途失败会清理已启动的进程。`vm stop` 先停 QEMU 再停登记的守护进程，保证客户机写入先落盘。
- **环境变量**：`LOCACHY_VIRTIOFSD` 指定二进制（默认先查 `PATH` 中的 `virtiofsd`，再依次找 `/usr/lib/virtiofsd`、`/usr/libexec/virtiofsd`、`/usr/bin/virtiofsd`）；`LOCACHY_VIRTIOFSD_SANDBOX` 可取 `namespace`、`chroot`、`none`；`LOCACHY_COMPUTE_APP_UID`（默认 1000）决定 root 把虚拟机文件交接给哪个 uid；`LOCACHY_COMPUTE_KVM=0` 强制 TCG。

前台运行与导出为应用：

- `locachy-compute vm run 名称 [--display none|gtk|sdl] [--qmp-sock 路径]` 在前台运行 QEMU，其 `virtiofsd` 进程作为子进程跟随存活，收到 SIGTERM/SIGINT 转发给 QEMU 触发关机。该命令不建 cgroup，`locachy-app` 把它放进 `app_名称` cgroup，`virtiofsd` 自动继承同一分组。`--display gtk|sdl` 会附上 `virtio-vga`、`qemu-xhci` 和 `usb-tablet`，并在显示参数中加上 `window-close=off`：关闭窗口（其按钮、顶栏、概览）既不会让 QEMU 退出，也不会切断客户机电源。QEMU 的 `-name` 为 `locachy-compute[名称]`，窗口标题带有它：GTK 为 `QEMU (locachy-compute[名称])`，SDL 为 `QEMU (locachy-compute[名称]-N)`，桌面据此把该虚拟机应用的窗口与其他 QEMU 窗口区分开（见 `base/open.c` 的 `open_window_matches`）。
- `locachy-compute vm export-app 名称 [--app-name 应用名] [--display gtk|sdl] [--freeze qmp|none] [--network 0|1]` 写入 `/etc/locachy/containers/应用名.conf`（默认 `vm-名称`），含 `vm="1"`、`vm_kind="compute"`、`vm_name`、`vm_display`、`vm_freeze` 和 `network`。`locachy-app` 会直接读取这些键（`link/locachy-app` 中的 `vm_command` / `vm_check_keys`）。在真实宿主机 root 下，它准备属主为 uid 1000 的 `/run/locachy/compute/vm-<vm_name>`，并在 `app_应用名` 中以 uid 1000 启动 `locachy-compute vm run <vm_name> --display <display> --qmp-sock <sock>`（见 [VM-APPS.zh.md](VM-APPS.zh.md)）。

以 uid 1000 运行虚拟机时的路径权限（`vm run` 路径上无需 root）：

| 路径 | uid 1000 所需权限 |
|---|---|
| `LOCACHY_COMPUTE_RUN`（`/run/locachy/compute`） | 可穿越：`compute-client` 每次启动都把它设为 `0711`，uid 1000 无需列出目录即可进入自己的 `vm-名称`。另外 uid 1000 可写，或事先创建属主为 uid 1000 的 `vm-名称` 目录（非本 uid 属主时 `vm run` 会拒绝） |
| 运行目录 `…/compute/vm-名称/` | 属主 uid 1000，权限 `0700`（`vm run` 会设置）。`vm start` 以 root 创建的目录同样是 `0700` |
| 虚拟机目录 `…/compute/vms/名称/` | 属主 uid 1000（真实 root 执行 `vm create`、`vm reset` 时自动交接） |
| `vm.json` | 属主 uid 1000，权限 0600 |
| `overlay.qcow2` | 属主 uid 1000（读写） |
| `--disk` 基础镜像 | uid 1000 可读 |
| `--share` 目录与存储卷目录 | uid 1000 可读写 |
| `/dev/kvm` | 属组 `kvm` |
| `qemu-system-x86_64`、`virtiofsd` | uid 1000 可执行 |
| `/etc/locachy/containers/应用名.conf` | root 写入（`export-app`），`locachy-app` 可读 |

root 仍用于 `vm create`、`vm reset`（真实主机上交接文件属主）、`export-app`、cgroup 创建（`locachy-app`）以及镜像准备。

## Cgroup

所有资源都在 `/sys/fs/cgroup/locachy-compute/` 下，与桌面的 `app_*` 分组分开：

- `service`：控制守护进程
- `job-ID`：每个容器作业（可选 `memory.max` 和 `cpu.max`）
- `vm-名称`：每台微型虚拟机

创建 cgroup 是必需的。创建失败时，作业不会运行。只有测试框架会设置 `LOCACHY_COMPUTE_ALLOW_NO_CGROUP=1`。

## 配对

默认不配对。在你添加之前，没有任何设备会被信任。

```sh
locachy-pair init
locachy-pair add --name "工作用 Mac" --scope extdisplay,compute   # 在终端确认一次（脚本加 --yes），并打印一次性令牌
locachy-pair list
locachy-pair revoke "工作用 Mac"
```

- 存储位于 `/etc/locachy/peers/`（目录权限 `0700`，文件权限 `0600`）。`host.json` 保存稳定的 `host_id`；`host-cert.pem` 与 `host-key.pem` 保存节点的 ECDSA P-256 身份（自签名，有效期 20 年）。每个 `peers/对端ID.json` 保存一台已配对设备的信息：它的权限范围、扩展显示用的 PSK（16 字节），以及在 `compute` 范围下它出示的证书。
- `add` 只打印一次 `locachy-pair-v1:` 令牌。对于 `compute` 范围，令牌中包含客户端证书和私钥，因此只需把它复制到客户端一次。节点本身不保存这把私钥。
- 已配对设备一直有效，直到 `revoke` 删除它的文件。不会自动重新配对。撤销在下一次连接时生效：计算服务器在存储变化时重建信任列表，扩展显示代理则在每次尝试时重新读取存储。
- 权限范围彼此独立。只有 `extdisplay` 范围的设备会被计算接口拒绝；只有 `compute` 范围的设备无法打开扩展显示会话。

## 控制接口

`locachy-compute serve`（由 `service_compute` 启动）提供**可选的** HTTPS 控制接口，端口为 27184，使用 TLS 1.3，并强制要求客户端证书：

- 服务器只信任权限范围为 `compute` 的已配对设备的证书。若没有任何已配对设备，所有握手都会失败。
- 它只绑定到 `tailscale0` 的地址；当 `compute_bind_tb="1"` 时，还绑定到 `thunderbolt-net` 接口的 IPv6 链路本地地址。它从不绑定 `0.0.0.0` 或 `::`。
- 客户端按指纹固定服务器证书（对照客户端 `peer.json` 中的 `host_cert_sha256` 检查连接所出示的证书）。

接口：`GET /v1/health`、`GET /v1/envs`、`POST /v1/envs/名称/reset`、`GET /v1/jobs`、`POST /v1/jobs`、`GET /v1/jobs/ID`、`GET /v1/jobs/ID/log`、`POST /v1/jobs/ID/stop`、`GET /v1/vms`、`POST /v1/vms/名称/start`、`POST /v1/vms/名称/stop`、`POST /v1/vms/名称/reset`。

从另一台机器访问：

```sh
locachy-compute remote --creds-dir ./work-mac --host 100.x.y.z health
locachy-compute remote --creds-dir ./work-mac --host 'fe80::1%tb0' run 环境名 -- python3 train.py
```

## 网络：Tailscale 与可选雷电

普通 NVIDIA 实验路径使用 Tailscale 和 OpenSSH。计算节点运行在
`locachy-net` 中；`compute-client` 用
`nsenter --net=/run/netns/locachy-net` 进入该命名空间，而不是
`ip netns exec`。后者会挂载新的 `/sys` 并隐藏 `/sys/fs/cgroup`；
`nsenter` 只切换网络命名空间，因此守护进程既能使用该命名空间内的套接字，
也能访问宿主机的 cgroup 树。

Thunderbolt/USB4 是**独立的可选异构路径**，不是 SSH、NVIDIA 访问或 mTLS
接口的前置条件。启用可选绑定后，它可以提供主机间直连链路：

- 当 `extdisplay_tb_shared="1"` 时，或当 `service_compute` 与
  `compute_bind_tb="1"` 同时存在时，扩展显示代理让 `thunderbolt-net`
  接口留在 `locachy-net` 中。TLS 数据泵仍然只能使用该接口的链路本地地址，
  以 `SO_BINDTOIFINDEX` 绑定，并受 Landlock 与 seccomp 限制。
- `network-client` 将入站策略保持为丢弃。只有在已校验的
  `thunderbolt-net` 接口上，链路本地 IPv6 流量才会被放行，并且仅限于
  TCP 27183（扩展显示）、TCP 27184（可选计算 API）和 UDP 5353（mDNS）。
  Wi-Fi 和以太网接口从不加入允许名单。
- 当 `extdisplay_tb_shared="0"`（高隔离线缆模式）时，代理保持原有行为：
  接口被移入专用的会话命名空间；扩展显示开启期间，计算节点会失去这条链路。

应使用后端自己的 IP 传输，不应把 LOCachy HTTPS API 当作它的调度器。上面的
Thunderbolt 放行规则不会替任意后端端口打开通路。下面的 Open MPI 5 TCP 测试验证
已有后端可以在两个隔离的 IP 对端之间分割数值任务。它在 veth 上使用 IPv4，不是
上面的链路本地 IPv6 路径，也不验证 GPU 计算。真实的 Thunderbolt 部署仍需要两个
有地址的线缆端点、两端都支持的后端，以及只允许其实际传输所需端口的规则。

实机验收时要证明路线确实经过线缆，并证明预期的远端加速器产生了正确结果。拔掉
线缆后应确认任务不会悄悄回退到另一张网卡或 CPU；重新插回后，新任务应能成功，
不需要重新输入对端地址或重启节点。链路、ping 或 `iperf` 都不算这个验收。当前
LOCachy 通过真实 Tailscale 和 Thunderbolt 的完整工作流仍未验收。

## rc.conf 键

| 键 | 含义 |
|---|---|
| `service_tailnet_ssh="/usr/local/lib/locachy/tailnet-ssh-client"` | 启用受管 OpenSSH（`sshd`），默认关闭。 |
| `ssh_listen="tailscale"` 或 `"lan"` | `sshd` 只绑定 `tailscale0`（默认），或同时绑定活动的局域网接口与 `tailscale0`。 |
| `ssh_port="22"` | `sshd` 监听端口（`1..65535`，默认 `22`）。 |
| `ssh_password="0"` 或 `"1"` | 允许普通账号使用 PAM 密码登录（默认 `0`，只允许公钥；root 与空密码始终禁用）。 |
| `service_compute="/usr/local/lib/locachy/compute-client"` | 启用可选的 HTTPS API。 |
| `compute_port="27184"` | 控制接口端口（默认 27184）。 |
| `compute_bind_tb="1"` | 同时监听雷电链路本地地址。 |
| `compute_gpu="0"` | 从不把 GPU 交给容器。 |
| `compute_storage="卷名"` | 将计算镜像、环境、作业与虚拟机存放在 `/srv/storage/卷名/locachy-compute`（要求卷角色为 `compute` 或 `shared`，未挂载时报错中止）。 |
| `compute_default_env="容器名"` | 在 `设置 → 计算与虚拟机 → 计算节点 → 计算节点管理` 顶部作为默认工作容器显示与直接进入的容器（未设置时默认取第一个容器）。 |
| `extdisplay_tb_shared="1"` 或 `"0"` | 共享链路（计算绑定雷电时的默认值），或专用会话命名空间。 |
| `extdisplay_paired_only="1"` | 只接受已配对设备；不广播也不接受明文配对码。 |

## 测试

```sh
src/tools/test-peers.sh        # 对端存储、配对权限、代理 PSK、共享雷电链路、过滤规则
src/tools/test-compute.sh      # 镜像导入；原生容器（无 Wayland/合成器环境泄漏）；主机数据绑定挂载与路径策略拒绝；瞬时清空上层目录的重置；GPU 设备与库探测及通配绑定拒绝；KVM 微型虚拟机配置、qcow2 写时复制覆盖层的创建与重置、QEMU 参数校验；mTLS HTTPS 控制接口（已配对、未配对、仅扩展显示与已撤销的对端；远程作业的运行、日志与重置）
src/tools/test-sshd-config.py  # 受管 sshd 的 rc.conf/配置/nftables 规则、授权公钥增删、.pub 文件导入、`--keys-pending` 安全校验与一次性 `--receive-key` HTTP 接收流程
src/tools/test-thunderbolt-backend.sh # 隔离的 Open MPI TCP 分割求和夹具；不是实机雷电或 GPU
src/tools/test-extdisplay-broker.sh
```

`src/tools/test-tailnet-ssh.py` 应在一次性 Linux 测试客户机中作为 PID 1 运行，
内核命令行需要有 `locachy.tailnet-ssh-test=1`。它通过模拟的 Tailscale 接口运行
真正的 OpenSSH、SCP、SFTP 和 `sudo locachy-compute`，但不是真实 tailnet。
测试核对 SSH 重新绑定后分离作业的实际退出码和日志，并确认作业 cgroup 已清理。

compute shell 测试使用非特权 Linux 命名空间；可写 cgroup v2 子树不可用时，
对应收尾断言会跳过。测试接缝是以 `LOCACHY_` 开头的环境变量：`locachy-compute` 读取 `LOCACHY_PEERS_DIR`（经由 `locachy_peers.py`）、`LOCACHY_COMPUTE_BASE`、`LOCACHY_COMPUTE_RUN`、`LOCACHY_COMPUTE_CGROUP`、`LOCACHY_COMPUTE_RC`、`LOCACHY_COMPUTE_APP_INIT`、`LOCACHY_COMPUTE_ALLOW_NO_CGROUP` 与 `LOCACHY_CONTAINERS_DIR`（以及上文列出的虚拟机变量）；`compute-client` 还读取 `LOCACHY_COMPUTE_NETNS`、`LOCACHY_COMPUTE_BIN` 与 `LOCACHY_NETWORK_CLIENT`。扩展显示代理的测试夹具是编译期宏。

## 限制

- 要在外来根文件系统中以 1 号进程运行，必须使用静态链接的 `app-init`。主机构建的动态版本不会在那里使用，内置的回收逻辑会接手。
- 撤销设备不会结束它已经建立的连接。每个请求使用自己的连接，因此这些连接会很快结束。
- GPU 直通通过设备节点和库的探测来验证。NVIDIA 用户态软件栈本身，在这些测试中没有在真实硬件上运行。
- 雷电链路发现，以及 `sync_tb_filter` 填充允许名单的过程，需要真实的 `thunderbolt-net` 设备才能运行。测试使用一对 veth 虚拟网卡，并套用相同的规则。
