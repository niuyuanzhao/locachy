# LOCachy 扩展屏

`extdisplay` 是 LOCachy 原生的**扩展屏接收器**。发送端保留原有显示器，另外新建一块归发送端本次会话所有的虚拟输出；LOCachy 只显示这块新增输出，并把本机键盘与指针回传。它不是对端屏幕的镜像，不是远程登录桌面，也不是 VNC 查看器。窗口之所以能到 LOCachy 屏上，是因为用户在发送端的显示器排列中把它拖到了这块新增输出上。

## 组成

| 文件 | 作用 |
|---|---|
| `progs/extdisplay.c` | 前端：直接屏幕、base 键盘与指针、会话所有者、状态显示；不新建 compositor。 |
| `progs/extdisplay-worker.c` | 沙箱 worker：唯一读取对端字节（封包、JSON、视频）的进程。 |
| `progs/extdisplay-protocol.[ch]` | 有界封包解析与下述 companion 协议。 |
| `progs/extdisplay-decode.[ch]` | 发行版 FFmpeg（`libavcodec`、`libavutil`、`libswscale`），不自写解码器。 |
| `progs/extdisplay-ipc.h`、`progs/extdisplay-client.c` | 定长 `SOCK_SEQPACKET` 消息；画面以封存只读 memfd 传递。 |
| `progs/extdisplay-setup.[ch]` | 配对客户端：broker 子进程（只保留 fd 3）、控制行、状态。用作真实子进程的桩 broker 测试。 |
| `progs/extdisplay-broker.c`、`extdisplay-broker-net.c`、`extdisplay-broker-pump.c`、`extdisplay-broker-tls.c`、`extdisplay-broker.h`、`extdisplay-broker-protocol.h` | broker：线缆租约与会话命名空间、TLS 1.2 PSK 监听与沙箱转发（见下文）。由 `extdisplay` 启动。 |
| `base/include/input-raw.h`、`base/input-raw.c` | 任何程序可选启用的原始键盘事件（见下）。 |

worker 以 `nobody` 运行，无补充组、`no_new_privs`、Landlock 规则不授予任何路径、资源上限，并以 seccomp 禁止新建 socket、连接、程序、进程、线程和向其他进程发信号；任一条件不满足即拒绝启动，前端显示“不可用”。未编入 FFmpeg 同样显示“不可用”，没有替代解码器或裸像素模式。前端同一时刻只有一幅画面在途；更新的画面替换尚未发出的旧画面，慢屏幕不会积压，也不会卡住数据流。

## 安装

接收器是可选组件“扩展屏 (extdisplay)”：设置 → 系统 → 可选组件。它安装 FFmpeg 和 json-c，并在 rc.conf 里写入 `program_extdisplay`，home 正是靠这一项列出该程序。两者都在，这一行才显示已安装：FFmpeg 若因别的软件已装上，这一行仍显示未安装，此时按 Enter 只补写 rc.conf 这一行并声明这些包。两者都装好时 `make.sh` 才编译 `extdisplay` 和 `extdisplay-worker`，否则打印 `make: extdisplay not built`。broker 只需要 OpenSSL，基础系统本来就会带上它。

## 传输与会话所有权

`extdisplay` 不接受参数。它自己启动 broker：`extdisplay-broker --control-fd 3`，从安装路径执行，控制通道是一对 socketpair。子进程中 broker 的一端是 fd 3，标准输入输出为 `/dev/null`，其他描述符全部关闭，包括会话所有者的描述符，因此 broker 永远够不到所有者。broker 是 `extdisplay` 的子进程，留在应用的 cgroup 内，PID 1 的停止与冻结仍然能作用到它。控制通道是 `SOCK_SEQPACKET`，每条消息一行 ASCII，最长 512 字节：`extdisplay` 发送 `scan`、`pair NAME`、`cancel` 与 `release`；broker 回复 `iface NAME DRIVER up|down` 行与 `end`，`lease NAME LINKLOCAL PORT` 与 `code GROUPS`（或 `error TEXT`），`paired`（恰好带一个描述符，即明文 `AF_UNIX` 流），以及 `ended REASON`。通道 EOF 表示 broker 释放并退出。接收器自己从不监听或拨出网络，TCP socket 会被拒绝。配对后的流承载下面的 companion 协议。已冻结的 broker 传输方案：

- 只走专用线缆链路（Thunderbolt/USB4 网络），位于会话私有网络命名空间，仅 link-local，无默认路由、DNS、转发和 Wi-Fi 回退；
- TLS 1.2，`TLS_PSK_WITH_AES_128_GCM_SHA256`，PSK identity 与 ALPN 均为 `locachy-extdisplay-v1`，不用会话票据、恢复或重协商；
- 每次连接由 LOCachy 新生成 16 字节随机 PSK，以 base32 分组显示在屏幕上，由用户在发送端输入；只在内存中，不保存、不记日志、不进 argv 或环境变量。

采用 TLS 1.2 PSK，是因为发送端平台的系统 TLS 不提供 TLS 1.3 外部 PSK。纯 PSK 密钥交换在**单次会话内没有前向保密**：拿到该次连接 PSK 且录下流量的人可以解密。之所以可以接受，是因为密钥随机、每次连接一份、由用户当场经直连线缆输入、连接结束即丢弃；事后入侵也找不到能解开旧会话的已存密钥。

中转进程（broker）负责线缆租约与 TLS 转发，见下文“Broker（中转进程）”一节。macOS 发送端位于 `companion/macos`，有自己的 README。安装路径上没有 broker 时，配对界面显示“配对助手已停止”。

任何会话都必须有受管会话所有者（`session.h`，由 process 1 仅对本程序设置 `LOCACHY_SESSION_FD`）。前端必须以 root 运行，因为它要把 worker 降权为 `nobody`；以其他用户运行则显示“不可用”。事件循环每一轮都先处理所有者策略，再转发输入。策略变化（锁屏、冻结、停止）时，前端停止转发、丢弃排队的本地输入，要求 worker 把 `cancel` 写进数据流，最多等 150 ms 确认（否则杀掉 worker，数据流随之结束），之后才向所有者确认；所有者最多等 250 ms。仅在已协商的数据流活动且允许输入时持有睡眠抑制。所有者描述符从不交给 worker。

## 配对界面

配对界面列出 broker 报告的线缆接口，每个带驱动与载波状态；没有硬编码的接口名。上下键选择，回车与所选接口配对，Esc 取消等待中的配对，F5 重新扫描，Ctrl+Alt+F12 释放 broker 并退出。broker 等待 Mac 时，地址、端口与配对码一起显示在各自的区域里，并附 Mac 上的命令形式（`locachy-macos-sender --interface <mac-interface> --address 地址 --port 端口`）。配对码与地址只在内存中，离开屏幕时清除；从不写入日志，也不放进 argv 或环境变量。配对成功后进入会话；会话结束时，broker 给出的原因显示在状态行，列表随即回到屏幕上。broker 发来的意外或格式错误的行会结束通道，界面显示助手已停止。

## Broker（中转进程）

`extdisplay-broker` 是接收器传输中的特权部分。`extdisplay` 以 `/usr/local/lib/locachy/extdisplay-broker --control-fd 3` 启动它，标准输入、输出和错误都接到 `/dev/null`（此时 broker 把日志写到内核日志），除此之外没有其他打开的描述符。broker 本身保持 root：它租用线缆，并拥有会话命名空间。只有处理网络字节的 TLS 转发进程在建立完成后放弃全部特权。broker 进程自己从不读取数据流；转发进程解密后，把明文作为一条 `AF_UNIX` 流交给 `extdisplay`。

### 控制

每条消息是一个 `SOCK_SEQPACKET` 报文，内含一行 ASCII（0x20 到 0x7e），最长 512 字节。末尾的换行可以接受，但不会发送。

`extdisplay` 发给 broker：

| 行 | 回复 |
|---|---|
| `scan` | 每个候选接口一行 `iface NAME DRIVER up` 或 `down`（最多 16 个），然后 `end`。租约活动期间回复 `error busy`。 |
| `pair NAME` | `lease NAME LINKLOCAL PORT`，然后 `code GROUPS`；或 `error TEXT`。租约活动期间回复 `error busy`。 |
| `cancel` | 有尝试或会话时：`ended cancelled`。空闲时：无回复。 |
| `release` | `ended released`，然后 broker 退出。 |
| 控制通道结束 | 释放一切并退出。无回复。 |
| 其他 | `error unknown command` |

broker 主动发出：

- `paired`，恰好带一个 `SCM_RIGHTS` 描述符：明文一端，是 `AF_UNIX` `SOCK_STREAM`。它跟在 `code` 之后，且先于任何 `ended`。
- `ended REASON`，每次尝试恰好一次。REASON 为 `handshake-failed`、`timeout`、`carrier-lost`、`released`、`cancelled`、`error` 之一。`error` 表示数据流自行结束（对端关闭，或 TLS 或转发失败）。除 `released` 外的任何 `ended` 之后，broker 保持存活并空闲，租约已释放；`scan` 与 `pair` 重新被接受。

`LINKLOCAL` 是不带区域（zone）的裸 IPv6 链路本地地址：区域属于发送端自己的接口。`PORT` 为 `27183`（`EXTDISPLAY_BROKER_PORT`）。Mac 伴生程序没有默认端口，它通过 `--port` 指定。`GROUPS` 即配对码。

### 候选与身份

只有驱动为 `thunderbolt-net`（从 sysfs 读取）的网络设备是候选，且只在 locachy-net 中才是候选，因为 NetworkManager 在那里掌管网络。不假定任何接口名。测试构建在编译时指定另一个驱动名，没有运行时覆盖。设备的身份是它的永久硬件地址（没有则用当前地址），加上设备的 sysfs 路径。

### 租约

收到 `pair` 后，broker：

1. 拒绝不是候选的接口，或无法读取其身份的接口；
2. 记录租约：在 `/run/locachy/extdisplay-broker` 下写一个仅 root 可读的文件 `lease-<address>`，内容包括启动标识、名称、索引、地址和设备路径；
3. 创建一个全新的网络命名空间，不带名字，也不做绑定挂载，只包含 `lo`；
4. 把设备从 locachy-net 移入该命名空间，移动后检查设备上仍然是被租用的地址（只用 netlink 检查；sysfs 显示的是挂载它的命名空间）。locachy-net 不存在时 broker 拒绝运行，从不退回到宿主命名空间；
5. 在设备本身上把 `accept_ra`、`autoconf`、`forwarding` 和 `accept_redirects` 设为 0，因为设备会保留它原来所在位置的设置；
6. 把设备拉起，最多等 10 秒，直到出现 IPv6 链路本地地址。若出现其他地址、存在默认路由，或没有载波，则拒绝。内核自己的链路本地路由允许存在；
7. fork 转发进程，它绑定监听并报告 `listening`。然后 broker 发送 `lease` 与 `code`。

会话命名空间中没有其他可达的东西：没有 DNS，没有转发，没有 Wi-Fi 回退，也没有通往任何其他网络的路由。

### 归还

在取消、释放、载波丢失、超时、转发失败、控制通道结束或收到信号时，转发进程被停止。对于已建立的会话，broker 先要求它发送 close_notify 并关闭 TCP 连接，最多等 500 毫秒。然后设备回到 locachy-net，前提是它仍然是被租用的那个设备（地址相同）。如果名字已被另一台设备占用，内核会从同一格式中分配一个空闲名字（例如 `extdisplay0` 对应 `extdisplay%d`），另一台设备不受影响。设备回到 locachy-net，或从未离开时，记录被删除。

broker 从不驱动 NetworkManager。它不改变 NetworkManager 的运行时 managed 状态，也不恢复它。NetworkManager 对归还的设备应用其配置的策略，与对任何新出现的设备一样。租约前人工设置的运行时状态不会保留，插拔后被清除的状态也不会保留。

### 监听与 TLS

监听是 TCP，绑定到被租用设备的链路本地地址，作用域为其接口索引（`SO_BINDTOIFINDEX`、`IPV6_V6ONLY`）。每次尝试只接受一个连接，且只接受同一作用域内的链路本地对端。一次尝试最长 300 秒，其中握手最长 20 秒。

配置即上文传输部分所述的那一套，由 broker 自己的服务端上下文强制执行：

- 只允许 TLS 1.2；TLS 1.3 被禁用并拒绝；
- 密码套件列表为 `PSK-AES128-GCM-SHA256`，握手后再次核对协商的套件；
- PSK identity `locachy-extdisplay-v1` 在 broker 自己的 PSK 回调中检查，因为 OpenSSL 本身不比较 identity；
- 要求 ALPN `locachy-extdisplay-v1`。没有 ALPN 的 ClientHello 被拒绝，并核对协商的值；
- 不使用会话票据，不使用会话缓存，不做重新协商。

### 配对码

每次尝试，broker 从 `getrandom()` 取 16 个随机字节。它们只保存在内存中，握手后清零，从不写日志，从不放进 argv 或环境变量，也从不写入磁盘。配对码是 26 个大写的 RFC 4648 base32 符号，每四个一组，以 `-` 分隔，最后一组只有两个符号。最后一个符号承载最后三位，其低两位为零。

Mac 发送端的解析器接受任意大小写、`-`、空格和行尾，并拒绝任何不能规范地解码为恰好 16 字节的输入。broker 的解码器遵循同样的规则。

### 转发与特权

在监听和 TLS 上下文建立之后，分出的转发进程在读取任何网络字节之前，依次做以下事情：

- 只保留它的三个描述符（命名空间句柄、明文一端和状态一端），关闭其余所有描述符；
- 从 bounding、ambient、effective、permitted 和 inheritable 集合中去掉全部能力，切换为 `nobody` 并清空补充组；
- 设置 `no_new_privs`，使自己不可转储（non-dumpable），并应用资源限制；
- 应用一条 Landlock 规则集，不授予任何路径，也不允许 TCP 绑定或连接；
- 应用一条 seccomp 过滤器，禁止新建 socket、连接、进程、命名空间与挂载相关调用、`ptrace`、密钥、BPF、io_uring、凭据变更和信号。在唯一一次 accept 之后，过滤器还会禁止 `accept`。

它的父进程死亡信号是 `SIGKILL`，因此它随 broker 一起终止。它的沙箱由测试套件检查（见下文）。

### broker 被杀

broker 死后没有需要保留的状态。租约记录在磁盘上。会话命名空间在它最后一个描述符关闭时结束；住在其中、随 broker 死亡的转发进程不会让命名空间继续存在。之后由内核决定设备去向。依据上游源码（`net/core/dev.c` 中的 `default_device_exit_net`），带有 rtnl 链路操作的设备随其命名空间一起销毁，其他设备则移到初始网络命名空间。`thunderbolt-net` 没有设置链路操作。这是从源码读来的结论，尚未在带有该硬件的运行内核上观察到。

- 没有 rtnl 链路操作的设备（例如 `thunderbolt-net`）会被移到初始网络命名空间。之后 `link/90-locachy-network.rules` 运行 `network-client --ifindex`，把它移回 locachy-net。这一环不需要额外添加任何东西。
- 虚拟设备（有链路操作，例如 veth 或 dummy 设备）随命名空间一起销毁。

记录会保留，直到具有该地址的设备再次出现在 locachy-net 中。每次 `scan` 和 `pair` 都会记录它（写到 stderr；stderr 为 `/dev/null` 时写到内核日志），格式为 `lease of NAME did not end cleanly; its device is not back in locachy-net yet`。候选只在 locachy-net 中列出，所以设备在回到那里之前不可能被租用。上一次开机留下的记录会被删除。

`extdisplay` 必须在宿主挂载命名空间中启动 broker（由 PID 1 在那里启动 `extdisplay`）。broker 会进入 locachy-net，并在它自己的挂载命名空间中重新挂载 sysfs。

### broker 测试

`src/tools/test-extdisplay-broker.sh [WORK_DIRECTORY]` 只能在 Linux 上运行。它接受一个可选的工作目录，只写入该目录。它为每项检查打印一行 `PASS:` 或 `FAIL:`，无法在此运行的部分打印 `SKIP:`。各部分如下：

- `build`：使用生产编译参数构建 broker。
- `code`：配对码的编码、解码与规范性拒绝。
- `tls`：以 Python 的 `ssl` 作为对端的十个回环用例：TLS 1.3、仅证书的套件以及另一个 PSK 套件（均被拒绝）；提供四个 PSK 套件（选中 `PSK-AES128-GCM-SHA256`）；错误的 identity、错误的密钥、错误的 ALPN 和没有 ALPN（均被拒绝）；正确的 identity、密钥和 ALPN（明文双向回显，正常的 close_notify）；以及第二次连接不恢复会话。
- `lease`：通过控制套接字驱动 broker，运行在测试自身的用户与网络命名空间中，以一对 veth 作为线缆。覆盖候选过滤、会话命名空间内容、配对、错误配对码、取消、超时、以 link-down 模拟的载波丢失、控制通道结束、协议错误、被杀的 broker 及其记录、期间另一台设备占用了名字的情况，以及转发进程的沙箱探测。

测试固件都在编译时指定：`EXTDISPLAY_BROKER_TEST_DRIVER`、`EXTDISPLAY_BROKER_TEST_HOME_NETNS`、`EXTDISPLAY_BROKER_TEST_STATE_DIR`、`EXTDISPLAY_BROKER_TEST_ATTEMPT_SECONDS` 与 `EXTDISPLAY_BROKER_TEST_PROBES`。生产二进制中没有其中任何一个。

由本脚本在 Linux 上验证的：上文列出的全部内容，包括被杀与记录行为，以及转发进程的沙箱探测。已设计但未在此验证的：真实的 `thunderbolt-net` 设备（发现、线缆载波、broker 被杀后内核移到初始命名空间，以及经 udev 与 `network-client` 回到 locachy-net，这需要一台可随意重装的虚拟机，因为在测试主机上这样的设备会到达主机的真实命名空间）；NetworkManager 对归还设备的处理；按地址拒绝租约的旧记录；对端一侧的载波丢失（区别于管理员的 link-down）；以及 Mac 发送端通过真实线缆配对。

## Companion 协议

与 OpenDisplay pv 3 的封包及 `hello`/`streamConfig` 形态兼容，另加必需的 LOCachy 握手；不带该握手的普通 OpenDisplay 发送端得不到会话。

```
4 字节大端负载长度，然后是负载（1 字节到 4 MiB）
```

小于 32 KiB、以 `{` 开头且不含 NUL 的负载是控制消息（JSON 对象）；其余必须是一个完整的 H.264 或 HEVC Annex-B 访问单元，使用四字节起始码，前面可带 OpenDisplay 的遥测前缀（不超过 256 字节的可打印 ASCII，跳过不解析）。每个 IDR 都携带参数集。`streamConfig` 之前的视频、三字节起始码、损坏的 JSON、已知类型的非法字段、或画面尺寸与协商不符，都会结束会话。解码错误同样结束会话：本版本没有关键帧请求，因此发送端在每次连接和每次改变尺寸时都从 IDR 开始，中途断开的 GOP 直接关闭而不猜测恢复。未知控制类型被忽略。画面上限 4096×4096、1600 万像素、每秒 60 帧。指针移动是速率：当待发给对端的数据超过 16 KiB 时，指针消息被丢弃；按键、按钮、滚动和取消永不丢弃。

1. 接收端：`hello`，含 `pv:3`、`locachy:1`、`extendedOnly:true`、`panel {pixelsWide, pixelsHigh, scale}`（LOCachy 屏上的画面区域）、`videoCaps`（H.264；HEVC 仅当所链接的 FFmpeg 能解码时列出），以及 `input:["key","button","cancel","pointer","scroll"]`。
2. 发送端：一次 `welcome`，含 `locachy:1`、`extended:true` 和全部五项输入能力。
3. 发送端：`streamConfig {codec, width, height, framesPerSecond}`，随后发送访问单元；再次发送 `streamConfig` 即改变尺寸。
4. 双方：每 2 秒 `ping {t}`，以 `pong {t}` 应答；8 秒无数据即结束会话；`closing` 结束会话。

接收端发出的输入（数值均为 JSON 数字）：

- `key {device, code, keysym, modifiers, locks, state, mapping:"logical"}`：`code` 为 Linux evdev 键码（物理身份），`keysym` 为按下时 LOCachy 当前配列的 XKB keysym，其重复与抬起沿用同一值；`state` 0 抬起、1 按下、2 重复；`modifiers` 位 1 Shift、2 Ctrl、4 Alt、8 Super、16 AltGr、32 Caps Lock 开、64 Num Lock 开；`locks` 位 1 Caps、2 Num。两者都是本事件发生前的状态，因此按下 Shift 的消息不带 Shift 位。
- `button {button, state, x, y}`：272 左、273 右、274 中；`x`、`y` 归一化到 0–1，始终存在。
- `pointer {x, y}`：归一化 0–1。
- `scroll {dx, dy, phase, source}`：带小数的流像素，符号与 OpenDisplay 自然滚动一致（`dy` 为正表示内容向下，`dx` 为正表示内容向右），由 LOCachy 像素按画面比例换算。滚轮一格是一次更新（`phase` 2、`source` 1），量为 LOCachy 的 15 像素；开始、结束与取消只来自手指和 TrackPoint 手势，增量为零。`phase` 0 无、1 开始、2 更新、3 结束、4 取消；`source` 0 无、1 滚轮、2 手指、3 连续（TrackPoint）、4 触摸。触摸屏拖动本版本不作为滚动转发。
- `cancel {scope:"all"}`：释放本接收端按住的一切。发送端在数据流结束和心跳超时时也必须这样做。

本地动作只在本地生效、从不发送：home、锁屏、亮度、音量，以及始终断开连接的 **Ctrl+Alt+F12**。本地动作按下时先取消对端按住的状态，因为 process 1 可能紧接着冻结本程序。没有剪贴板、文件传输、终端、命令通道或屏幕共享，也不合成捏合、横滑和惯性手势。

## 原始键盘 API

`base_raw_enable(1)` 后，只要有排队的原始事件，`key_get`/`key_poll*` 就会返回 `K_RAW_KEY`，由 `base_raw_next()` 取出。每个事件含设备身份（evdev 的 `st_rdev`，Wayland 座位为一个固定 id）、evdev 键码、按下时配列的 keysym、修饰键、锁定状态、状态和本地绑定标志。没有对应按下记录的抬起或重复被丢弃。设备移除、`SYN_DROPPED`、输入被收回、Wayland 失焦或 keymap 变化、队列溢出时，发出一次“全部取消”事件，而不是猜测补发抬起。`pointer_scroll_precision(1)` 为指针事件增加带小数的 `scroll_dx`/`scroll_dy` 像素。未启用的程序行为不变。

## 测试

```sh
src/tools/test-extdisplay.sh
```

它编译协议、FFmpeg 解码部分，在 Linux 上还编译 IPC 客户端，然后运行：封包、握手与输入用例；IPC 边界用例（封存的画面可映射，未封存、尺寸不符、两个描述符、非画面消息上的描述符、错误魔数与保留位都被拒绝）；worker 缺少通道时必须以退出码 78 拒绝运行；配对客户端对桩 broker 的测试（真实子进程，只保留 fd 3，故意留着的描述符不得到达它）；以及用已安装的 `ffmpeg` 编码的 H.264 与 HEVC 流，经接收器自己的分流与解码路径解码，并覆盖尺寸不符拒绝和损坏切片数据。脚本不安装任何东西，不启动显示、VM 或网络。这些测试未覆盖：worker 在沙箱内处理实时数据流（带补充组的测试用户或 root 按设计都会被拒绝）、前端面对真实屏幕，以及任何真实对端。

broker 有自己的测试套件 `src/tools/test-extdisplay-broker.sh`（仅 Linux，见“broker 测试”）。两者都在 `tools/test-interaction-regressions.sh` 中运行。

## 发送端与验收边界

macOS 发送端必须用私有 `CGVirtualDisplay` 接口创建新增输出（没有公开 API 能创建），用 ScreenCaptureKit 只捕获该显示器，用 VideoToolbox 编码；需要用户授予“屏幕录制”和“辅助功能”权限，不能代为授予；Mac 原显示器保持不变，断开时只移除本会话自己的虚拟显示器。Windows 与 Linux 发送端需要各自的虚拟输出组件（IddCx 驱动；compositor 的 headless 输出）。以上均未在真机上运行：Mac 虚拟显示器创建、线缆链路、配对、拖窗到屏、JIS 与圆形滚动、睡眠抑制和拔线行为仍待实机验收。
