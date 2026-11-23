# 全局快捷键与共用动作

[English](KEYS.md)

LOCachy 的按键分两类。

- **共用程序动作**（`base/key-action.c`，详述见 `KEYBOARDS.zh.md` 的「按键与动作」）：
  所有程序都通过 `key_action` / `key_list_action` 读取的固定按键，包括 `Ctrl+Q`
  退出、`Esc` 取消（绝不退出）、列表中 `q` 等同 `Esc`、`F1` 帮助，以及标准编辑和
  导航键。程序响应动作，而不是原始键值（`test-key-actions`、`test-key-literals.py`）。
- **可配置全局快捷键**：在 `/etc/locachy/rc.conf` 中以 `key_*` 保存。`base/keys.c`
  把它们解析成 `K_ACT_*` 动作码供直连帧缓冲程序和进程 1 使用；`settings` 将它们写入
  labwc 的 `/run/locachy/labwc-rc.xml`（模板为 `link/labwc-rc.xml`）供共享 Wayland
  显示使用。

## 全局搜索（`key_search`）

`key_search` 打开 LOCachy 的全局搜索（`/usr/local/bin/search`，见 `SEARCH.zh.md`）。
默认值为 `Super+Space`；旧的 `/etc/locachy/rc.conf` 缺少该键时，make.sh 会补写
`key_search="Super+Space"`。

| 层次 | 机制 | 当前状态 |
|---|---|---|
| 键表（`base/keys.c`、`base/include/base.h`） | `key_search` 解析为 `K_ACT_SEARCH`；与 `K_ACT_HOME`、`K_ACT_LOCK` 一样忽略长按重复。 | 已接入。 |
| 共享 Wayland 显示（`link/labwc-rc.xml`、`progs/settings.c`、`progs/seat.c`、`init/apps.c`） | `SEARCH-KEY` 绑定到 `/usr/local/bin/locachy-seat --window search`。`settings` 的 `labwc_line` 用 `key_search` 替换 `SEARCH-KEY` 并写入 `/run/locachy/labwc-rc.xml`；`seat` 接受 `"search"` 窗口请求，`init/apps.c` 将它映射到 `/usr/local/bin/search`，以置顶居中浮窗方式打开（`LOCACHY_WINDOW_FLOATING=1`）。浮窗打开时再按一次，会向它的请求描述符写入 `close`，因此同一按键可以切换关闭。 | 正常应用路径已接入。`seat` 的回退路径（`labwc_fallback_from`，仅在 `settings --apply` 失败时使用）把 `SEARCH-KEY` 替换为默认的 `W-space`（Super+Space）。 |
| 宿主帧缓冲（`init/init.c`、直连程序） | `K_ACT_SEARCH` 在焦点帧缓冲屏幕上打开 `/usr/local/bin/search`（进程 1），不依赖共享 Wayland 显示；锁定时或共享显示被占用时忽略。搜索界面内，同一按键即关闭。 | 已在进程 1 接入。其他帧缓冲程序都不分发 `K_ACT_SEARCH`。 |
| 重绑定（`progs/settings.c`） | `settings` 在一行中显示该键，与其他全局快捷键做冲突检查（`key_taken`），写入 labwc 的 `SEARCH-KEY`，并重载 labwc。 | 已接入：该行提供编辑（`choose_key`）与删除。否则请直接修改 `/etc/locachy/rc.conf` 中的 `key_search`，并运行 `settings --apply`。 |


## 其他全局快捷键（`rc.conf`）

输入法按键、Caps/Num Lock 以及共用程序动作表的完整说明见 `KEYBOARDS.zh.md`。

- `key_home`（默认 `Super+Esc Ctrl+Alt+Esc Ctrl+Alt+Super ThinkVantage`）：
  `K_ACT_HOME`，labwc 中为 `HOME-KEY`；冻结当前任务并回到首页。
- `key_lock`（默认 `Super+L`）：`K_ACT_LOCK`，labwc 中为 `LOCK-KEY`；锁屏。
- `key_terminal`（默认 `Ctrl+Alt+T`）：labwc 中为 `TERMINAL-KEY`；在共享显示上打开
  一个 foot 窗口。
- `key_ime`（默认 `Ctrl+Space`）、`key_ime_cn`、`key_ime_raw`、`key_ime_width`：
  输入法按键（labwc 中由 `IME-KEY` 消耗；共享显示由 `locachy-bar` 处理，帧缓冲由
  `base/ime.c` 处理）。
