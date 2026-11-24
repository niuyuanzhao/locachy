# LOCachy 计算节点

`service_compute` 把一台 LOCachy 机器变成无界面的计算节点。它用原生容器运行 Linux 程序，容器的根文件系统可以来自任何发行版；也可以在 KVM 微型虚拟机里运行。计算节点能访问 GPU 和数据盘，并通过经过认证的 HTTPS 控制接口接受远程调度。它不依赖 Wayland、合成器、X11、席位管理或会话总线，桌面的冻结和停止操作也不会影响它。

## 组成

| 文件 | 作用 |
|---|---|
| `link/locachy-compute` | 命令行工具和守护进程：镜像、环境、原生容器、作业、KVM 微型虚拟机、`serve`，以及作为客户端的 `remote`。 |
| `link/compute-client` | `service_compute` 的启动脚本。用 `nsenter --net` 进入 `locachy-net`，再运行 `locachy-compute serve`。 |
| `link/locachy-pair` | 一次配对、永久信任：添加、列出、查看、撤销已配对设备，并导出令牌。 |
| `link/locachy_peers.py` | 共享的对端存储（`/etc/locachy/peers`），由 `locachy-pair`、`locachy-compute` 和 `extdisplay-broker` 共用。 |
| `link/network-client` | `locachy-net` 的入站过滤规则，包括雷电（Thunderbolt）允许名单（`tb_ifaces`、`tb_tcp_ports`、`tb_udp_ports`）。 |
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

## 重置与数据

- `locachy-compute env reset 名称` 会停止该环境的作业，并删除 `upper/` 和 `work/`。基础镜像和所有已挂载的数据集保持不变，因此重置在毫秒内完成。
- 主机数据用 `--mount 主机路径:容器路径[:ro|:rw]` 挂载，或在创建环境时用 `env create --mount` 写入配置。允许任何已存在的绝对路径，例如 `/mnt/nvme/...`、`/data/...`、`/var/lib/locachy/compute/data/...`。以下位置禁止挂载：`/`、`/proc`、`/sys`、`/dev`、对端存储、`/var/lib/locachy/compute`（其 `data/` 子目录除外）、`/run/locachy/compute`，以及扩展显示代理的状态目录。
- 远程对端无法指定挂载。通过网络提交的作业只能在节点管理员配置好的环境中运行。

## GPU

创建环境时加上 `--gpu`（且 `compute_gpu` 不为 `"0"`），容器会获得 `/dev/nvidiactl`、`/dev/nvidia-modeset`、`/dev/nvidia-uvm*`、`/dev/nvidiaN`、`/dev/nvidia-caps/*` 和 `/dev/dri/renderD*`，以及主机 NVIDIA 用户态库的只读绑定挂载（`libcuda.so*`、`libnvidia-*.so*`、`libnvcuvid.so*`、`libnvoptix.so*`），位于 `/usr/local/lib/locachy-gpu`。`locachy-compute gpu-probe` 列出主机上实际存在的设备。不存在的设备会被跳过。

## KVM 微型虚拟机

`locachy-compute vm create 名称 --disk 基础.qcow2 [--share 目录:标签] [--cloud-init 种子.img]` 会创建写时复制的覆盖层（`qemu-img create -b 基础 -F qcow2`）。`vm start` 运行 `qemu-system-x86_64`：若存在 `/dev/kvm`，加上 `-enable-kvm`，否则使用 TCG。它提供 virtio 磁盘、串口控制台套接字、QEMU 监视器套接字，每个 `--share` 对应一个 virtio-9p 共享目录，并可选择挂载 cloud-init。`vm reset` 丢弃覆盖层，恢复干净状态。每台虚拟机运行在自己的 cgroup 中：`/sys/fs/cgroup/locachy-compute/vm-名称`。

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

`locachy-compute serve`（由 `service_compute` 启动）在 HTTPS 端口 27184 上监听，使用 TLS 1.3，并强制要求客户端证书：

- 服务器只信任权限范围为 `compute` 的已配对设备的证书。若没有任何已配对设备，所有握手都会失败。
- 它只绑定到 `tailscale0` 的地址；当 `compute_bind_tb="1"` 时，还绑定到 `thunderbolt-net` 接口的 IPv6 链路本地地址。它从不绑定 `0.0.0.0` 或 `::`。
- 客户端按指纹固定服务器证书（对照客户端 `peer.json` 中的 `host_cert_sha256` 检查连接所出示的证书）。

接口：`GET /v1/health`、`GET /v1/envs`、`POST /v1/envs/名称/reset`、`GET /v1/jobs`、`POST /v1/jobs`、`GET /v1/jobs/ID`、`GET /v1/jobs/ID/log`、`POST /v1/jobs/ID/stop`、`GET /v1/vms`、`POST /v1/vms/名称/start`、`POST /v1/vms/名称/stop`、`POST /v1/vms/名称/reset`。

从另一台机器访问：

```sh
locachy-compute remote --creds-dir ./work-mac --host 100.x.y.z health
locachy-compute remote --creds-dir ./work-mac --host 'fe80::1%tb0' run 环境名 -- python3 train.py
```

## 网络：Tailscale 与雷电同时使用

计算节点运行在 `locachy-net` 中。`compute-client` 用 `nsenter --net=/run/netns/locachy-net` 进入该命名空间，而不是 `ip netns exec`。`ip netns exec` 会挂载新的 `/sys`，从而隐藏 `/sys/fs/cgroup`。`nsenter` 只切换网络命名空间，因此守护进程既能使用该命名空间内的套接字，也能访问主机的 cgroup 树。

雷电/USB4 主机间链路是共享使用，而不是被整条交出：

- 当 `extdisplay_tb_shared="1"` 时，或当 `service_compute` 与 `compute_bind_tb="1"` 同时存在时，扩展显示代理让 `thunderbolt-net` 接口留在 `locachy-net` 中。它的 TLS 数据泵仍然只能使用该接口的链路本地地址，并以 `SO_BINDTOIFINDEX` 绑定，同时受 Landlock 与 seccomp 限制。
- `network-client` 将入站策略保持为丢弃。只有在已校验的 `thunderbolt-net` 接口上，链路本地 IPv6 流量才会被放行，并且仅限于 TCP 27183（扩展显示）、TCP 27184（计算）和 UDP 5353（mDNS）。Wi-Fi 和以太网接口从不加入允许名单。
- 当 `extdisplay_tb_shared="0"`（高隔离线缆模式）时，代理保持原有行为：接口被移入专用的会话命名空间。扩展显示开启期间，计算节点会失去这条链路。

## rc.conf 键

| 键 | 含义 |
|---|---|
| `service_compute="/usr/local/lib/locachy/compute-client"` | 运行计算节点。 |
| `compute_port="27184"` | 控制接口端口（默认 27184）。 |
| `compute_bind_tb="1"` | 同时监听雷电链路本地地址。 |
| `compute_gpu="0"` | 从不把 GPU 交给容器。 |
| `extdisplay_tb_shared="1"` 或 `"0"` | 共享链路（计算绑定雷电时的默认值），或专用会话命名空间。 |
| `extdisplay_paired_only="1"` | 只接受已配对设备；不广播也不接受明文配对码。 |

## 测试

```sh
src/tools/test-peers.sh        # 对端存储、配对权限、代理 PSK、共享雷电链路、过滤规则
src/tools/test-compute.sh      # 原生容器、重置、挂载、策略、微型虚拟机覆盖层、mTLS 认证
src/tools/test-extdisplay-broker.sh
```

两个新测试都在非特权的用户、网络和挂载命名空间中运行，不安装任何东西。测试接缝是以 `LOCACHY_` 开头的环境变量（`LOCACHY_PEERS_DIR`、`LOCACHY_COMPUTE_BASE`、`LOCACHY_COMPUTE_RUN`、`LOCACHY_COMPUTE_CGROUP`、`LOCACHY_COMPUTE_APP_INIT`、`LOCACHY_COMPUTE_NETNS`、`LOCACHY_COMPUTE_ALLOW_NO_CGROUP`）。扩展显示代理的测试夹具是编译期宏。

## 限制

- 要在外来根文件系统中以 1 号进程运行，必须使用静态链接的 `app-init`。主机构建的动态版本不会在那里使用，内置的回收逻辑会接手。
- 撤销设备不会结束它已经建立的连接。每个请求使用自己的连接，因此这些连接会很快结束。
- GPU 直通通过设备节点和库的探测来验证。NVIDIA 用户态软件栈本身，在这些测试中没有在真实硬件上运行。
- 雷电链路发现，以及 `sync_tb_filter` 填充允许名单的过程，需要真实的 `thunderbolt-net` 设备才能运行。测试使用一对 veth 虚拟网卡，并套用相同的规则。
