# NVIDIA GPU 解锁与机型功耗配置

LOCachy 将独立 NVIDIA GPU 的底层硬件能力直接并入系统的原生分层：`/usr/local/share/locachy/machines/` 机型配置、`rc.conf` 分层覆盖、`/etc/modules-load.d/` 与 `/etc/modprobe.d/`（由 `src/base/modload.c` 加载）、`locachy-boot` 统一内核镜像（UKI），以及由「设置」驱动的 `locachy-jobs` 系统任务。

## 显示路由与独显开关

**设置 → 输入与输出 → NVIDIA GPU** 下有三项独立控制：

- **混合显卡策略**对应 `gpu_graphics_policy="auto"`（默认）或 `"off"`。能确认是内置 NVIDIA 混合显卡、且内屏接在核显上时，原生界面优先使用内屏所在的卡，labwc 优先用核显渲染，同时保留两张卡的输出。不能确认拓扑、手动指定显示卡或关闭策略时，保留原有选择。此项不装驱动，不强制应用使用 NVIDIA，不改固件，也不影响 CUDA。下次启动图形界面时生效。
- **固件显示路由**在内核 `asus-armoury` 驱动确实暴露 MUX 控制时，可选混合或 NVIDIA 独显直连。等待重启时显示的是固件里已设定的值，不是当前生效的路由；系统不会自动重启。
- **NVIDIA 独显**使用 `asus-armoury` 的独立硬件开关。禁用后独显端口和 CUDA 都不可用，直到重新启用。只有能确认当前是混合模式、没有待重启的固件设置、且核显有已连接内屏时，才能请求禁用；独显正在使用时，驱动还可以拒绝。普通运行时省电和 `gpu-unlock` 的 RTD3 设置不变。

固件设置需再次按 Enter 确认。`locachy-gpu-graphics status --json` 可读回固件配置；`set-mux hybrid|discrete` 和 `set-dgpu on|off` 走特权系统任务。未暴露、不可读或冲突的控制不会猜测写入。若机型只能从 BIOS 设置切换，就在 BIOS 中操作；启动旧的 UKI 不能撤销已写入的固件设置。

## 0. NVIDIA DKMS 驱动管理（`locachy-nvidia`）

LOCachy 统一通过 **DKMS** 管理 NVIDIA 驱动（不使用绑定特定内核分支的预编译模块包），按 `auto` → `open` → `580xx` → `off` 排序，贴合 Arch Wiki 建议并与 LOCachy 的内核切换（`locachy-pkg kernel select`）协同：

- **`auto`**：读取 PCI sysfs（`/sys/bus/pci/devices/*/vendor`、`device`、`class`）并从 `/usr/share/hwdata/pci.ids` 解析 NVIDIA（`10de`）设备描述（不可用时回退到 `lspci -nn -d 10de:`），按 Arch Wiki 的 NVIDIA 架构表识别 GPU 代际，安装推荐分支对应的 DKMS 与用户态包（若推荐分支不在已启用软件源中，会改用下一个受支持分支，并打印回退提示），同时在 `rc.conf` 中记录 `nvidia_driver="auto"`。
- **`open`**（新版，Turing RTX 20+ / Ampere / Ada / Blackwell 及更新架构默认）：`nvidia-open-dkms` + `nvidia-utils` + `dkms` + 当前内核头文件。开源内核模块强制使用 GSP-RM 固件。
- **`580xx`**（旧版闭源 / 可关 GSP：Maxwell / Pascal / Volta，作为 Turing / Ampere / Ada 笔记本的手动回退分支（GSP-RM 出现故障时，通过 `NVreg_EnableGpuFirmware=0` 关闭 GSP 固件）：`nvidia-580xx-dkms` + `nvidia-580xx-utils` + `dkms` + 当前内核头文件。注意 `cachyos/nvidia-580xx-utils` 依赖 `[multilib]` 中的 `lib32-nvidia-580xx-utils`，`locachy-nvidia` 在改动已安装驱动前会先检查 `[multilib]` 是否启用。
- **`off`**：移除已安装的 NVIDIA 驱动包、清理 `/etc/locachy/packages` 条目、删除 `/etc/modprobe.d/40-locachy-nvidia.conf` 并重建 UKI。

### PCI 架构自动识别与分支映射（遵循 Arch Wiki）

| GPU 架构代际（`pci.ids` 中的芯片代号） | 受支持分支 | `auto` 推荐 | 说明 |
| --- | --- | --- | --- |
| Blackwell / Hopper（`GB*`、`GH*`）及更新 | `open` | `open` | `580xx` 不支持 Blackwell；手动执行 `set 580xx` 会被拦截（除非显式传入 `--force`）。 |
| Ada Lovelace（`AD*`）、Ampere（`GA*`）、Turing（`TU*`） | `open`、`580xx` | `open`（台式机与笔记本相同；启用 `gpu_unlock_targets` 时同样为 `open`） | 检测到 Ampere 笔记本 GPU（`GA*M`、名称含 Mobile/Laptop 或 DMI 机箱类型为笔记本）时，会提示 Arch Wiki 的说明：部分 Ampere 笔记本上 GSP-RM 会导致完全失效；若本机受影响，请手动切换到 `580xx` 并关闭 GSP。 |
| Volta（`GV*`）、Pascal（`GP*`）、Maxwell（`GM*`） | `580xx` | `580xx` | Turing 之前的 GPU 无 GSP；手动执行 `set open` 会被拦截（除非显式传入 `--force`）。 |
| Kepler（`GK*`）、Fermi（`GF*`）、Tesla（`G8*`/`G9*`/`GT2*`）、Curie（`G7*`/`NV4*`） | 无 | `off`（`set auto` 会明确拒绝） | `open` 与 `580xx` 均已不再支持，请使用 `nouveau` 或 AUR 的 `470xx`/`390xx`。 |
| 未知 NVIDIA PCI ID（尚未收录的新卡） | `open`、`580xx` | `open` | 在 `pci.ids` 收录前按新卡默认推荐 `open`。 |

若机器同时存在多张 NVIDIA GPU，`locachy-nvidia` 会求各卡受支持分支的交集：全部卡都支持 `open` 与 `580xx` 时沿用单卡规则；仅有一个分支同时覆盖全部卡时推荐该分支；若出现一卡仅支持 `open`、另一卡仅支持 `580xx`（或含不受支持的旧卡）的冲突，`status` 会明确报告冲突且 `set auto` 会拒绝安装。

「设置」在 **输入与输出 → NVIDIA GPU** 顶部提供两行控制：

- **NVIDIA driver (DKMS)**（`nvidia_driver="auto"|"open"|"580xx"|"off"`）：选择器行（`Enter` 进入 → `Left`/`Right` 切换 → 再次 `Enter` 通过 `gpu-driver-set` 应用），并展示 `locachy-nvidia status --tsv` 返回的 GPU 架构与推荐分支。
- **NVIDIA GSP firmware**（`nvidia_gsp="auto"|"off"|"on"`）：作用于 `580xx`（通过 `gpu-driver-gsp`）。`auto` 在 `gpu_unlock_targets` 启用时自动写入 `options nvidia NVreg_EnableGpuFirmware=0`；`off` 强制关闭；`on` 强制开启。`open` 分支始终使用 GSP-RM，`locachy-nvidia` 因此保持 GSP 开启；若同时启用 `gpu-unlock`，`status` 会给出提示（GSP-RM 不会阻止 `gpu-unlock`；595 分支的回归由 `NVreg_GpuInitOnProbe` 默认值为 0 引起，615 及以上分支已恢复为探测时初始化。若在受影响的 595 版本或 GSP 不稳定的机器上出现问题，可设置 `NVreg_GpuInitOnProbe=1` 或手动切换到 `580xx`，见 `Zagitta/gpu-unlock` #135）。

命令行接口：

```sh
locachy-nvidia status [--json | --tsv]
locachy-nvidia set auto|open|580xx|off [--force]
locachy-nvidia gsp auto|off|on
locachy-nvidia sync-dropin
```

手动指定与硬件不匹配的分支（例如在 Maxwell/Pascal/Volta 上执行 `set open`，或在 Blackwell 上执行 `set 580xx`）会在改动软件包前被拦截并给出原因，只有显式传入 `--force` 时才允许覆盖。在 `open` 与 `580xx` 之间切换时，工具会原子替换冲突包（`pacman -S --needed --noconfirm --ask 4`），更新 `/etc/locachy/packages`（`locachy-pkg` 的 Vulkan 驱动探测同时兼容 `nvidia-utils` 与 `nvidia-580xx-utils`），生成 `/etc/modprobe.d/40-locachy-nvidia.conf`（`blacklist nouveau`、`options nouveau modeset=0`、`options nvidia-drm modeset=1`、`softdep nvidia post: nvidia-uvm` 及按需写入的 `NVreg_EnableGpuFirmware`），在启用 `nvidia_powerd="1"` 时同步服务，并重建 UKI。`locachy-gpu-unlock` 在安装、开关和卸载时也会自动调用 `locachy-nvidia sync-dropin`。

## 1. 驱动前 NVIDIA GPU 解锁（`locachy-gpu-unlock`）

部分 NVIDIA GPU 在驱动初始化前会应用固件中的速度选择器。`locachy-gpu-unlock` 将上游 [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) 直接集成进 LOCachy。上游内核模块为其 recipe 表中的 PCI ID 解除该限制，具体效果与范围见上游 README。在「设置」中进入 `Input & Output -> NVIDIA GPU -> NVIDIA GPU unlock (gpu-unlock)` 按 `Enter` 即可一键从 GitHub 拉取、构建并启用，也支持命令行操作：

```sh
locachy-gpu-unlock status [--json]
locachy-gpu-unlock install [--target all|0000:01:00.0] [--repo URL] [--branch NAME]
locachy-gpu-unlock set off|all|0000:01:00.0
locachy-gpu-unlock remove
```

实现机制：

1. **上游跟踪与最小 PCI ID 补丁**：上游仓库元数据存放在 `/usr/local/share/locachy/gpu-unlock/upstream.json`。执行 `install` 时，`locachy-gpu-unlock` 通过 `locachy-net`（若启用）浅克隆上游仓库，幂等地将 `extra_pci_ids` 中上游尚未收录的 PCI ID（例如 RTX 3080 Ti Laptop 对应的 `GA103(0x2460)`）插入 `kernel/gpu_unlock/recipes.c`，随后切换到隔离的非 root 构建用户（`locachy-build` / `builder` / `nobody`）执行 `makepkg`。上游 `PKGBUILD` 自行下载固定版本的官方 NVIDIA 固件输入并做 SHA-256 校验。
2. **包清单与 DKMS**：`locachy-gpu-unlock` 通过 `locachy-pkg` 确保内核头文件与构建依赖（`dkms`、`git`、`python`、`zstd`、`openssl`）已就位，安装生成的 `gpu-unlock-dkms` 与 `gpu-unlock-firmware`，并将两者登记到 `/etc/locachy/packages`。
3. **原生启动顺序（无需改动 initramfs）**：LOCachy 不将 `nvidia` 打进 initramfs。`switch_root` 之后，1 号进程调用 `modules_start()`（`src/base/modload.c`），严格先于 PCI `coldplug()` 加载 `/etc/modules-load.d/*.conf`，并解析 `/etc/modprobe.d/` 中的 `softdep`。`locachy-gpu-unlock` 写入：
   - `/etc/modules-load.d/50-locachy-gpu-unlock.conf`（`gpu_unlock`）
   - `/etc/modprobe.d/50-locachy-gpu-unlock.conf`（`softdep nvidia pre: gpu_unlock` 与 `options nvidia NVreg_DynamicPowerManagement=0x00`，关闭 RTD3 断电以防止解锁后的频率选择器掉电复位）
   - `/etc/locachy/rc.conf` 中的 `gpu_unlock_targets="..."`
4. **UKI 启动参数**：`locachy-boot` 按分层配置读取 `gpu_unlock_targets`（`all` 或逗号分隔的 `DDDD:BB:DD.F` PCI 地址），向 UKI 内核命令行追加 `gpu_unlock.targets=...` 并重建启动镜像。
5. **卸载**：在「设置」对应行连续按两次 `Delete`（或运行 `locachy-gpu-unlock remove`），将清理 drop-in 配置、清除 `gpu_unlock_targets`、卸载软件包与清单条目，并重建 UKI。

电源状态说明：上游 `gpu-unlock` 已挂接 S3 挂起/唤醒回调以在睡眠恢复后重新应用设置；当前尚未覆盖 S4 休眠恢复。

驱动兼容性说明：上游报告（`Zagitta/gpu-unlock` #135）显示 GSP-RM 本身并不阻止解锁。595 分支的问题来自 NVIDIA 将 `NVreg_GpuInitOnProbe` 默认值改为 `0`（延迟初始化会让速度选择器重新锁定）；615 及以上分支把默认值恢复为探测时初始化的 `AUTO`。上游 #88 报告 HP RTX 3080 Ti Laptop（`GA103`，`0x2460`）可以正常工作。上游 #206 记录过一例：580 系列 open 模块下速度选择器被复位，而关闭 GSP 的闭源 580xx 仍保持锁定，因此 `580xx` 仍保留为手动回退。

## 2. 机型 GPU 功耗配置（`locachy-gpu-power`）

机型专用的独显功耗控制与默认显卡、`boot_cmdline` 一样声明在 `src/machines/`（安装到 `/usr/local/share/locachy/machines/`）的机型配置文件中。

### HP OMEN 17-ck1xxx（`hp-omen-17-ck1.conf`）

主板 `8A1A`（Intel i9-12900HX + NVIDIA RTX 3080 Ti Laptop）：

- `display_card="xe"`、`extdisplay_card="xe"`：将直连帧缓冲与扩展显示固定在 Intel 核显，避免无故唤醒独显。
- `boot_cmdline="nouveau.modeset=0 nvidia_drm.modeset=1 i915.force_probe=!4688 xe.force_probe=4688"`：将 PCI ID `0x4688` 的核显交给 `xe` 驱动（对其排除 `i915`），并禁用 `nouveau`。（`locachy-boot` 会从激活的机型配置读取 `boot_cmdline` 并在 `/etc/locachy/rc.conf` 未覆盖时自动生效，同时允许 `force_probe` 排除语法中的 `!`。）
- `gpu_power_method="hp-omen-wmi"`、`gpu_power_mode="full"`、`nvidia_powerd="1"`：通过仓库内置的 DKMS 驱动 `src/machines/hp-omen-gpu/`（`hp_omen_gpu`）设置 HP BIOS WMI 独显功耗模式，将 RTX 3080 Ti Laptop 从出厂 115 W 提升至 150 W cTGP，配合 `nvidia-powerd` 动态加速最高可达 175 W。

`hp_omen_gpu` 调用与上游 `hp-wmi` 完全一致的 HP BIOS WMI 接口（`GUID 5FB7F034-2C63-45E9-BE91-3D44E2C707E4`，命令 `HPWMI_GM 0x20008`，查询 `0x21` 读 / `0x22` 写），每次写入前都先读回并原样保留 `gpu_slowdown_temp` 温度阈值，不导出 `MODULE_ALIAS`，无常驻轮询进程或后台定时器。

### 功耗模式与「设置」入口

「设置」在 `Power & Battery -> NVIDIA GPU power` 下提供两行控制：

- **GPU power mode**（`gpu_power_mode`）：
  - `auto`：跟随 LOCachy 的 `power_profile`（`performance` -> `full`、`balanced` -> `ctgp`、`power` -> `eco`、`auto` 模式下插电为 `ctgp` / 电池为 `stock`）
  - `full`：`ctgp=1 ppab=1 dstate=1`（150 W cTGP + Dynamic Boost 最高 175 W，D1 100%）
  - `ctgp`：`ctgp=1 ppab=0 dstate=1`（固定 150 W cTGP，D1 100%）
  - `stock`：`ctgp=0 ppab=0 dstate=1`（出厂 115 W TGP，D1 100%）
  - `eco`：`ctgp=0 ppab=0 dstate=2`（出厂 TGP，D2 50%）
- **NVIDIA Dynamic Boost (nvidia-powerd)**（`nvidia_powerd`）：在 `/usr/bin/nvidia-powerd` 已安装时，通过 1 号进程启停 `service_nvidia_powerd="/usr/bin/nvidia-powerd"`。

命令行接口：

```sh
locachy-gpu-power status [--json]
locachy-gpu-power apply [auto|full|ctgp|stock|eco]
locachy-gpu-power nvidia-powerd on|off
locachy-gpu-power remove
```

在「设置」中切换机型配置（`System -> About This System -> Machine profile`）选中 `hp-omen-17-ck1` 时，系统会自动应用对应的 GPU 功耗配置并重建包含该机型 `boot_cmdline` 的启动镜像。
