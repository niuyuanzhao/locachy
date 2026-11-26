# LOCachy 扩展屏

`extdisplay` 通过雷电／USB4 链路将 LOCachy 作为另一台计算机的扩展显示器。发送端创建虚拟显示器并将画面流式传输至 LOCachy，同时将 LOCachy 的键盘、触摸板、鼠标、多点触摸屏与主动手写笔输入回传给发送端。

## 组成

| 文件 | 作用 |
|---|---|
| `progs/extdisplay.c` | 前端：屏幕输出（非阻塞 DRM 翻页 + 多 plane dma-buf）、键盘、指针、多点触控与手写笔转发、会话生命周期。 |
| `progs/extdisplay-worker.c` | 沙箱 worker（`nobody`、Landlock、seccomp）：封包解析、JSON 与视频解码。 |
| `progs/extdisplay-protocol.[ch]` | 封包、能力协商（`h264`/`hevc420`/`hevc444`/`hevc`、`touch`/`tablet`）与控制／输入消息。 |
| `progs/extdisplay-decode.[ch]` | FFmpeg（`libavcodec`、`libavutil`、`libswscale`）H.264/HEVC 解码、VA-API 硬件能力探测与多 plane VPP dma-buf 导出。 |
| `progs/extdisplay-ipc.h`、`progs/extdisplay-client.c` | `SOCK_SEQPACKET` 进程间通信与只读 memfd 帧传递。 |
| `progs/extdisplay-setup.[ch]` | 配对与链路状态客户端。 |
| `progs/extdisplay-broker.c`、`extdisplay-broker-net.c`、`extdisplay-broker-pump.c`、`extdisplay-broker-tls.c`、`extdisplay-broker.h`、`extdisplay-broker-protocol.h` | broker：在隔离网络命名空间中租用雷电接口、启动发现助手与 TLS 1.2 PSK 监听。 |
| `progs/extdisplay-advertise.c`、`extdisplay-discovery.[ch]` | 在租用的雷电链路上发布限定接口的 mDNS/DNS-SD 发现信息。 |
| `base/include/input-raw.h`、`base/input-raw.c` | 原始键盘事件队列。 |

## 安装与硬件加速

在 LOCachy 的 **设置 → 系统 → 可选组件** 中启用 **扩展屏 (extdisplay)**（依赖 FFmpeg、json-c、Avahi 与 OpenSSL）。

- 在 Linux 上，`extdisplay-worker` 会在进入 Landlock + seccomp 沙箱前通过预打开的 `/dev/dri/renderD*` 描述符向 `libva` 查询真实硬件 VLD 解码能力，并通过 `VA-API VPP` → 多 plane `DRM_PRIME_2` 配合非阻塞 `DRM_IOCTL_MODE_PAGE_FLIP` 零拷贝直出扫描帧（驱动拒绝翻页时记录日志并回退至 `SETCRTC`）。`hello.videoCaps` 精确声明硬件支持的档案（`h264`、具备 `VAProfileHEVCMain` VLD 时报 `hevc420`、仅当真正具备 `VAProfileHEVCMain444` VLD 时才报 `hevc444` 与旧字段 `hevc`），避免没有 4:4:4 硬解的显卡误入慢速 4:4:4 软件解码。运行在 `nouveau`、`nvidia-drm`、`simpledrm` 或无 VA-API 的虚拟机时，自动回退至沙箱 CPU 解码与 `MADV_HUGEPAGE` 只读 `memfd` 路径。
- 在系统安装、组件安装及同步更新期间，`locachy-pkg` 会自动扫描 `/sys/class/drm` 与 PCI 显示设备，为本机硬件自动补全匹配的 Vulkan 与 VA-API 驱动（Intel Gen8+ 自动安装 `intel-media-driver`，Gen4–7.5 安装 `libva-intel-driver`，AMD/Nouveau 安装 `mesa`，专有 NVIDIA 安装 `libva-nvidia-driver`，并在双显卡 Optimus / MX 笔记本上按实际驱动各自匹配）。
- 可在 **设置 → 输入与输出 → Extended display video**（`rc.conf` 的 `extdisplay_decode`：`"auto"` / `"hw"` / `"sw"`）中切换：自动选择硬件支持的最高编解码、优先硬件解码，或仅使用纯 CPU 沙箱解码。

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
3. 接收端在已认证的 `hello` 中上报其面板物理像素尺寸（`pixelsWide × pixelsHigh`）、缩放系数、脱敏后的 `host` 与 `model` 机器标识、精确硬件解码能力（`h264`、`hevc420`、`hevc444`、`hevc`）及输入能力（在 5 项基础输入之外协商 `"touch"` 与 `"tablet"`）。
4. 发送端根据上报面板尺寸自动推导适用于任意主流分辨率和纵横比的 5 档 HiDPI 缩放比例与对应物理尺寸提示（涵盖“更大文字 ↔ 更多空间”及 `1:1` HiDPI／原生档），并按 `(host, model, pixelsWide × pixelsHigh)` 记入 `~/Library/Application Support/LOCachy/display-modes.json` 供重连自动恢复。处于 `1:1` 档位时零缩放直出；其余缩放档位经 Metal Mitchell 双三次（`B=1/3, C=1/3`）采样至面板物理像素后编码传输。

## 会话行为

链路保持连接期间：

- 所有按键（含 `Super+Esc`、`Ctrl+Q`、`Esc`、`q`、`Ctrl+Alt+F12`）、触摸板手势与鼠标移动均转发给发送端，不会退出或断开 `extdisplay`。
- 当发送端支持 `"touch"` 与 `"tablet"` 时，触摸屏多点手势（单指点击／拖拽／长按右键、双指平移滚动、双指捏合缩放）和主动手写笔（接近／离开、笔与橡皮擦切换、亚像素定位、压感、X/Y 倾斜角、悬停距离、旋转、笔身按键及提笔手掌防误触）严格映射到扩展屏范围，同时相对鼠标／触控板仍可跨全部屏幕自由移动。
- `Super+Esc`（回首页）与三指首页/切换手势不会切出活动扩展屏；`Super+L`（锁屏）仍可本地锁屏，同时保持扩展屏连接不断开（`base_session_hold`）。
- 扩展屏活动期间不触发空闲自动锁屏；若本机空闲已达锁屏时长，在拔出线缆或退出会话后立即锁屏。
- 拔出线缆或发送端重启后，会话结束且 `extdisplay` 回到等待状态，线缆重新接入时自动重新连接，无需重启或重新打开；等待界面下 `Esc` 取消配对、`Super+Esc` 返回首页、`Ctrl+Q` 退出。

## 测试

```sh
src/tools/test-extdisplay.sh
src/tools/test-extdisplay-input.sh
src/tools/test-extdisplay-discovery.sh
src/tools/test-extdisplay-broker.sh
```
