# Rescue 模式

[English](RESCUE.md)

Rescue 是安装介质中的独立程序，与 Terminal 分开。它有自己的屏幕、磁盘和网线状态、提示、输出区以及清理流程。不通过 shell 启动，不运行安装器，也不会自动修复系统。普通 Terminal 仍然是 live 系统中的独立 root shell。

## 进入和退出

安装器菜单单独提供 **Rescue mode**，UEFI 和 BIOS 菜单也提供 **LOCachy Rescue**。它们沿用 archiso 的 kernel、initramfs 和介质搜索选项，并添加 `locachy.mode=rescue`。Rescue 启动时直接在 tty1 运行 `locachy-rescue.service`，不会进入 getty 登录或安装器；其他启动项保持普通安装器行为。

Rescue 启动时不会解锁磁盘，也不会启用远程访问。用键盘或指针选择操作。状态、可选择的操作输出和控制区分开显示。Tab 切换区域；方向键、滚轮、PgUp/PgDn 滚动。y/Ctrl+C 复制选中文本，a 复制 SSH 连接信息，f 复制主机密钥指纹。

键盘布局操作可以选择 US、Japanese JIS 或标准 console/XKB 布局名，使用已安装的 `keytable`。布局表放在 `/run`，只作用于 Rescue 及其可选的目标终端，不改变已安装系统的设置。恢复口令默认隐藏；显示口令必须显式操作。

退出时停止调试线服务，卸载本程序拥有的文件系统并关闭本程序拥有的映射。资源忙或已变化时不会强制删除：清理会明确失败，Rescue 保持打开以便再次尝试。退出目标终端会回到 Rescue。从安装器菜单打开时，成功退出后回到该菜单；从专用启动项进入时，保留“救援会话已关闭”完成页，可选择新会话、重启或关机，后两者必须按 Y 明确确认。不会自动打开 Terminal 或安装流程。

## 解锁已有系统

1. 重新扫描并按设备、大小和 UUID 选择加密 root。重复 label 或 UUID 不会静默选择第一个设备。
2. 在本地输入 LUKS 恢复口令。`cryptsetup` 通过 stdin 接收原始字节，并使用 `--key-file=- --disable-external-tokens --readonly`，不使用 TPM token。口令不会放进参数、环境变量、日志、磁盘 key 文件或 ESP。
3. 程序核验 LUKS2 `locachy-crypt`、ext4 `locachy-root` 以及已挂载系统的身份。root 以 `ro,noload` 挂载，不回放 journal。唯一关联的 ESP 以只读方式挂到目标的 `efi` 目录。ESP 不安全、占用中或不明确时会报告原因而不是猜测；root 仍可只读检查，但会阻止写修复。
4. 关联的加密 swap 单独用同一口令核验。Rescue 不激活 swap、不恢复休眠，也不启用 discard。两个加密设备不意味着必须弹出两次独立口令提示。

**Enable repair** 是单独的确认流程。Rescue 再次要求输入恢复口令，关闭只读资源，重新核验相同身份，然后以读写方式重新打开 root 和已识别的 ESP。只有此授权之后，**Target terminal** 才会运行 `arch-chroot`。它本身不会更新包、重建启动文件、修改 keyslot 或注册 TPM。第二次口令错误会让目标保持关闭，不会静默变成可写。

程序在 root-only 的 `/run/locachy-rescue/storage.json` 中记录随机 session 映射名、DM UUID、设备号和内核 mount ID。它不会接管已有映射，也不会卸载其他程序的 nested mount。关闭目标不会影响调试线服务；停止调试线也不会关闭目标。这些是运行安全措施，不是限制已获信任的 root 管理员。

## 认证调试线

选择 **Start debug link**，选择物理有线网口，然后设置一次性的 session 密码并再次确认。这不是 LUKS 口令，也不是任一系统的 root 密码。密码必须为 12–256 字节，不能含 NUL 或换行。

选中的网口会暂时退出 NetworkManager 并进入专用 network namespace。它只有 IPv6 link-local 地址，没有默认路由、DNS 或转发。其他网口不会被重新配置。Rescue 保存网口身份以及 NetworkManager 状态，以便恢复。

独立的 OpenSSH 服务只在该 namespace 的 2222 端口监听。它使用自己的配置、内存中的 Ed25519 主机密钥和私有 shadow 视图；不会修改 live 或目标系统的普通 root 密码。密码认证必需；转发、agent/X11/tunnel 和用户 rc 文件全部禁用。普通 SSH 服务保持屏蔽。远程访问授予 root Rescue 管理权限，包括 SSH 命令和 SFTP 传输；它不是只读安全沙箱。

屏幕显示地址、端口和 SHA-256 主机密钥指纹。在控制端使用**控制端本地网口**作为 IPv6 scope：

```sh
ssh -S none -p 2222 root@fe80::ADDRESS%LOCAL_INTERFACE
sftp -P 2222 'root@[fe80::ADDRESS%LOCAL_INTERFACE]'
```

将占位符替换为屏幕显示的地址和控制端的网口名，而不是 Rescue 机器的网口名。在接受主机密钥前核对 Rescue 屏幕上的指纹。每次 session 都会生成新的主机密钥；不要关闭主机密钥验证，也不要盲目删除旧密钥。SSH 提示时输入这次的临时 session 密码。

**Stop debug link** 会终止服务的完整进程树，删除 network namespace 和凭据，并恢复被选中的网口。服务最长运行一小时。UI 和调试线服务是分开的：停止调试线不会退出 Rescue，也不会关闭目标。

## 组件和检查

- `progs/rescue.c`：独立的屏幕/输入前端和固定 helper 请求。
- `installer/locachy-rescue`：动作路由和结构化合并状态。
- `installer/locachy-rescue-storage`：磁盘身份、解锁和资源归属。
- `installer/rescue-link.py`：显式调试线 session 和 OpenSSH 生命周期。
- `installer/locachy-rescue*.service`：直接 Rescue 启动以及独立的调试线 cgroup。
- `installer/rescue-boot-entries.py`：从打包模板生成启动项；上游模板变化会明确失败。

`tools/test-rescue.c` 使用可执行 helper double 驱动真实 UI 循环，覆盖精确 argv/stdin、取消、畸形回复、超时升级、输出、布局变化和清理拒绝。`test-rescue-{dispatch,storage,link}.py` 覆盖后端契约；`test-rescue-link.py --linux-auth` 在私有 Linux namespace 中使用真实 OpenSSH。这些检查不能替代真实 LUKS/systemd VM 或完整 ISO Rescue session 验收。任何测试都不授权修改实体安装的磁盘、TPM、固件密钥或启动状态。
