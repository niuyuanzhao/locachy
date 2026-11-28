# 外观设置

在「设置 → 外观与应用 → 颜色」中，分别选择 **LOCachy Dark / LOCachy Light** 和 **重点色**。重点色的「方案默认」会随当前方案变化；预设或自定义 `#RRGGBB` 在切换深浅方案时保留。壁纸与这两个设置相互独立。

系统把选择写入 `/etc/locachy/rc.conf`：

```sh
color_scheme="dark"       # dark 或 light
accent_color="default"    # default 或 #RRGGBB
```

没有这两个键的旧配置继续使用原有深色外观。请通过设置页面应用变更；手动编辑后运行 `settings --apply` 才会应用已保存设置，单独编辑文件不会刷新所有使用方。

- LOCachy 窗口和共享桌面使用系统方案；变化的是界面颜色，不是原文档、PDF 标注、截图像素或终端程序明确指定的 24 位颜色。
- labwc 窗口装饰和菜单使用同一选择。每个新启动的 LOCachy 管理 foot 窗口得到新配色，不会改写 `foot.ini`；已经打开的 foot 窗口要重新打开。
- 管理的应用可通过各自的私有会话总线从 Settings portal 读取深浅方案和重点色，无需访问宿主配置文件或宿主总线。Wayland 实测中，GTK 4.24 实时跟随两项设置；libadwaita 1.10 实时切换深浅，并把重点色映射到自己的命名色。Electron 42 的深浅外观和网页 `prefers-color-scheme` 实时跟随，但没有测到自定义重点色继承。GTK 3.24 与默认配置的 Qt 5.15／6.12 Wayland 控件都没有跟随。这是特定工具包和版本的实测结果，不保证每个应用都一样；应用可选择自己的主题。
