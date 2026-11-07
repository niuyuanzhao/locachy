# 把私人文件拖进容器

[English](DOCUMENTS.md)

容器在相同路径共享用户的文件夹（`user_dirs` 和 Drives），所以那里的文件以其自身
路径拖放。用户主目录中的其他文件是宿主私有的：任何容器都看不到。从 files 把这样的
文件拖进容器，要经过宿主的文档服务：文件成为打包的文档门户（xdg-document-portal）
中的一个文档，只有放置目标所在的容器可以读取，只读，直到该容器停止。

只有合成器知道放置落在哪里，而且只在放置的那一刻知道。接收方在放置之前就会读取拖放
的数据：Chromium 每次进入都读，Firefox 在 dragenter 时读。因此这些文档从拖放开始就
存在，但谁也不能读；labwc 在放行放置之前，先把它们授予放置目标。

## 1. 组成

| 部分 | 运行位置、身份 | 生命周期 |
|---|---|---|
| `locachy-docs`（`progs/locachy-docs.c`），即服务 | 宿主，root | rc.conf 的 `service_documents`（desktop 模块的；有 `/usr/lib/xdg-document-portal` 时 make.sh 会添加）：由进程 1 启动，结束 5 秒后再次启动。它与它启动的一切都在自己的 cgroup `/sys/fs/cgroup/locachy-docs` 中。 |
| 文档总线（`dbus-daemon`，`link/docs-bus.conf`） | 宿主，用户（uid 1000）；socket 为 `/run/locachy/docs/user/bus` | 由服务启动；只有该用户能连接，任何容器都没有这个目录 |
| `xdg-document-portal`、`xdg-permission-store`（xdg-desktop-portal 软件包，未打补丁） | 宿主，用户，在文档总线上 | 由总线按其自带的 service 文件启动；门户在 `/run/user/UID/doc` 挂载其 FUSE 文件系统（fusermount3，`auto_unmount`，没有 `allow_other`）；其数据在 `/run/locachy/docs/user/data`（文档都是临时的） |
| `locachy-docs --user`，即服务端 | 宿主，用户，不能获得新特权 | 由服务带着控制 socket 启动；是文档总线上门户的客户端 |
| 控制 socket `/run/locachy/docs/control` | 仅宿主，属 root（0600） | 由服务绑定，由服务端应答 |
| 容器的视图 | 容器中的 `/run/user/UID/doc` | `/run/locachy/docs/views/NAME`，由 `link/locachy-app` 绑定进容器 |
| `container-documents`（`progs/container-documents.c`） | 每个容器中，用户，在容器自己的总线上 | 照旧在那里持有 `org.freedesktop.portal.Documents`，并应答 `FileTransfer.RetrieveFiles` |
| files（`progs/files.c`、`base/wayland-data.c`） | 宿主，root（原生窗口） | 拖放的来源 |
| labwc 的放置闸门 | 宿主，root | `pkgbuilds/labwc-locachy` 的补丁（此处不描述） |

门户以用户身份打开 `/dev/fuse`（打开失败时 libfuse 没有后备方案），所以服务先等待该节点
变成 udevd 所设置的样子（0666，打包的规则）：开机时服务在 udevd 描述设备之前启动，于是它
像 seat 对待自己的设备那样，自己向 udevd 请求这一个设备。

总线或服务端结束时，服务结束其余部分（它的 cgroup），卸下视图并退出；进程 1 再次
启动它，它会重新挂上所有视图。门户离开总线时，服务端随之结束。

## 2. 一次拖放

1. **开始。** 项目拖动离开 files 的窗口。共享文件夹中的文件照旧以其路径拖放。对主目录
   中的其他每个文件，files 以用户身份打开它（`O_PATH`，不带 `O_NOFOLLOW`：即用户的路径
   所指的文件），把描述符发给服务，每条消息 16 个（`begin`、`begin-more`）。服务端逐个
   检查（跟随链接的 `O_PATH` 描述符、文件自己的名字、位于主目录中、是文件或文件夹），
   然后调用 `AddFull(fds, flags, "", [])`：不给任何应用权限，不持久，不复用（每次拖放的
   文档只属于这次拖放），文件夹带 `DIRECTORY`。它应答路径 `/run/user/UID/doc/ID/NAME`。
2. **提供。** files 用这些路径（以及共享文件的真实路径）提供 `text/uri-list`，并用同样
   路径组成的密钥（第 4 节）提供 `application/vnd.portal.filetransfer` 和
   `application/vnd.portal.files`，只允许复制。私人文件的真实路径从不提供。
3. **悬停。** 指针下的容器窗口可以读取列表或密钥。它知道了文档的路径和名字，但一个
   也打不开。
4. **放置。** labwc 扣住按键松开事件，向服务询问 `drop PID container NAME`（或
   `host`、`xwayland`），只有得到 `ok` 才放行放置。对于容器，服务端检查它正在运行（其
   cgroup `app_NAME` 中有进程）且视图已挂上，然后对每个文档调用
   `GrantPermissions(ID, APPID, ["read"])`。
5. **结束。** 目标读取了放置的内容时，files 发送 `end SESSION done`，否则发送
   `end SESSION cancelled`，然后关闭连接。labwc 只有在得到 `ok` 且指针仍在同一客户端
   上时才递交放置；等待超时（300 毫秒）或指针已移开时，它取消拖放。因此拖放可能在授予
   之后以取消结束：这时授予会被撤销。
6. **撤销。** 服务端监视每个被授予过的容器的 `cgroup.events`；cgroup 一旦变空（停止、
   崩溃），就删除该容器的文档。冻结的容器保留它们。

## 3. 控制 socket

`/run/locachy/docs/control`，`SOCK_SEQPACKET`，每条消息一个请求；由服务绑定，只属
root（0600）。服务端只接受 root 对端（`SO_PEERCRED`），即便如此也拒绝容器中的进程
（cgroup `app_NAME`；原生窗口 `app_window-*` 是 root 程序，可以接受）。它用对端的
pidfd 确认该 pid 仍是对端本身。

| 请求 | 来自 | 应答 |
|---|---|---|
| `begin\nNAME...`，每个 NAME 带一个 `O_PATH` fd，最多 16 个 | files | `ok SESSION\nPATH...`（每行一个路径，按顺序）或 `error WHY` |
| `begin-more SESSION\nNAME...`，带 fd | files，同一连接 | 同上；出错时删除该会话的全部文档 |
| `end SESSION done\|cancelled` | files，同一连接 | 无应答：这是该连接的最后一条消息（files 随即关闭连接）；cancelled 删除该会话的文档 |
| `drop SOURCE_PID container NAME` | labwc（comm 为 `labwc`） | 授予完成后 `ok`，或 `refuse WHY` |
| `drop SOURCE_PID host` | labwc | `ok`，不授予任何权限 |
| `drop SOURCE_PID xwayland` | labwc | `refuse`：无法区分 X11 程序 |
| `view NAME` | 宿主的 root（不是窗口） | `ok`，附带门户 `by-app/APPID` 的 `O_PATH` fd |

labwc 每个连接只发一个请求，并等待它唯一的应答。NAME 是指针下客户端最外层的
`app_NAME` cgroup，不论有没有安全上下文（所以容器在 `wayland-0` 上的
xdg-desktop-portal-wlr 也算该容器）。原生窗口（`app_window-PROGRAM-N`，进程 1 在
`wayland-0` 上运行的 root 程序，如 files）无论作为拖放的来源还是目标，都是宿主客户端：
`drop SOURCE_PID host`，不授予任何权限；`drop SOURCE_PID container window-...` 会被拒绝。
没有容器以 `window-` 开头命名（link/locachy-app 拒绝这样的名字）。

放置所对应的会话，是 SOURCE_PID（files 连接的对端）开始的、尚未结束也未被放置过的
最新会话。被拒绝的放置会立即删除其文档（随后 labwc 取消拖放）。在 `end` 之前关闭的
连接，等于取消其会话。取消一个已授予容器的会话时（`end cancelled`、连接关闭、某次授予
失败、容器结束），先逐个撤销授予（`RevokePermissions`），再逐个删除文档。

`end done` 之后：授予容器的文档保留到该容器的 cgroup 变空；放到宿主客户端的文档保留到
门户退出（临时的，不在任何视图中）；从未被放置的拖放，其文档被删除。

容器 NAME 的应用 id 是 `org.locachy.app.` 加上 NAME，其中 `_` 写作 `__`、`.` 写作
`_d`，NAME 以数字开头时前面再加 `_`（`progs/docs-proto.h`）：对门户是有效的应用 id，
每个名字各不相同。

## 4. 密钥与容器中的 FileTransfer

密钥是 `LOCACHYDOC` 加上各路径（以 NUL 分隔）的无填充 base64url：没有 `:`（Firefox
会把能解析成 URI 的数据当作别的东西），纯 ASCII。files 写出时带结尾的 NUL，与 GTK 4
相同。

每个容器总线上的 `container-documents` 增加了 `org.freedesktop.portal.FileTransfer`
（版本 1）。`RetrieveFiles(key, {})` 自行解码密钥并返回路径（绝对路径、UTF-8），否则
返回 `InvalidArgument`；它不授予任何权限，也不向宿主请求任何东西：容器里编造的密钥，
只是列出一些该容器本来能读或不能读的路径。`StopTransfer` 成功；`StartTransfer` 和
`AddFiles` 返回 `NotAllowed`：容器不能作为文档的拖放来源。Documents 接口保持原样：
`GetMountPoint` 为 `/run/user/UID/doc`，所有修改都是 `NotAllowed`。

宿主程序（`host="1"`）也运行 `container-documents`：它自己的文档门户会挂载在它的视图
上面。

## 5. 每个容器的视图

门户的挂载只向容器展示 `by-app/APPID`：该应用 id 可读的文档。它的 FUSE 挂载属于用户
且没有 `allow_other`，所以 root 无法在其中查找路径。因此服务：

1. 让 `/run/locachy/docs/views` 成为一个单独的、共享的 tmpfs，并在其中建立
   `views/NAME`（由 `link/locachy-app` 在容器进入其 cgroup 之前运行
   `locachy-docs --attach NAME`，或由服务启动时对每个 `views/NAME` 进行）；
2. 向服务端请求（`view NAME`）以用户身份打开的 `by-app/APPID` 的 `O_PATH` fd；
3. 克隆该 fd 的挂载树（`open_tree(OPEN_TREE_CLONE)`），把克隆设为 private，再挂到
   `views/NAME` 上（`move_mount`），取代那里原有的内容。

`link/locachy-app` 把目录 `views/NAME` 本身绑定到容器中的 `/run/user/UID/doc`，作为
视图挂载的 slave（`docs_bind`），而不是当时挂在它上面的视图：那样的副本在服务再次启动后
会留下来，成为失效的挂载。它在仅含视图挂载本身的绑定中找到该目录（在服务的锁下），并依据
挂载表作出判断（`docs_select`）：root 无法 stat 已挂上的视图。启动结束后，视图会被重新
挂上（`docs_reattach`，在启动器进入 `app_NAME` 之前派生），这次挂载会到达容器；之后的每次
卸下和挂上（服务再次启动）也一样。`app_NAME` 以 0755 创建，与进程 1 创建其窗口的方式相同：
服务端以用户身份读取它的 `cgroup.events`。宿主程序在自己的运行时目录中得到
同样的视图，并且 `/run/locachy/docs` 被一个空 tmpfs 遮住：文档总线上的任何对端都能
授予任何文档。

没有视图时（服务未运行、门户未安装），容器的 `/run/user/UID/doc` 是空目录，放到它上面
的放置会被拒绝。

## 6. 失败情形

| 情形 | 结果 |
|---|---|
| 拖放开始时服务未运行 | 不拖放；files 提示文档服务没有运行。 |
| 门户拒绝某个文件（`AddFull` 失败）、文件被改名、FIFO、主目录之外的文件 | 不拖放；什么都不留下（全有或全无）。 |
| 放到 Xwayland、放到没有视图或未运行的容器、授予失败 | `refuse`；文档被删除；labwc 取消拖放。 |
| labwc 没有及时得到应答（300 毫秒），或指针离开了目标 | labwc 取消拖放；files 发送 `end cancelled`；已做出的授予被撤销，文档被删除。 |
| Esc、放到空处、目标拒绝 | wlroots 取消；`end cancelled`；删除。 |
| files 在拖放中结束 | 连接关闭；删除。 |
| 容器停止 | 其文档被删除。 |
| 门户或总线结束 | 服务结束并再次启动；文档消失（临时的）；视图重新挂上。 |

## 7. 隔离、功耗、速度、安全

- **隔离。** 容器只看到自己的 `by-app` 视图，其中只有放给它的文档。容器里没有任何宿主
  总线，宿主也不解析容器总线上的任何内容。服务、总线、门户和服务端在同一个受管 cgroup
  中，一起结束。
- **功耗。** 一切都等待事件：控制 socket、D-Bus、FUSE、`cgroup.events`（inotify）。
  没有轮询。
- **速度。** 拖放开始时每 16 个文件一次门户往返；放置时，labwc 放行之前每个文档一次
  `GrantPermissions`。
- **安全。** 新增的暴露面：门户的 FUSE 服务端，每个容器都能通过自己的视图访问（与
  Flatpak 相同）；仅 root 可用的控制 socket；属于用户、从不进入容器的文档总线。授予都是
  只读且临时的。

## 8. 检查

- `tools/test-docs.sh 新目录`：`progs/docs-proto.h`（密钥双向转换，覆盖每个字节；拒绝
  畸形密钥；应用 id 及其转义；区分对端），然后在用户命名空间中以 root 身份、在私有总线
  上用替身门户测试服务端（普通与 ASan/UBSan 两种构建）：非 root 对端被拒之门外；只接受
  comm 为 `labwc` 的放置；`AddFull` 不带应用和权限、文件夹带 `DIRECTORY`；`end cancelled`；
  连接关闭；放到宿主；跨 `begin-more` 的全有或全无；拒绝的描述符（改名、主目录之外、
  `O_NOFOLLOW`、非 `O_PATH`、FIFO）；拒绝的放置（Xwayland、无视图、未运行、第二次放置、
  以容器名义出现的原生窗口）；`end` 无应答；为容器的应用 id 授予；容器运行或冻结时保留文档，cgroup 变空后删除；授予后又取消（或
  连接关闭）时撤销并删除；`view NAME`。
- `tools/test-docs-portal.sh 新目录`：服务本身配合打包的门户和权限存储，运行在用户、
  挂载和 PID 命名空间中（用户的进程保留该命名空间的 `CAP_SYS_ADMIN`，因为 fusermount3
  在那里不是 setuid）：控制 socket 只属 root；root 无法查看用户的 FUSE 挂载；经由 `view`
  和 `open_tree`/`move_mount` 挂上的视图到达先于它启动的“容器”挂载命名空间；
  link/locachy-app 自身的视图选择（`docs_select`，从中引入）能选中 root 无法 stat 的已挂上
  视图；在服务运行时启动的容器（`docs_bind`、`docs_reattach`）能得到其启动后挂上的视图；
  文件和文件夹在放置前不可读，放置后可读、只读，且不出现在另一个容器的视图中；cgroup
  变空时删除；服务结束一切，再次启动后重新挂上视图，并到达两个正在运行的容器；授予后又
  取消时不再可读。这里只是命名空间的 root：真正的 root 见 `tools/test-docs-view.sh`。
- `tools/test-docs-view.sh [NAME]`：仅在虚拟机中，以运行中系统的真正 root 执行：
  `locachy-docs --attach` 为 NAME 挂上的视图（用户的 FUSE 挂载）拒绝 root 的 stat，而已安装的
  locachy-app 的 `docs_select` 仍然选中它。
- `tools/test-files.sh`：files 一侧（见 [WAYLAND-DATA.zh.md](WAYLAND-DATA.zh.md)）。
- `tools/test-container-documents.sh`：用 coreutils 的 base64 生成的密钥调用
  `RetrieveFiles`、错误的密钥、`StartTransfer`、`AddFiles`、`StopTransfer`。
- `tools/test-drop-gate.sh`（labwc，在虚拟机的闸门中）：原生窗口作为拖放的来源和目标，以及
  以容器名义出现的窗口。

这些检查不覆盖：服务以 LOCachy 真正的 root 运行（它的 cgroup、
`/sys/fs/cgroup/app_NAME`、`link/locachy-app` 启动真实容器），`test-docs-view.sh` 所测
部分除外；以及浏览器读取被放置的文档。
