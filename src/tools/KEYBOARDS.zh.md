# 键盘布局

[English](KEYBOARDS.md)

在 **设置 → 输入与输出 → 键盘** 中，**系统默认键盘布局** 决定没有单独指定布局的键盘
如何输入。其下各设备的布局是独立覆盖。这些是实体键盘的按键布局，与可选的中文
输入法分开。

系统默认用 US、内置键盘用日文布局时：

1. 给内置键盘明确指定日文布局，例如 `jp106`。
2. 将 **系统默认键盘布局** 设为 `us`。
3. 未单独配置的键盘保留 **跟随系统**；以后新接的键盘也默认 US，除非已为它保存了
   自己的布局。

明确选择的布局即使恰好与当前系统默认相同，也保留为明确指定。只有 **Delete**
或选择 **跟随系统** 才清除设备覆盖。设备覆盖记录在 `/etc/locachy/keyboards.conf`，
系统默认记录在 `/etc/locachy/rc.conf` 的 `keymap`。

系统布局选择器先生成替换键位表，再应用。生成、配置写入或文件替换失败时，旧布局
保持生效。成功后重读直连输入和共享显示，不需要重新编译系统。labwc 在重载配置时
读取标准的 `environment` 文件：layout、model、variant 来自新键位表，不相关的环境
变量和用户明确配置的 XKB options 保留。`/etc/vconsole.conf` 同步系统默认，并保留
其他设置。

initramfs 中是键位表和设备覆盖的副本。开机磁盘口令只有在下次生成引导镜像后才使用
新选择；运行时修改布局不会自行生成或替换引导镜像。

`test-settings-monitors.c` 覆盖系统默认与设备覆盖的区分、US 默认加明确 JIS 和跟随
默认的 USB、选择确认与取消、生成/配置/替换失败的回滚，以及 labwc 用户选项保留。
运行时键盘交接和焦点客户端输入由单独的输入测试验证；界面测试不证明交接安全。

## 锁定键与输入法

共享显示按成熟 Rime 前端的方式，向 Rime 传送布局解析的原生 XKB keysym、标准
Shift/Lock/Control/Alt/Super 修饰状态及释放事件。Num Lock 改变的是解析后的小键盘
符号（`KP_1` 或 `KP_End`），不把它的修饰位误当作 Rime 模式切换。JIS 的 Shift+英数
通过实际 XKB Caps Lock 状态变化识别，不再假定其未按 Shift 时就是锁定键。
各键盘共享锁定状态。

直连键位表现在包含 Shift/Caps/AltGr/Num 的十六种组合。旧八态表仍可读取，但不能
提供从未记录的 Num 状态符号。正常 LOCachy 重建会重新生成这些表，不需要手工
装包或修改键盘配置。

`tools/test-input-delivery.sh` 在真实 Rime 和无头焦点客户端中验证：JIS/US 实际大写
文字、关闭 Caps 后恢复普通组字、Num 开关后的小键盘数字/导航，以及 RAW/CN/EN
下英文空闲标点不变。`tools/test-caps-ime.c` 使用生成的 JIS/US 表和真实 Rime 验证直连路径。
这些测试不验证实体键盘 LED，也不替代硬件验收。

参考：[fcitx5-rime](https://github.com/fcitx/fcitx5-rime) 的 Rime 按键事件/修饰适配。
