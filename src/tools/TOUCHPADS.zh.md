# 触摸板

[English](TOUCHPADS.md)

## 触摸板点击

**设置 → 输入和输出 → 鼠标和触摸板 → 触摸板点击**决定触摸板在什么情况下点击，保存为 `/etc/locachy/rc.conf` 中的 `touchpad_clicks`：

| 取值 | rc.conf | 轻触 | 按压表面 | 独立按键 |
| --- | --- | --- | --- | --- |
| 轻触和按压（默认） | 不设置 | 点击 | 点击 | 点击 |
| 仅按压 | `press` | 无 | 点击 | 点击 |
| 仅指点 | `pointing` | 无 | 无 | 点击 |

独立按键指 TrackPoint 的按键，或触摸板自带的按键，例如松下 CF-SV、CF-FV 圆形触摸板下方的按键。

### 实现方式

直连程序和共享显示都经过 libinput，配置方式相同：

- **轻触点击**对应 libinput 的 tapping 设置。`base/pointer.c` 在配置设备时读取 `touchpad_clicks`。共享显示方面，settings 在 labwc `rc.xml` 默认的 `<libinput><device>` 配置中写入 `<tap>yes</tap>` 或 `<tap>no</tap>`。labwc 只给每个设备匹配最合适的一个配置，单独加一个触摸板配置会让触摸板失去默认配置里的其他设置。labwc 只对能轻触的设备（即触摸板）设置 tapping。
- **仅指点：按压。** clickpad（内核 `INPUT_PROP_BUTTONPAD`）表面下只有一个按键 `BTN_LEFT`，libinput 没有关闭它的设置。点击方式 `none` 只去掉软件按键区，之后按压任何位置都是左键。labwc 手册说 `none` 不产生按键事件，但 libinput 1.32 实际并非如此。因此，settings 为每个现有的 clickpad 在 libinput 本地 quirks 文件 `/etc/libinput/local-overrides.quirks` 中写一节 `AttrEventCode=-BTN_LEFT`。libinput quirks 不能按输入属性匹配，所以这一节按 udev 类型、总线、厂商、产品和名称匹配该设备。libinput 打开设备时会去掉这个事件码，触摸板仍可指点、滚动和使用手势。
- **仅指点：点击方式。** settings 同时为 labwc 写入 `<clickMethod>none</clickMethod>`，`pointer.c` 也设置相同的方式，因为既然没有点击，软件按键区就没有用处。去掉后，从底部区域开始的手指可以立即指点。高度 50 mm 及以上的触摸板上，libinput 的拇指检测仍会暂时忽略在那里缓慢移动的手指。
- **独立按键照常工作。** TrackPoint 的按键属于 TrackPoint 自己的输入设备。即使硬件上接在触摸板上（Synaptics pass-through、RMI4 F03、Elantech），内核也会把它们转到 TrackPoint 设备。自带按键的触摸板有 `BTN_LEFT` 和 `BTN_RIGHT`，不是 clickpad，不会匹配任何一节，只有轻触点击被关闭。
- **例外：顶部软件按键。** ThinkPad *40 系列（`INPUT_PROP_TOPBUTTONPAD`）没有实体 TrackPoint 按键，它的 TrackPoint 按键是 clickpad 表面的一部分，选“仅指点”后这些按键也不再点击。

只要保持“仅指点”，这些 quirks 节就会保留，包括以前见过的 clickpad 的节。`settings --apply` 在开机和唤醒后运行，那时触摸板驱动可能还没完成识别，如果那时删除就会丢失这些节。第一次接入的 clickpad 会在下一次 `settings --apply` 或打开 settings 时得到自己的节。改选其他取值会删除所有这些节，文件其余部分（包括触摸板绕边滚动的那一节）保持不变。

libinput 无法解析的 quirks 文件会让所有 quirks 失效，包括 libinput 自带的。因此，设备名中解析器不能原样接受的字符，settings 都写成 `*`。

### 生效时间

- **直连程序：**下次取得指针时（回 home 后）生效。每个新的 libinput 上下文都会重新读取 rc.conf 和 quirks。
- **共享显示：**轻触和点击方式立即生效，因为 settings 会写 labwc 的 `rc.xml` 副本，labwc 收到 `SIGHUP` 后重新配置设备。libinput 每个上下文只读取一次 quirks，所以按压的去掉或恢复要到共享显示下次启动时才生效。

该行的提示会说明刚才的改动属于哪种情况。

### 测试

- `tools/test-settings-monitors.c` 检查 rc.conf 键、labwc 配置行和 quirks 节：只为 clickpad 写入、保留和删除。
- `tools/test-touchpad-clicks.sh` 不需要设备和 root，通过 `pointer.c` 运行真实的 libinput。在用户和挂载命名空间中，tmpfs 提供 clickpad、TrackPoint 和双键触摸板的 udev 与 sysfs 信息，evdev 事件通过管道送入。

两者都在 `tools/test-interaction-regressions.sh` 中运行。
