# foot 通过 ssh 连接远端

foot 设置 `TERM=foot`（也可能是 `foot-direct`、`foot-extra`）。远端如果没有 foot 的 terminfo 条目，它的 shell 和行编辑器就会用错误的控制序列，屏幕会乱。macOS 上没有 foot 条目（用 `infocmp` 查过）。

LOCachy 安装 `/etc/ssh/ssh_config.d/50-locachy-foot-term.conf`（源文件在 `src/link/`）。本地 `TERM` 是 `foot`、`foot-direct` 或 `foot-extra` 时，ssh 客户端向远端发送 `TERM=xterm-256color`；其他值原样不变。本地 shell 自己的程序仍然看到 `TERM=foot`。

## 要求

- ssh 客户端是 OpenSSH 8.7 或更新版本，它会在 pty 请求里接受 `SetEnv TERM`。LOCachy 基础清单里的 `openssh` 包提供它。
- 远端 sshd 不用改，值由客户端发送。

## 按主机设置

`~/.ssh/config` 里的条目先于系统片段读取，所以装有 foot terminfo 的主机可以保留 `TERM=foot`：

```
Host my-host
    SetEnv TERM=foot
```

要在自己控制的主机上安装 foot 的 terminfo，把 `foot` 和 `foot-direct` 复制到那台机器的 `~/.terminfo/f/`（见 foot(1)）。

## 代价

远端程序看到的是 `xterm-256color`：256 色和 xterm 的按键集，没有 foot 独有的扩展。本地程序不受影响。

## 验证

`src/tools/test-foot-ssh-term.sh` 用 `ssh -G` 检查映射。它还会启动一个不设 `AcceptEnv` 的非 root OpenSSH 服务，确认远端 shell 收到的 `TERM`，覆盖 `foot`、`foot-direct`、`linux` 和主机级覆盖。

未验证：在 LOCachy 的 foot 窗口里通过 ssh 连真实远端时，fish、zsh 和 vim 的画面是否正常。这个测试只检查 TERM 的值，不检查画面。
