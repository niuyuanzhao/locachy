# Xwayland

显示模块使用 labwc 内置的 Xwayland。全系统共用一个 labwc 和一个 X 服务，不会为
每个应用另起合成器或 X 服务。`xorg-xwayland` 和 `xorg-xhost` 属于显示模块的软件包。
支持 Wayland 的程序继续使用 Wayland。

## 会话

`/etc/locachy/labwc/rc.xml` 设置
`<core><xwaylandPersistence>yes</xwaylandPersistence></core>`（labwc-config(5)）：
Xwayland 随共享显示启动，并在共享显示运行期间一直运行。labwc 每次启动 Xwayland
时都会运行 `/etc/locachy/labwc/xinitrc`（labwc-config(5)），此时 `DISPLAY` 为 labwc
实际选择的显示编号（不假定为 `:0`）。该脚本：

1. 把该显示的文件系统套接字设为所有人可连接（0777），与 X 服务自己创建的套接字一致。
   wlroots 按 labwc 的 umask 创建它（0755），只有 root 能连接；容器和宿主程序各有
   自己的网络命名空间，只能使用这个套接字，用不到抽象套接字；
2. 用 `xhost +si:localuser:USER`（xhost(1)）把应用用户（UID 1000）加入 Xwayland 的
   访问控制列表。Xwayland 本身只接纳运行它的用户，即 root。不添加任何主机或网络条目，
   也从不关闭访问控制：决定谁能连接的是这张列表，而不是套接字权限；
3. 然后把显示编号写入仅 root 可读的运行时文件 `/run/user/0/locachy-x11-display`。
   文件为空表示 X11 不可用，Wayland 不受影响。

访问控制列表属于正在运行的 X 服务。常驻设置让它在整个会话中保持；如果 Xwayland
仍然退出（崩溃），labwc 会在同一显示编号上重新启动它，并再次运行 xinitrc。

实测过的其他做法（Xwayland 24.1.14，labwc 0.20.2）：

- labwc 的默认方式是按需启动的 Xwayland：第一个 X11 客户端连接时启动，最后一个
  客户端离开 10 秒后退出。触发启动的那个客户端会被拒绝，无论是首次启动、空闲退出后
  还是崩溃后都一样：它在 xinitrc 加入用户之前就已连接。之后的客户端可以连接。
- `/etc/X0.hosts`（Xserver(1)）会被读取：`local:` 一行会出现在列表中，但 UID 1000
  仍被拒绝。`si:localuser:USER` 一行根本不会进入列表（服务器解释地址的值是
  “类型 NUL 值”，见 os/access.c 的 siCheckAddr，一行文本无法包含 NUL）。此外该文件
  对应固定的显示编号，而 labwc 自己选择编号。

`locachy-seat` 启动 labwc 时不带 `DISPLAY` 和 `XAUTHORITY`，在每个会话开始和结束时
删除该记录文件，并在记录出现后向进程 1 发送 `display-ready`（仅在已安装 Xwayland
时等待；10 秒后不带 X11 继续并记录原因：这一回退路径未经实测）。Xwayland 是 labwc
的子进程，位于共享显示的控制组，不在任何应用的控制组中。

## 容器

`locachy-app` 读取并验证记录（`:N`），不把其内容当作 shell 代码执行；如果该显示的
套接字不存在（Xwayland 正在重启），程序会不带 X11 启动并给出提示。只有配置中写了
`x11="1"` 的程序才得到 X11（原因见下面的安全边界）；其他程序只用 Wayland，没有
`DISPLAY`，也没有 X11 套接字。这样的容器收到 `DISPLAY`，并在自己私有的
`/tmp/.X11-unix` 中只读绑定这个显示的套接字；不接收宿主 `/tmp` 的其他内容、其他显示、
TCP 访问或 `XAUTHORITY`。写了 `x11="1"` 的宿主程序（`host="1"`）收到相同的
`DISPLAY`。应用仍各自拥有受管理的进程树以及冻结、关闭控制。

## 安全边界

连接同一个 X 服务的 X11 客户端**彼此之间没有隔离**：能连接的客户端“可以完全支配
屏幕”（Xserver(1)），包括其他 X11 窗口、它们的输入以及 X11 选区，而这些是受限的
Wayland 客户端无法触及的。容器的文件系统隔离不会改变这一点。支持 Wayland 的程序应
使用 Wayland；受限的 Wayland 套接字 `locachy-0` 保持不变。

## 剪贴板

宿主剪贴板（`clipboard`）会立即读取每个新选区，并以自己的名义重新提供。Xwayland 的
选区桥接（wlroots 的 xwm）把 X11 复制转换为 Wayland 选区，并为宿主剪贴板提供的内容
接管 X11 选区。实测结果：

- 容器中的 X11 复制会进入宿主剪贴板，其他 X11 客户端可以粘贴；之后该容器不再拥有
  这个选区；
- 用 `locachy-app freeze` 冻结（此时它无法应答）或关闭后，容器复制的内容仍留在宿主
  剪贴板中，X11 客户端可以粘贴；
- 宿主剪贴板仍在读取一个慢速 X11 复制时发起的 `locachy-app freeze` 会先等待传输完成
  （它的同步步骤），再冻结。

**缺口。** labwc-locachy 的冻结屏障（`clipboard freeze` 捕获内容到写入冻结之间的
控制组闸门）只覆盖 Wayland 客户端。xwm 通过它自己的 X 连接转发 X11 复制（剪贴板用
不带 Wayland 客户端的 `wlr_seat_request_set_selection`，主选区直接用
`wlr_seat_set_primary_selection`），闸门无法把它归属到容器的控制组。测试中人为放大
这一时间窗口后实测：

- 容器在冻结的同步与提交之间做的 X11 复制会被接受。闸门测试
  （`tools/test-clipboard-guard.sh`）实测：闸门打开期间，受管控控制组的 Wayland 复制
  会被拒绝；
- 如果提交时容器被冻结，而宿主剪贴板仍在读取这次复制，读取就无法完成：冻结后立即显示
  “传输未完成”，恢复后（2.5 秒后）显示“传输失败”。冻结期间 X11 粘贴得不到内容，恢复
  后又能得到该复制内容。宿主剪贴板能正常接收下一次 X11 复制。

实际使用中，这个窗口只是一次冻结的同步与写入冻结之间的时间（其长度未经实测），容器
必须恰好在这段时间内复制。要消除它，需要修改 labwc-locachy 的闸门
（`pkgbuilds/labwc-locachy`），本次修改没有做。

## 验证

不需要显示服务：

```sh
python3 -B tools/test-xwayland.py
```

在已构建、安装显示模块并创建应用用户的系统上，以 root 运行：

```sh
bash tools/test-xwayland.sh
```

它从本源码树构建 `locachy-seat` 和 `clipboard`，使用本源码树的 labwc 配置，用替身
进程 1、无头 labwc 和直接使用 XCB 的客户端运行，处于独立的挂载和进程编号空间中，
不使用真实输入设备，也不使用物理屏幕。输出行有 PASS、FAIL，以及表示上述实测限制的
GAP。集成测试失败或尚未运行，均不等于部署成功。
