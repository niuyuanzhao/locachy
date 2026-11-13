# 宿主程序作为受管理窗口

[English](WINDOWS.md)

view、files、settings、混音器、bluetooth、network 和 term 除了从 home 在直连屏幕上运行，也可以在共享桌面上各自成为一个窗口。窗口是同一程序的受管理实例，不是另一份实现：进程 1 像管理容器一样启动、冻结、停止它；它从不持有屏幕租约或输入设备；窗口开着时，共享桌面保持运行。

## 请求

`locachy-seat --window PROGRAM [TARGET]` 请求一个窗口，`locachy-seat --open [--own-tor] CONTAINER` 请求进程 1 在共享桌面上启动或置顶容器。顶栏运行它们（其启动器镜像 home 的三个分组，蓝牙开启时点击蓝牙区直接打开 bluetui，音量区打开混音器，右侧其余状态区打开 settings 的对应页面）；共享桌面上的容器通过 bar 的 xdg-open 打开文件时也走这条路：交给 view 的文档开一个 view 窗口，文件夹开一个 files 窗口，不再切回 home。

| PROGRAM   | TARGET                                              | 运行的命令 |
|-----------|-----------------------------------------------------|------------|
| settings  | 无，或 `network`、`power`、`devices`、`date-time`、`hardware` | `/usr/local/bin/settings [--page=PAGE]` |
| view      | 无（文件选择器），或文件的绝对路径                     | `/usr/local/bin/view [FILE]` |
| files     | 无（主目录），或文件夹的绝对路径                       | `/usr/local/bin/files [FOLDER]` |
| mixer     | 无                                                  | `/usr/local/bin/term /usr/bin/wiremix` |
| bluetooth | 无                                                  | `/usr/local/bin/term /usr/bin/bluetui` |
| network   | 无                                                  | `/usr/local/bin/term /usr/bin/nmtui` |
| term      | 无                                                  | `/usr/local/bin/term /usr/local/lib/locachy/user-shell` |

终端以 UID 1000 打开用户 shell，不提供管理员 shell。设置的网络页还会在 settings 自己受管理的 cgroup 内启动 nmtui 子终端窗口，等待关闭期间仍响应窗口事件。

启动器与 home 共用宿主程序分类，列出全部已登记程序，不再固定只列几个。Enter 或左键打开受支持的窗口；F 或右键经 home 打开其全屏入口。仅直连程序使用 home。重启、关机只打开 home 原有的确认框，不从启动器直接执行。这里的容器状态是 cgroup/ready marker 快照，并非 home 的完整进程 1 状态；容器按名称排序。RAM、swap、CPU 和温度是读数，不是按钮。

其他一律不接受：没有别的程序、没有调用方自带的参数、不接受任意 shell 命令。请求只走已有通道：客户端发到 labwc-control 的 socket（`/run/user/0/locachy-control.sock`，仅 root）；labwc-control 通过 seat 给它的 socketpair（`LOCACHY_SEAT_REQUESTS`）把字段交给 seat；seat 再通过自己的控制 socket 向进程 1 发送 `window PROGRAM [TARGET]`。客户端、seat 和进程 1 各自检查程序与目标；进程 1 还检查 view 的文件确实存在。锁屏期间进程 1 拒绝窗口请求。

## 生命周期

进程 1（`init/apps.c`，`apps_window_open`）只在内存中记录窗口，与容器并列：没有配置文件，也没有配方。每个窗口命名为 `window-PROGRAM-N`，同时也是它的 cgroup（`app_NAME`）、日志（`/run/locachy/apps/NAME.log`）和 Wayland app id。程序与直连屏幕上一样以 root 运行，带有：

- `LOCACHY_BACKEND=wayland`、`XDG_RUNTIME_DIR=/run/user/0`、`WAYLAND_DISPLAY=wayland-0`
- `LOCACHY_WINDOW_TITLE`、`LOCACHY_WINDOW_APP_ID`
- `LOCACHY_WINDOW_READY_FD=3`：第一个窗口完成配置并画出后写一个字节。在此之前窗口处于 `starting`；程序若先结束或先关闭该描述符，即视为失败（home 显示原因）。
- `LOCACHY_REQUEST_FD=4`：运行期间的请求（settings 的 `page NAME`）。
- 没有 `LOCACHY_CONTROL`：从不借用进程 1 的控制 socket。

窗口与容器一样等待共享桌面的 display-ready（开机后的第一个窗口），并计入共享桌面的状态：共享桌面会为窗口启动，且只要有窗口在运行就不会停止，即使没有容器在运行。

settings、混音器、bluetooth 和 network 各只有一个窗口：再次请求会把它调到前台，settings 的新页面以 `page NAME` 发给已开的窗口，而不是再开一份。view、files 和 term 每次请求开一个窗口（最多十六个窗口）。

关闭窗口（关闭按钮、Alt+F4）以 `K_CLOSE` 交给程序，程序走自己的退出路径：

- files 与 Ctrl+Q 一样退出（窗口的关闭按钮和 Alt+F4 也是），在提问或列表中也是如此（提问按“否”处理）。复制进行中会先询问：`y` 立即停止复制，`n` 复制完成后关闭，Esc 保留窗口。
- view 与 Ctrl+Q 一样退出；有未保存的修改时先询问保存还是放弃，询问期间窗口保持打开。
- settings 与 Ctrl+Q 一样退出，即使它的软件包、软件源或启动镜像任务正在运行或排队：这些是系统任务（`locachy-jobs`，见 [JOBS.zh.md](JOBS.zh.md)），在窗口之外运行，窗口关闭后继续进行；home 列出它们，settings 再次打开时会重新跟进。
- 混音器（`term`）像终端关闭时一样向其程序的进程组发送 SIGHUP（挂断），然后结束。

程序结束后，进程 1 结束它在 cgroup 中留下的一切（`cgroup.kill`）并忘掉它。home 像管理容器一样停止、冻结、恢复窗口：冻结经过剪贴板服务，后者与容器一样对 cgroup 设闸；停止会先解冻、保存剪贴板、向 cgroup 中的进程发 SIGTERM，最后 `cgroup.kill`。home 的停止仍是强制的；它不会波及窗口请求的系统任务，冻结窗口也不会冻结它们：这些任务在 locachy-jobs 下各自的 cgroup 中运行。files 是会清理的例外：复制中收到 SIGTERM 时，它先删除未完成的副本（见下文）。

## 从窗口打开文件

窗口中没有进程 1 可问，所以 files 打开文件的方式与 home 的文件管理器不同，做不到时会明确说明：

- 打开程序为已安装的文档查看器（`/usr/local/bin/view`）的文件，通过 `locachy-seat --window view PATH` 在单独的 view 窗口中打开，与顶栏一样直接运行该程序；
- 交给容器的文件发往顶栏的打开 socket（`/run/locachy/open/socket`，`with NAME TARGET`，即 xdg-open 的协议）。顶栏和 files 都把容器限制在共享文件夹内；
- 文件夹在窗口内浏览；
- 其他直连程序（edit、archive、term）被拒绝，并提示它们只能从 home 打开；
- 留给 home 的请求（`/run/locachy/files-request`、`open-request`）仍归 home 处理。

## 复制

files 把每个复制项先写到目标文件夹中的隐藏名（`.locachy-copy-PID-N`），完整后才改名，因此不完整的文件永远不会使用最终的名字。跨文件系统的移动也是这样的复制，源文件只在改名之后才删除。失败、被停止（Esc 会先询问）或被 SIGTERM 打断的项会被删除；已完成的项保留，可以撤销。在数据块之间 files 保持响应：绘制进度、接受新尺寸、读取 Esc 和关闭请求。只有 SIGKILL（home 停止给的 3 秒之后的 `cgroup.kill`）或断电才可能留下隐藏的 `.locachy-copy-*`。

不支持原子的不覆盖改名的驱动器（带 `RENAME_NOREPLACE` 的 `renameat2` 返回 EINVAL，rclone 云盘就是这样）由 files 在其中对自己的一个空隐藏文件试一次改名来识别，试完即删除。在这样的驱动器上，每一项改为直接写到它自己的名字，以独占方式创建（`O_EXCL`、`mkdir`），因此期间别人创建的名字仍然永远不会被替换。未完成的文件在关闭之前删除：rclone 在文件关闭时上传，从不上传打开期间已删除的文件，所以云端只会收到完整的文件。在这样的驱动器内部移动会被拒绝（“不支持在不覆盖已有文件的前提下在其内部移动”）：若改为复制再删除，会把全部内容重新下载并上传一遍。

## 尚未支持

- 把已开的窗口调到前台，依赖 labwc-control 按 app id 匹配 `window-PROGRAM-N` 名称。
- 程序需要 Wayland 后端（`LOCACHY_BACKEND=wayland`）；没有后端时窗口会明确失败，而不是去占用屏幕。

## 检查

- `tools/test-app-registry.sh OUT`：用程序和剪贴板的替身检查注册表：拒绝的请求、传入的环境变量和描述符、就绪、共享桌面状态、settings 单窗口及其页面请求、关闭、冻结/恢复/停止、失败、display-ready。
- `tools/test-window-routing.sh OUT`：针对替身 socket 的 `locachy-seat --window`、seat 的转发以及 labwc-control 的部分。
- `tools/test-files.sh OUT`：文件管理器的场景中包括以隐藏名复制（Esc、关闭、失败、SIGTERM、跨文件系统移动），以及针对 seat 客户端和顶栏打开 socket 替身的“从窗口打开”。
- `tools/test-settings-close.c` 和 `tools/test-term-close.c`（在 `test-interaction-regressions.sh` 中）：有无系统任务时 settings 的关闭——任务交给队列，再次打开时重新跟进；term 关闭时挂断其程序的进程组。
