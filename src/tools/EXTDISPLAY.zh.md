# LOCachy 扩展屏

`extdisplay` 通过雷电／USB4 链路将 LOCachy 作为另一台计算机的扩展显示器。发送端创建虚拟显示器并将画面流式传输至 LOCachy，同时将 LOCachy 的键盘、触摸板与鼠标输入回传给发送端。

## 组成

| 文件 | 作用 |
|---|---|
| `progs/extdisplay.c` | 前端：屏幕输出、键盘与指针转发、会话生命周期。 |
| `progs/extdisplay-worker.c` | 沙箱 worker（`nobody`、Landlock、seccomp）：封包解析、JSON 与视频解码。 |
| `progs/extdisplay-protocol.[ch]` | 封包与控制／输入消息。 |
| `progs/extdisplay-decode.[ch]` | FFmpeg（`libavcodec`、`libavutil`、`libswscale`）H.264/HEVC 解码与色彩转换。 |
| `progs/extdisplay-ipc.h`、`progs/extdisplay-client.c` | `SOCK_SEQPACKET` 进程间通信与只读 memfd 帧传递。 |
| `progs/extdisplay-setup.[ch]` | 配对与链路状态客户端。 |
| `progs/extdisplay-broker.c`、`extdisplay-broker-net.c`、`extdisplay-broker-pump.c`、`extdisplay-broker-tls.c`、`extdisplay-broker.h`、`extdisplay-broker-protocol.h` | broker：在隔离网络命名空间中租用雷电接口、启动发现助手与 TLS 1.2 PSK 监听。 |
| `progs/extdisplay-advertise.c`、`extdisplay-discovery.[ch]` | 在租用的雷电链路上发布限定接口的 mDNS/DNS-SD 发现信息。 |
| `base/include/input-raw.h`、`base/input-raw.c` | 原始键盘事件队列。 |

## 安装与硬件加速

在 LOCachy 的 **设置 → 系统 → 可选组件** 中启用 **扩展屏 (extdisplay)**（依赖 FFmpeg、json-c、Avahi 与 OpenSSL）。

- 在 Linux 上，当活动显示设备的 DRM 驱动为 `i915`、`xe`、`amdgpu` 或 `radeon` 且存在可用 VA-API 驱动时，`extdisplay-worker` 会在进入 Landlock + seccomp 沙箱前完成 `libva` 初始化，使用 FFmpeg VA-API 硬件解码并通过 `VA-API VPP` → `DRM_PRIME_2` 零拷贝直出扫描帧；若运行在 `nouveau`、`nvidia-drm`、`simpledrm`、镜像多屏或老 GPU 不支持 4:4:4 硬解的环境，将自动无缝回退至 AVX2/SSE2 CPU 解码与 `MADV_HUGEPAGE` 只读 `memfd` 路径。
- 在系统安装、组件安装及同步更新期间，`locachy-pkg` 会自动扫描 `/sys/class/drm` 与 PCI 显示设备，为本机硬件自动补全匹配的 Vulkan 与 VA-API 驱动（Intel Gen8+ 自动安装 `intel-media-driver`，Gen4–7.5 安装 `libva-intel-driver`，AMD/Nouveau 安装 `mesa`，专有 NVIDIA 安装 `libva-nvidia-driver`，并在双显卡 Optimus / MX 笔记本上按实际驱动各自匹配）。
- 可在 **设置 → 输入与输出 → Extended display video**（`rc.conf` 的 `extdisplay_decode`：`"auto"` / `"hw"` / `"sw"`）中切换：自动模式（支持时走 GPU + 4:4:4）、优先硬件解码（老 GPU 上自动协商硬件 H.264），或仅使用纯 CPU 沙箱解码。

在 macOS 上通过 `niuyuanzhao/locachy` Homebrew tap 安装并启动后台发送端服务：

```sh
brew tap niuyuanzhao/locachy
brew install locachy-macos-sender
brew services start locachy-macos-sender
```

## 连接与显示协商

Thunderbolt/USB4 网络支持线缆直连，或经过兼容的雷电／USB4 Hub、Dock 与菊花链。

在 LOCachy 上打开 `extdisplay` 时：

1. `extdisplay-broker` 将已验证的 `thunderbolt-net` 接口移入独立网络命名空间，配置 IPv6 链路本地地址，并启动 `extdisplay-advertise` 在该链路上发布携带当次会话随机 128 位 PSK 的 `_locachy-ext._tcp.local.` 服务。
2. macOS 后台服务在雷电链路上自动发现该广播，并使用 `TLS_PSK_WITH_AES_128_GCM_SHA256`（身份与 ALPN 为 `locachy-extdisplay-v1`）自动建立连接。
3. 接收端在已认证的 `hello` 中上报其面板物理像素尺寸（`pixelsWide × pixelsHigh`）、缩放系数、脱敏后的 `host` 与 `model` 机器标识、支持的编码格式（`h264`、`hevc`）及输入能力。
4. 发送端按握手物理像素计算 `1:1` 基础几何（高密度且可整除为偶数逻辑点时采用 `@2x` HiDPI，否则 `@1x`），在系统“显示器”设置提供五档比例缩放（`2.0×`、`1.75×`、`1.5×`、`1.25×`、`1.0×`，对应“更大文字 ↔ 更多空间”），并按 `(host, model, pixelsWide × pixelsHigh)` 查询本机模式数据库（`~/Library/Application Support/LOCachy/display-modes.json`）：首次连接默认使用 `1:1`，用户在系统设置切换后自动记入该机器对应的表项，下次重连或重新插拔直接按表恢复所选大小。处于 `1:1` 档位时零缩放直出；中间分数 HiDPI 档位按原生 HiDPI 采样并经 Metal Mitchell 双三次（`B=1/3, C=1/3`）降采样至面板物理像素后编码传输。

## 会话行为

链路保持连接期间：

- 所有按键（含 `Super+Esc`、`Ctrl+Q`、`Esc`、`q`、`Ctrl+Alt+F12`）、触摸板手势与鼠标移动均转发给发送端，不会退出或断开 `extdisplay`。
- `Super+Esc`（回首页）与三指首页/切换手势不会切出活动扩展屏；`Super+L`（锁屏）仍可本地锁屏，同时保持扩展屏连接不断开（`base_session_hold`）。
- 扩展屏活动期间不触发空闲自动锁屏；若本机空闲已达锁屏时长，在拔出线缆或退出会话后立即锁屏。
- 拔出线缆后会话结束，按任意键关闭 `extdisplay`。

## 测试

```sh
src/tools/test-extdisplay.sh
src/tools/test-extdisplay-discovery.sh
src/tools/test-extdisplay-broker.sh
```
