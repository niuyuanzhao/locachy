# 云盘

[English](CLOUD.md)

云盘是用 FUSE 挂载到 `~/Drives/<remote>` 的 rclone remote。各家后端、登录和令牌都由 rclone 负责；LOCachy 只管理挂载、显示状态、刷新文件夹和报告故障，没有针对具体服务商的代码。

## 挂载

`link/rclone-client` 是 `service_rclone` 服务。它用 `rclone listremotes --json` 列出 remote，为每个 remote 以用户身份（uid 1000，无附加组）运行一个带 VFS 写缓存的 `rclone mount`。

挂载必须传到宿主和所有正在运行的容器。不能用 `ip netns exec`：它会把新挂载命名空间里的所有挂载设为 slave，在那里做的 FUSE 挂载对宿主不可见。因此服务在自己的挂载命名空间里只把 `/` 设为 slave，保留单独共享的 `~/Drives`，私下绑定 `locachy-net` 的 resolver，再用 `nsenter --net` 进入该网络命名空间。没有 `locachy-net` 或其 resolver 时不挂载，而不是直接联网。`rclone config` 和 `--reconnect` 也因此在 `locachy-net` 中运行。

名字不能安全作为文件夹名的 remote，或者其文件夹已是挂载点、非空、或是符号链接时，跳过该 remote，不遮盖任何本地数据。U 盘服务（`drives`）启动时现在只接管真实的可移动块设备，绝不卸载云盘或其他不是它挂的东西。

## 状态

`/run/locachy/cloud/status`（root，0644）每个 remote 一行：名称、路径、状态（`mounting`、`ready`、`refreshing`、`failed`、`unmounted`）、上次成功刷新的时间、原因类别（`authentication`、`rate-limit`、`permission`、`network`、`attention`、`busy`）。rclone 的原始输出只在仅 root 可读的日志里；状态和通知都不带它。

每个挂载有自己的 rclone 控制（RC）socket，放在只有用户能进的目录。应用接触不到它。服务自己的控制 socket 只接受 root 和用户，只对已知挂载做两件事：刷新一个文件夹、卸载一个盘。

## 刷新

rclone 会缓存目录列表。重新读文件夹只能看到缓存而不是远端，所以单靠 F5 看不到别处的改动。在云盘文件夹里，F5、工具栏的“刷新”按钮、文件夹菜单的“刷新”、以及在列表顶部下拉，都会请服务用 rclone 的 `vfs/refresh` 刷新这一个文件夹（不递归），核对该文件夹的结果，然后重新读取。其他文件夹照旧重新读。

下拉刷新是手指手势：触屏上一根手指，或触摸板的手指滚动，且开始时列表在顶部。拉过三行后提示“松开即刷新”，抬起手指刷新一次。反向、第二根手指、捏合或失去屏幕都会取消。鼠标滚轮、小红点的中键滚动和拖动文件都不会触发。指针层只向请求的程序报告滚动来源及其开始、结束和取消。

files 在单独的进程里读取云盘文件夹：以用户身份、无网络、有时限，界面从不等网络。失败或超时保留原来的列表并给出提示。类型按文件名判断（如 KIO 对慢挂载的做法），画文件夹不会下载任何东西；不统计文件夹大小。在 `~/Drives` 里对云盘按 `e` 通过服务卸载；仍有待上传数据或打开的文件时保持挂载。

## 登录与通知

rclone 自己续期访问令牌，令牌到期本身不是重新登录的理由。只有 rclone 自己的 reconnect 提示和标准 OAuth 错误 `invalid_grant`、`invalid_token` 才把 remote 标为需重新登录。网络故障、401/403/429/5xx 和其他错误都不会报成登录问题；无法分类的故障显示“需要处理”。rclone 没有与后端无关的登录状态，所以不再多推断。

状态变化通过标准通知服务通知一次；bar 现在也在 root 的 session bus 上为宿主程序提供该服务。状态持续期间通知被替换而不重复，恢复后关闭。files 和设置显示同样的状态。在设置里，对需重新登录的 remote，在 remote 行按 Enter 运行 `rclone config reconnect`，否则运行 `rclone config`。不会自动登录、删除凭据或修改配置。

## 测试

- `tools/test-rclone.py`：用替身测试 supervisor（16 项）。
- `tools/test-rclone-fuse.sh`：在一次性用户与挂载命名空间里用真实 rclone 和 FUSE：传到宿主和挂载前已存在的命名空间、私有 resolver、刷新、状态、待上传写入和卸载。
- `tools/test-files.sh`：在临时 home 上用真实的文件夹 worker 和替身服务测试 files：places、读取文件夹、F5、下拉、未拉够与取消、小红点、失败、卸载；普通构建及 ASan/UBSan。
- `tools/test-pointer.c`、`tools/test-host-notify.c`、`tools/test-drives-state.c`：滚动生命周期、宿主通知、U 盘接管。

这些测试用本地替身 remote 代替云端后端。真实账号、真实过期的 OAuth 授权，以及在真机上用手指操作，仍需在机器上验收；见仓库的 `REGRESSIONS.md`。
