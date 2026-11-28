# 通过 SSH 运行 LOCachy 程序（终端后端）

[English](SSH-TUI.md)

基础库中会打开屏幕的程序（`settings`、`packages`、`firmware`、`files`、`archive`、`note`、`home`，以及构建时包含的 `view` 和 `edit`）可以改为直接画在终端会话里。这样即使物理显示正被扩展显示（`extdisplay`）占用，操作者仍能通过 SSH 维护机器。后端实现位于 `base/tty-backend.c`。进程 1（initramfs 中的静态 musl 初始化程序）以不含 `CONFIG_TTY` 的方式构建，其中没有这部分代码。

## 何时使用终端后端

- `LOCACHY_BACKEND=tty` 强制使用。若标准输入或输出不是可用终端，程序以退出状态 69 结束；二进制未以 `CONFIG_TTY` 构建时以 78 结束。它绝不会回退到 DRM 或输入设备。
- 未设置 `LOCACHY_BACKEND` 时，同时满足以下条件即自动启用：
  - 程序没有调用 `screen_direct_only()`；
  - 没有进程 1 控制套接字（未设置 `LOCACHY_CONTROL`）；
  - 标准输入和输出都是终端，并且设置了 `SSH_TTY` 或 `SSH_CONNECTION`，或标准输入是 `/dev/pts/*` 设备。
- 其他显式取值（`direct`、`wayland`）会使程序不走终端后端。
- `extdisplay` 和 `lock` 调用 `screen_direct_only()` 禁止自动选择终端后端，因此除非显式强制 `LOCACHY_BACKEND=tty`，它们都留在物理 DRM 屏幕上。
- 选择与带内探测在 `sudo` 下同样有效。`/etc/sudoers.d/10-locachy-user` 保留 `COLORTERM` 与 `LOCACHY_TTY_MODE`（不放宽受信任窗口启动器专用的 `LOCACHY_BACKEND`）；在 `sudo` 下 `ttyname_r(0)` 仍能识别 `/dev/pts/*`，并且带内探测直接通过 PTY 协商 Kitty/Sixel/CSI u。

## 三档渲染模式

后端打开时，通过 PTY 发送一次带内探测：Kitty 图形协议查询；窗口大小中没有像素尺寸时发送像素尺寸查询（`CSI 14 t`）；Kitty 键盘协议查询（`CSI ? u`）；并以主设备属性请求（`CSI c`）作为结束标记。探测不依赖 `TERM_PROGRAM`。

1. **Kitty 图形协议**（Ghostty、Kitty、WezTerm、Konsole）：`screen.c` 的完整软件帧缓冲（XRGB8888）原样呈现，曲线、图标、PDF 和 FreeType 中日韩文字都像素级准确。画布与终端行对齐（这是常见情况），因此只发送变化的单元格行条带；否则发送整帧。条带使用乒乓图像 ID、编入 `CONFIG_ZLIB` 时的 zlib 压缩，以及同步输出（`DEC 2026`）。HiDPI 缩放比例为单元格像素高度除以字体的文本高度（`TEXT_H`），按四分之一步进取值，范围 1.0 到 3.0。鼠标使用 SGR 像素模式（`DEC 1016`）。
2. **Sixel 图形**（iTerm2、foot、Windows Terminal、xterm）：由 DA 回复中的属性 4 识别。同一帧缓冲以 Sixel 带输出，带与整个终端行对齐，使用自适应调色板（不超过 256 色时无损，否则用 6×6×6 色立方加 32 级灰度），并做游程编码，包在 `DEC 2026` 中。鼠标使用 SGR 像素模式。
3. **ANSI 字符网格回退**（macOS 自带的 Terminal.app、未开启图形透传的 tmux、普通 SSH）：差分渲染的 UTF-8 字符网格，宽字符宽度与 `screen.c` 一致，包在 `DEC 2026` 中。当 `COLORTERM` 为 `truecolor` 或 `24bit`，或 `TERM` 含有 `kitty`、`ghostty`、`wezterm`、`foot`、`alacritty` 或 `direct` 时使用 24 位颜色；否则使用最接近的 xterm-256 颜色。鼠标使用 SGR 单元格模式（`DEC 1006`）。

受管 sshd（`tailnet-ssh-client`）没有设置 `AcceptEnv`，因此 `COLORTERM` 等客户端变量不会进入会话；`TERM` 总会到达，因为它随 PTY 请求一起发送。所以在该 sshd 下，字符网格模式的 24 位颜色取决于 `TERM`。

macOS 自带的 Terminal.app 既不支持 Kitty 图形协议，也不支持 Sixel，即使是 macOS 26 Tahoe（新增了 24 位真彩色支持）也是如此，因此会自动使用字符网格回退。macOS 上的 Ghostty、Kitty、WezTerm 和 iTerm2 则能呈现完整的像素帧缓冲。

## 环境变量

| 变量 | 作用 |
|---|---|
| `LOCACHY_BACKEND` | 可取 `tty`、`direct` 或 `wayland`；其他取值以退出状态 78 结束（见 [WINDOW-BACKEND.zh.md](WINDOW-BACKEND.zh.md)）。 |
| `LOCACHY_TTY_MODE` | 取 `auto`（默认）、`kitty`、`sixel` 或 `cell`；其他取值等同于 `auto`。若终端未报告像素尺寸或像素画布无法建立，像素模式回退为 `cell`。 |
| `LOCACHY_NO_SUDO` | 设为任意值（即使是空字符串）即可阻止 `settings`、`packages` 和 `firmware` 在终端中通过 `sudo` 重新运行自身。 |

## 通过 SSH 使用

- **提权**：以 LOCachy 用户通过 SSH 或 PTY 启动时，`settings`、`packages` 和 `firmware` 会经 `/usr/bin/sudo` 重新运行自身，因此管理操作无需手动加 `sudo`。密码是该账户自己的，与普通 `sudo` 相同。显式设置 `LOCACHY_BACKEND` 时不会重新运行（sudo 会丢掉这个变量），程序保持在启动它的账户上。
- **子命令**：`settings`（`in_term()`）和 `base_call()` 通过 `tty_backend_run()` 把终端交给子命令：退出备用屏幕、恢复普通终端模式，在同一 PTY 上运行命令，结束后重绘。
- **输入**：Kitty 键盘协议（`CSI u`）、xterm 的 CSI 与 SS3 序列、`modifyOtherKeys`、UTF-8 文本、Ctrl/Alt/Shift 修饰键，以及 40 毫秒的等待，用于区分单独的 Esc 与转义序列。鼠标点击、拖动和滚轮以 SGR 像素或单元格事件送达。
- **剪贴板**：`Ctrl+C` 通过 OSC 52 复制，最多 64 KiB 文本。终端必须允许 OSC 52 写入。
- **电源**：`reboot` 和 `poweroff` 需要时经 `sudo` 提权；若 `locachy-jobs` 系统任务正在运行则给出提示（init 会等待该任务），然后向 PID 1 发送信号：重启为 `SIGTERM`，关机为 `SIGUSR2`。`--force` 不等待，分别发送 `SIGRTMIN+15`（重启）或 `SIGRTMIN+14`（关机）。`home` 的重启/关机与 `packages` 更新页的 `R` 快捷键都走同一路径；图形窗口里的 `packages` 无法直接通向 PID 1，按 `R` 会提示改在 home 或经 SSH 重启。正在运行的 `extdisplay` 会一直保持运行，直到机器关闭。

## 测试

```sh
src/tools/test-tty-backend.sh [BUILD_CHECK_OUT_DIR]
```

`test-tty-backend.c` 通过 `tty_backend_use_fds()` 钩子验证带内能力探测（Kitty、Sixel、ANSI 字符网格与预输入保留）、Kitty 脏行条带与乒乓图像 ID 替换、Sixel 精确/回退调色板、真彩色/256 色字符网格差分渲染、键盘/鼠标解码、OSC 52 剪贴板以及 `tty_backend_run()` 挂起恢复。传入 `build-check.sh` 输出目录时，`test-tty-backend.sh` 还会在 Linux PTY 上运行真实编译出的程序，由测试本身回应终端探测：

- `settings`：ANSI 字符网格、Kitty 像素帧缓冲和 Sixel 像素帧缓冲三种模式；
- `settings`：Kitty 模式但终端未报告像素尺寸，必须回退到字符网格模式；
- `packages`：字符网格和 Kitty 像素帧缓冲两种模式；
- `firmware` 与 `home`：字符网格模式（`home` 在终端会话里按 `Ctrl+Q` 干净退出，在物理屏幕上仍保持不退出）。

每次运行都验证 `SIGWINCH` 实时缩放和 `Ctrl+Q` 干净退出，并检查 `/proc/<pid>/fd`，确认不会打开任何 `/dev/input/*` 或可写的 `/dev/dri/*` 设备。像素模式需要 `/usr/local/share/locachy/` 下的 Terminus 字体。主机上没有这些字体时，脚本会把 `/usr/share/kbd/consolefonts` 中的字体解压到私有挂载命名空间，不需要主机 root 权限。
