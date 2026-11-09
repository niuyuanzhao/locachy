# 系统任务（locachy-jobs）

耗时的 root 操作——软件包事务、内核与软件源的更改、启动镜像、容器构建、固件——作为**系统任务**运行：由一个服务持有唯一的队列，**按请求的先后一次只运行一个**，与发起请求的程序分开。关闭、退出、冻结或停止一个程序都不会影响它的任务；两个程序也不会再同时运行 pacman（或 pacman 与 locachy-boot，或两次 flashrom）。

## 服务

`/usr/local/lib/locachy/locachy-jobs`（`progs/locachy-jobs.c`）是 rc.conf 中的服务 `service_jobs`（make.sh 会为旧的 rc.conf 补上）。进程 1 像启动其他服务一样启动它，它结束 5 秒后再次启动；除此之外进程 1 与它无关。

- 它以 root 运行在自己的 cgroup `/sys/fs/cgroup/locachy-jobs/service` 中。
- 每个任务运行在 `/sys/fs/cgroup/locachy-jobs/job-ID` 中，并有自己的会话：一个 *runner*（服务的子进程）先进入该 cgroup，再依次运行任务的各个步骤，输出写入任务日志。任务结束后，它在 cgroup 中留下的一切会被结束（`cgroup.kill`），cgroup 随之删除。
- 服务自己的文件只给 root（umask 077）；每个步骤以 umask 022 运行，与 root 的 shell 相同：步骤写的文件（pacman 的配置和镜像列表、rc.conf、清单）别人也要读，其中包括以构建用户身份运行的 pacman。
- 这些都不是程序的 cgroup（`pN`），也不是容器的（`app_NAME`）：进程 1 冻结和结束的是后者，从不碰前者。屏幕易手时任务不会被冻结，窗口关闭时任务也不会结束。

## 种类

固定的表；每个种类以 `execv` 运行系统中固定的程序，先带固定参数，再带任务自己的参数——不经过 shell。服务检查每一个参数，其他一律拒绝。

| 种类 | 命令 | 参数 |
|---|---|---|
| `pkg-install`、`pkg-remove` | `locachy-pkg install` / `remove` | 1–32 个软件包名：字母、数字、`@ . _ + -`，不以 `-` 或 `.` 开头 |
| `pkg-swap` | 先 `locachy-pkg remove 旧`，再 `locachy-pkg install 新` | 2 个软件包名 |
| `pkg-sync`、`pkg-plan` | `locachy-pkg sync` / `plan` | 无 |
| `pkg-apply` | `locachy-pkg apply` | 至多 63 个目标名（容器名） |
| `kernel-select` | `locachy-pkg kernel select` | 由 `[a-z0-9+._-]` 组成、以 `linux` 开头的包名 |
| `kernel-headers` | `locachy-pkg kernel headers` | `on` 或 `off` |
| `repos-rank` | `locachy-repos rank` | 无 |
| `repos-set` | `locachy-repos apply --set` | `core`、`extra` 或 `multilib`，加 `=on` 或 `=off` |
| `boot` | `locachy-boot` | 无 |
| `app-build`、`app-rollback` | `locachy-app build` / `rollback` | 一个容器名（与 locachy-app 的规则相同） |
| `app-remove` | `locachy-app remove` | 一个容器名，可加 `--data` |
| `fw-refresh`、`fw-update` | `locachy-firmware fwupd-action refresh` / `update` | 无 |
| `fw-verify`、`fw-unlock` | `locachy-firmware fwupd-action verify` / `unlock` | 一个 fwupd 设备 ID |
| `fw-local-install` | `locachy-firmware fwupd-action local-install` | 一个普通文件的绝对路径 |
| `fw-install-release` | `locachy-firmware fwupd-action install-release` | 设备 ID；一个 `http(s)://` 地址，或 `install`、`upgrade`、`downgrade`、`reinstall`、`update` |
| `spi-probe` | `locachy-firmware spi-probe` | 一个 flashrom 编程器；`--chip 名称` |
| `spi-backup`、`spi-verify` | `locachy-firmware spi-backup` / `spi-verify` | 编程器，以及文件夹（备份）或镜像文件；`--chip`、`--regions fd,bios,…` |
| `spi-write` | `locachy-firmware spi-write` | 同 `spi-verify`，另加 `--expected-size 字节数` |

与仍在排队的某个任务完全相同（种类与参数）的请求，就是那个任务。

## 状态

`queued` → `running` → `done`（状态码 0）、`failed`（其状态码），或 `keys`：某次构建（locachy-pkg 或 locachy-app 的任务）因有尚未被人信任的源码签名密钥而停下——locachy-build 以 3 退出，或在任务期间把它们写入 `/var/lib/locachy/builder/untrusted-keys`（此时 locachy-pkg 只报告失败）。这不是失败：发起请求的程序列出这些密钥，在它们被信任后请求剩下的部分（packages：`g`）。正在被停止的任务处于 `stopping`，之后为 `cancelled`；被移出队列的排队任务立即为 `cancelled`。除 `queued`、`running`、`stopping` 外的状态都表示已结束，下一个任务随即开始。

## 文件

`/run/locachy/jobs/`（0700，属 root），每个文件 0600：

- `ID.state`——`kind=`、`state=`、`status=`、`from=`（发起请求的程序名）、`runner=`、`submitted=`、`started=`、`ended=`（自 1970 年起的秒数），每个参数一行 `arg=`，以及 `last=`（结束后其输出的最后一行）。
- `ID.log`——输出：第一行为 `locachy-jobs: job ID: 种类 参数... (from 程序)`，之后是其程序写出的内容。
- `ID.exit`——runner 写入的结束方式；`ID.run`——runner 存活期间对它加锁。服务取得任务的结束后两者都会删除。

保留最新的 32 个已结束任务及其文件。`/run` 是 tmpfs：队列不会跨越重启。

## 控制 socket

`/run/locachy/jobs/control`：`SOCK_SEQPACKET`，权限 0600，所在文件夹 0700，只有 root 能访问。它从不被绑定进容器（`link/locachy-app` 给容器一个自己的 `/run`；locachy-app 的宿主程序以用户身份运行）。对端用 `SO_PEERCRED`（uid 0）及其 cgroup（`/proc/PID/cgroup`）检查：容器的进程（`app_NAME`）被拒绝；原生窗口（`app_window-PROGRAM-N`，root）和直接程序（`pN`）可以。直接程序和窗口以同样的方式连接它，不经进程 1 或 seat 转发。

每条消息一个请求、一个回复，字段之间以 NUL 分隔：

| 请求 | 回复 |
|---|---|
| `submit 种类 参数...` | `ok ID 位置`（0：立即运行；N：前面还有 N 个任务），或 `error 原因` |
| `list` | `ok 行...`：保留的所有任务，旧的在前 |
| `status ID` | `ok 行` |
| `log ID` | `ok 文本`：其输出的末尾（至多 8 KiB，从某行开头起） |
| `cancel ID` | `ok removed`（它在排队）、`ok ended`，或 `confirm 原因`（它在运行） |
| `cancel ID confirm` | `ok stopping` |

任何请求都可能得到 `error 原因`（未知 ID、格式错误）。“行”为 `ID 种类 状态 状态码 位置 来源 参数 最后一行`，以制表符分隔：状态码结束前为 -1（128 + N：信号 N），位置 0 为运行中、N 为排队、-1 为已结束，参数以空格分隔，最后一行为其输出的最后一行。

`base/include/jobs.h`（`base/jobs.c`，链接进各程序，不进入进程 1）封装了它：`jobs_submit`、`jobs_list`、`jobs_status`、`jobs_log`、`jobs_cancel`、`jobs_mine`（本程序跟踪的任务：按去掉 `-window` 后的名字匹配的自身任务，以及同一领域内所有运行中或排队中的任务，使 `packages`、`settings` 与 `firmware` 互相感知彼此的活动任务）、`jobs_pending`。`locachy-pkg` 与 `locachy-repos` 在执行变更操作时还会持有 `/run/locachy/pkg.lock`，避免 root 终端直接运行命令与正在执行的队列任务冲突。

## 停止任务

排队中的任务立即移除。运行中的任务只有带 `confirm` 才会停止，服务会先要求确认并说明原因：中途停止的软件包事务可能让系统处于半更改状态。停止时向任务步骤所运行的程序发送 SIGINT（与 Ctrl+C 相同；pacman 自己的中断：在两个软件包之间停下并释放锁），而不发给步骤本身（Python 工具在等待 pacman 时被中断，会在 0.25 秒后杀死 pacman）；步骤没有运行其他程序时才发给它。若 20 秒后任务仍在运行，其 cgroup 中的一切都会被杀死。

由人在 home 中停止：`J` 列出任务，`K` 询问（Y/N，运行中的任务附带警告），Y 将其移除或停止。

## 服务结束或重启

SIGTERM（进程 1 结束该服务、其行从 rc.conf 删除时）只结束服务本身：运行中的任务在自己的会话和 cgroup 中继续。下次启动时读取状态文件，凭 runner 持有的锁找到运行中的任务（并通过 pidfd 监视它），从 `ID.exit` 取得期间已结束任务的结果，然后继续处理队列。被杀死的服务以同样的方式重新开始。

## 关机

有任务运行或排队时，home 拒绝重启或关机，并说明是哪个任务（`jobs_pending`；服务不在时，仍有进程的任务 cgroup 也算）。

进程 1 自己的关机与重启在有任务运行时先等待，再结束任何东西：root 执行的 `poweroff` 和 `reboot`（SIGUSR2、SIGTERM）、Ctrl+Alt+Del、电源键、home 的请求以及电量严重不足。只要某个 `/sys/fs/cgroup/locachy-jobs/job-*/cgroup.events` 写着 `populated 1`，就算有任务在运行；进程 1 不看任务的其他任何东西（不连 socket，不看队列），读 `/run/locachy/jobs/ID.state` 的第一行只是为了显示任务种类。

- 它用 inotify 监视这些文件及其所在文件夹，没有定时器。等待期间谁也拿不到屏幕：进程 1 自己的屏幕和内核日志说明在等哪个任务（编号与种类），以及如何立即继续。该服务和其他所有服务照常运行，因此需要网络或驱动的任务能继续下去。等待期间合盖不做任何事，正如 logind 在一个动作进行中时忽略它。
- 服务在一个任务结束后立即启动下一个排队的任务，这个任务同样会被等待。没有任务运行满 1 秒后，关机照旧进行：结束一切（SIGTERM，2 秒后 SIGKILL）、卸载 ESP、根分区改为只读。
- **立即继续**，切断任务（日志会写明是哪个）：等待时按 Ctrl+Alt+Del（重启）、电源键（关机），或随时执行 `poweroff --force` / `reboot --force`（也可用 `-f`），无论是否在等待。这两个命令发送 systemd 用于立即关机和立即重启的信号 SIGRTMIN+14 与 +15，按编号即 48 和 49（glibc 的 SIGRTMIN 为 34，进程 1 所用的 musl 为 35）；关机的其余步骤不变。按键只有是新的一次按下才算再次请求：按住 Delete（会自动重复），或同一次按下在两个设备上被看到，距上一次不到 1 秒的，都不算。等待时再执行不带 `--force` 的 `poweroff` 或 `reboot`，或 home 的请求，不会强制继续；以最后一次为准决定是关机还是重启。
- **电量严重不足**时，有恢复设备就休眠，与没有任务时一样：任务在休眠镜像中，恢复后继续。没有恢复设备，或镜像写不进去时就关机，正在运行的任务**最多再等 60 秒**（屏幕上会说明）；到时切断任务，并记录 `POWER: the critical battery's power off waited 60 s; system job ID (KIND) still runs and is cut`。立即切断，pacman 或固件写入必然半途而废；无限等待，电池耗尽同样会切断它，而且根分区仍可写。已经在等待的关机也会得到同样的期限。
- 恢复时会重新启动固件和启动镜像：正在写它们的任务（`boot`、`kernel-select`、固件类任务）若在中途休眠，安全程度只相当于在那一刻断电。
- 等待屏幕是英文的：进程 1 只用 Terminus 字体绘制，其中没有中日韩字符（开机磁盘密码提示也是如此）。

## 锁屏

锁屏时不拒绝任务请求：队列不属于屏幕，能访问 socket 的只有容器之外的 root。锁屏下被冻结的程序不会发送请求；锁屏前请求的任务继续进行。

## 程序

- **settings**：软件包相关行（Tailscale、Clash、Tor、蓝牙、人脸及其 dlib、云盘、输入法与中文拼音、可选模块、指纹）、启动内核及其头文件、软件源与镜像、启动镜像都请求任务；相应的行显示其任务输出的最后一行，或在队列中的位置。关闭 settings 立即退出；再次打开时重新跟进它请求过且尚未结束的任务。
- **packages**：检查更新、应用更新、使宿主与清单一致、添加和移除宿主软件包、构建、回滚和移除容器。它的面板显示它最后请求的任务（再次打开时也是）；因源码签名密钥而停下的任务会列出这些密钥，`g` 把剩下的部分作为新任务请求。
- **firmware**：fwupd 的刷新、更新、校验、解锁和安装，安装 fwupd 或 flashrom，以及所有 flashrom 操作。
- **home**：队列（`J`）、停止任务（`K` 后按 Y），任务运行或排队时暂缓重启和关机。

## 清单

各程序启动的操作，以及是否作为任务：

| 位置 | 操作 | 任务？ | 原因 |
|---|---|---|---|
| settings | `locachy-pkg install/remove`、dlib 切换 | 是 | pacman 事务及其后的重建：数分钟 |
| settings | `locachy-pkg kernel select / headers` | 是 | pacman 事务和启动镜像 |
| settings | `locachy-repos apply --set`、`rank` | 是 | 改写 pacman 配置、检查声明的软件包、联网为镜像排序 |
| settings | `locachy-boot` | 是 | 写 ESP；pacman 的钩子也会运行它，不能与事务重叠 |
| settings | `locachy-tpm enroll`、Secure Boot 密钥、fwupd 报告、`tailscale up --qr`、howdy 及其摄像头、bluetui | 否 | 需要人在终端前（密码、二维码、人脸）；它们在 settings 之上的 term 中运行，属于其任务 |
| settings | `fprintd-enroll` | 否 | 需要手指放在读取器上，用 q 或 Esc 取消 |
| settings | `locachy-tpm clear` | 否 | 移除一个 LUKS 令牌，几秒钟 |
| settings | `tor-enforce`、`locachy-setup debug-link`、`nmcli`、`keytable`、各状态查询 | 否 | 短小的辅助操作 |
| packages | `locachy-pkg plan/apply/sync/install/remove`、`locachy-app build/rollback/remove` | 是 | 软件包事务、构建、网络 |
| packages | `locachy-pkg plan --cached`、`status`、`info`、`search`、`commands` | 否 | 查询 |
| firmware | fwupd 刷新、更新、校验、解锁、本地安装、版本安装 | 是 | 写入固件和下载 |
| firmware | 安装 fwupd 或 flashrom | 是 | pacman 事务 |
| firmware | SPI 探测、备份、校验、写入 | 是 | flashrom 操作芯片：绝不能同时两个 |
| firmware | `bios-set`，设备、版本、属性和编程器列表 | 否 | 一次 sysfs 写入，或查询 |
| 进程 1 | 启动时的 `locachy-tpm enroll --pending` | 否 | 进程 1 自己的，未改动 |
| locachy-bar | 托盘的 `locachy-app open/stop` | 否 | 容器生命周期，属于进程 1 的注册表 |

此前 packages 和 firmware 把任务放进根 cgroup 运行，不受任何管理；settings 在任务运行时扣住窗口的关闭。两者都已去除。

## 测试

- `tools/test-jobs.sh`（在 `test-interaction-regressions.sh` 中）：在用户命名空间中以 root 运行服务，使用私有的 `/run`、作为夹具的 cgroup 和对端 `/proc/PID/cgroup`，并以 `test-jobs-tool.sh` 代替各程序；分别以普通方式和 AddressSanitizer/UBSan 构建运行。检查种类及其参数、一次一个且按顺序、重复的排队请求只排一次、因密钥停下的状态、文件及其权限、任务比发起请求的客户端活得更久、移除和停止（SIGINT 发给步骤运行的程序、宽限期后杀死）、对端（容器、窗口、直接程序、用户）、服务被结束（SIGTERM）和被杀死后重新启动、服务不在时仍能看到队列。
- `tools/test-home.c`：列出队列，只有按 Y 后才停止或移除，任务运行或排队时拒绝重启和关机。
- `tools/test-init-shutdown.c`（在 `test-interaction-regressions.sh` 中）：用作为夹具的任务 cgroup 和状态文件测试进程 1：没有任务时立即进行；有任务时等待（屏幕、日志、无定时器的监视），看到下一个排队任务启动，最后一个结束 1 秒后才继续，并且到那时才结束一切；再次按 Ctrl+Alt+Del 和电源键、`--force`；电量严重不足时休眠，或最多等 60 秒；`link/poweroff` 和 `link/reboot` 在用户与 PID 命名空间中以 root 运行，由该命名空间的进程 1 检查各自发送的信号。
- `tools/test-settings-close.c`：有任务时 settings 立即退出，按种类和参数请求任务，再次打开时重新跟进其任务。
- `tools/test-packages.c`、`tools/test-firmware.c`、`tools/test-settings-*.c`：各程序使用队列替身（`tools/jobs-double.h`，或运行其工具替身的替身）。
