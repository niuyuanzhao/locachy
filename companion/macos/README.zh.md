# macOS 扩展屏发送端

面向 macOS 的 LOCachy 小型配套程序。它在当前登录会话中新增一块虚拟显示器，让 macOS 把窗口排布到真实的额外屏幕上，现有显示器保持不变。它只用 ScreenCaptureKit 捕获这块显示器，用 VideoToolbox 编码为 H.264，并通过专用的有线主机互连链路（Thunderbolt/USB4 IP 网络）发送给 LOCachy 接收端。LOCachy 的键盘和触摸板输入作用于这块显示器。

它不是镜像显示，不是屏幕共享／VNC，也不是完整的 macOS 应用。

## 上游来源与许可

虚拟显示器代码取自 `peetzweg/opendisplay` 提交 `0f44853adc5d44646a9e4d74e4516db460efb8bb`（发行版 1.30.2），归档 SHA-256 锁定在 `upstream.json` 中。`build.sh` 只使用上游的四个文件（`VirtualDisplay.swift`、`OriginRestore.swift`、`VirtualCanvasSizing.swift`、`CGVirtualDisplayPrivate.h`），并应用 `patches/session-display.patch`。该补丁使显示模式和位置的修改仅限于本会话，并移除了上游对其他显示器的重新配置。上游的 Mac 应用没有被复制。

上游以 GNU GPL 第 3 版发布（其 `LICENSE` 以哈希固定在 `upstream.json` 中）。本配套程序自己的文件与 LOCachy 其余部分一样为 GPL-3.0-or-later；构建出的发送端包含两者，按 GPL 第 3 版分发。

`CGVirtualDisplay` 是未公开的 CoreGraphics 接口；macOS 没有创建显示器的公开 API。`--check` 只报告这些私有类是否存在；若无法创建显示器，会话会失败。这意味着它不能上架 Mac App Store，属于 Developer ID／侧载配套程序。

## 构建与检查

需要 Xcode 命令行工具、`curl` 和 `shasum`。测试还需要带 `ssl` 模块的 `python3`（该模块需基于 OpenSSL 1.1.1 或更新版本；用于监听端和测试密钥。实际使用的是 Python 3.15 与 OpenSSL 3.5）。

```sh
companion/macos/build.sh /tmp/locachy-macos-build          # 仅构建二进制
companion/macos/tests/run-all.sh /tmp/locachy-macos-build  # 构建并运行全部检查
```

所有生成物都留在输出目录中。两条命令都不会安装发送端、创建显示器、捕获屏幕、发送输入或请求任何权限；`run-all.sh` 只以 `--check` 方式运行发送端。它依次运行协议、输入和配对码测试（输入写入记录型接收器，不发送任何事件），然后用独立的监听端进行 TLS 回环互操作测试，最后执行 `--check`。

## 运行

```sh
locachy-macos-sender --interface <有线接口> --address <169.254.x.x | fe80::...%接口> --port <端口>
```

发送端会以关闭回显的方式提示输入 LOCachy 屏幕上显示的 26 个字符的配对码。若标准输入不是终端，则改为从标准输入读取一行（供测试和脚本使用）。配对码绝不从参数、环境变量、文件、钥匙串或日志中获取。

首次运行前需要 macOS 14 或更新版本，并在系统设置中为该配套程序二进制授予屏幕录制和辅助功能权限。配套程序自身从不弹出权限请求。较新的 macOS 在连接链路本地对端时可能还需要本地网络权限。裸可执行文件没有 Info.plist 中的用途说明，因此将发送端打包为已签名的应用包仍是待办事项。

## 会话协议（已冻结）

**传输。** 接收端监听，发送端拨入。只接受一个指定有线接口上的数字形式链路本地地址。Wi-Fi、蜂窝和回环路径均被禁止。没有 DNS、没有发现机制，也不会回落到其他网络。

**TLS。**
- 仅 TLS 1.2，套件为 `TLS_PSK_WITH_AES_128_GCM_SHA256`（0x00A8）。
- PSK 身份为 `locachy-extdisplay-v1`；必须协商 ALPN `locachy-extdisplay-v1`。
- 会话票据和会话恢复均已禁用。任何出示的证书都会被拒绝。
- 已显式禁用重新协商；Network.framework 默认启用它。
- 对于 PSK，Network.framework 会忽略套件限制：ClientHello 会提供其全部四个 PSK 套件（AES-128/256，GCM 与 CBC；已从监听端验证）。TLS 1.2 握手也可能在未协商 ALPN 的情况下完成。因此，配套程序会在握手完成的那一刻检查协商得到的版本、套件和 ALPN，若有任何不符则在发送任何应用数据之前关闭连接。接入端必须只允许 `PSK-AES128-GCM-SHA256`；其他任何结果都是硬失败，没有回落。

**配对码。** LOCachy 接入端为每个连接生成 16 个随机字节，并以 RFC 4648 base32 显示：26 个字符，无填充，以 `-` 分组。输入不区分大小写，`-` 和空白字符会被忽略。任何不能规范地解码为恰好 16 字节的输入都会被拒绝。配套程序在把密钥交给 TLS 层之后会清零自己的副本，并且从不保存。

### 为什么使用 TLS 1.2 PSK，以及它不保护什么

Network.framework 无法用外部 PSK 完成 TLS 1.3。对 OpenSSL 4.1 的 TLS 1.3 PSK 监听端测试时，客户端在发送 ClientHello 之前即在本地以 `errSSLHandshakeFail`（-9858）失败，所有尝试过的配置均如此。TLS 1.2 PSK 可以工作。Network.framework 不提供 ECDHE-PSK 套件，因此会话没有前向保密。

之所以接受这一点，是因为密钥是每个连接一次的随机值，只显示在 LOCachy 屏幕上，由用户手动输入，仅保存在内存中，并且从不持久化。不存在一个长期密钥，其日后泄露会解密已录制的会话。没有选用带固定证书的 TLS 1.3，因为 Mac 端需要在没有系统 API 的情况下生成证书。

它不保护以下内容：
- 若某个连接的配对码泄露（在屏幕上被看到，或从任一进程的内存中读出），并且该连接的流量被录制，则该连接可以被解密。在线缆上、持有配对码的主动攻击者可以冒充任一方进行该连接。
- 密钥以 dispatch-data 副本的形式交给 Security.framework，其生命周期由框架控制。配套程序无法清零该副本。
- 会话期间发送端的内存转储或崩溃报告中可能包含密钥。

## 协议

接收端的契约具有权威性，发送端遵循它：
- `hello` 最先到达：`pv` 为 3，`locachy` 为 1，`extendedOnly` 为 true，包含面板信息、`videoCaps` 中的 H.264，以及全部五项输入能力。
- 发送端恰好回复一次 `welcome`，随后发送 `streamConfig`（H.264、尺寸、最高 60 的帧率）。
- 视频为完整的 Annex-B 访问单元，使用 4 字节起始码；每个 IDR 都携带 SPS/PPS。
- 接收端的 `ping` 会得到相同 `t` 的 `pong`。`closing` 结束会话。未知的消息类型会被忽略。

输入：
- `key` 使用接收端的逻辑 keysym，并按 Mac 当前的键盘布局解析。Super 映射为 Command，Alt／AltGr 映射为 Option，Ctrl 映射为 Control。按住状态按设备和键码分别跟踪。消息中的修饰键和锁定状态是该事件之前的状态；修饰键自身的按下会显示其修饰键，松开则清除它。
- `button` 覆盖 272/273/274；`pointer` 为归一化位置。
- `scroll` 的增量以流像素为单位，符号为自然滚动方向。`phase` 为 0 none、1 begin、2 update、3 end、4 cancel。begin、end、cancel 必须携带零增量，并且只能来自触控板类来源（2 手指、3 TrackPoint）。滚轮一格（`source` 为 1）以带增量的 phase 2 update 到达，发送为离散、非连续的滚轮事件，不带手势阶段；滚轮的 begin、end 或 cancel 会被拒绝。`source` 取值 0 至 4。
- `cancel`（`scope` 为 `all`）会释放全部按住的按键和按钮，并结束未完成的滚动手势。
- 格式错误的输入会在任何事件发出之前结束会话。

第一版的限制：仅 H.264；不支持实时调整尺寸（`hello` 改变时会结束会话，以便按新尺寸创建新会话）；每个轴最多 4096 像素，帧率会被降低，以保持在 H.264 level 5.2 的宏块限制之内；不支持音频、剪贴板或文件传输。

## 生命周期

一个进程只拥有一块虚拟显示器。会话在传输结束、八秒无数据、TLS 关闭、输入格式错误、收到 `closing` 或收到信号时结束。随后发送端释放按住的按键和按钮，结束未完成的滚动手势，停止捕获，并释放虚拟显示器的唯一持有者（这会将其移除），然后退出。其他显示器从不被重新配置。

## 验证状态

本地已验证：构建；39 项协议、输入与配对码检查；针对独立监听端的 TLS 1.2 PSK 回环互操作。该监听端在其 PSK 回调中强制校验身份和密钥，并记录协商得到的身份、套件、版本、ALPN 以及收到的成帧消息；另外还验证了以下情形会被拒绝：错误的配对码、错误的期望身份、错误的套件、缺少 ALPN、仅支持 TLS 1.3 的对端；以及 `--check`。

未验证：在真实 Mac 会话中创建虚拟显示器、捕获和输入注入；屏幕录制、辅助功能和本地网络提示；Thunderbolt／USB4 线缆；真实的 LOCachy 接收端和接入端。回环互操作不等于线缆实测。

参考监听端（强制校验身份，`tests/tls-listener.py`，基于 Python `ssl`）：

```sh
python3 -I tests/tls-listener.py 端口 <32 位十六进制> locachy-extdisplay-v1 locachy-extdisplay-v1 PSK-AES128-GCM-SHA256 1.2 日志文件
```

等效的 OpenSSL 命令同样能完成握手，但它不是身份测试：在 TLS 1.2 下，`s_server -psk_identity` 接受了以不同身份连接的客户端。基于 OpenSSL 的接入端必须在其 PSK 服务端回调中校验身份，并对其他任何身份不返回密钥。

```sh
openssl s_server -accept 127.0.0.1:端口 -tls1_2 -cipher PSK-AES128-GCM-SHA256 \
  -psk <32 位十六进制> -psk_identity locachy-extdisplay-v1 -nocert -alpn locachy-extdisplay-v1 -msg
```
