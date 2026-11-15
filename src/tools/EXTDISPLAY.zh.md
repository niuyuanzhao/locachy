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

## 安装

在 LOCachy 的 **设置 → 系统 → 可选组件** 中启用 **扩展屏 (extdisplay)**（依赖 FFmpeg、json-c、Avahi 与 OpenSSL）。

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
3. 接收端在 `hello` 中上报其面板的物理像素尺寸（`pixelsWide × pixelsHigh`）、缩放系数、支持的编码格式（`h264`、`hevc`）及输入能力。
4. 发送端根据接收端上报的物理像素尺寸创建虚拟显示器，并按面板尺寸比例自动派生 5 档缩放模式（`2.0×`、`1.75×`、`1.5×`、`1.25×`、`1.0×`）供 macOS **系统设置 → 显示器** 选择与记忆，同时始终以面板物理像素分辨率传输画面。

## 会话行为

链路保持连接期间：

- 所有按键（含 `Ctrl+Q`、`Esc`、`q`、`Ctrl+Alt+F12`）、触摸板手势与鼠标移动均转发给发送端，不会退出或断开 `extdisplay`。
- `Super+Esc`（回首页）与 `Super+L`（锁屏）照常切换本地画面，同时保持扩展屏连接不断开（`base_session_hold`）。
- 扩展屏活动期间不触发空闲自动锁屏；若本机空闲已达锁屏时长，在拔出线缆或退出会话后立即锁屏。
- 拔出线缆后会话结束，按任意键关闭 `extdisplay`。

## 测试

```sh
src/tools/test-extdisplay.sh
src/tools/test-extdisplay-discovery.sh
src/tools/test-extdisplay-broker.sh
```
