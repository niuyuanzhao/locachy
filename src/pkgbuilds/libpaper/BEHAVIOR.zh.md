# libpaper 生命周期补丁

[English](BEHAVIOR.md)

这是固定的 Arch 官方 `libpaper` 配方上的小源码补丁，不是替换 PKGBUILD。
保留上游依赖、源码、校验、构建及检查函数；本地 epoch 选择打补丁的库。

打包的 libpaper 2.2.8 实现违反 `paperdone()`“释放 paperinit 分配资源”的约定：
每条解析目录行泄漏 `getln` 缓冲，`paperdone` 不释放目录链表和名称。因此重复的
Ghostscript/libspectre 初始化会泄漏，与 LOCachy 渲染器独立。由自行分配的 HOME
配置路径改用借来的 `XDG_CONFIG_HOME` 时，旧所有权标志也未清除，可能误释放
环境变量指针。上游 v2.3.0 仍有相关链表、行缓冲清理遗漏，不能只升级版本就认定修好。

`lifecycle.patch` 在复制字段后释放各解析行，释放空 papersize 文件的行缓冲，
清理目录和诊断全局变量，并在关闭时复位路径所有权标志。不改变纸张尺寸、默认
选择规则，也不隐藏 sanitizer 诊断。

`tools/test-libpaper.sh` 在已安装候选上普通运行及开启 ASan/UBSan/LeakSanitizer
运行 `test-libpaper.c`：重复 init/default/done、重复 done、由自有 HOME 路径切换到
借用 XDG 路径、重新初始化后目录内容正确。同一测试在原版库上触发无效释放，
修复候选无抑制通过（`~/x230-dev/libpaper-lifecycle-probe/`，对照 `stock.log`、
`fixed.log`）。发布前，正常 prepared 包事务须把该候选与渲染依赖共同安装验证。

渲染器测试现在检查**每个**子进程退出，包括中间重启，不能再用
`waitpid(..., NULL, ...)` 丢掉 sanitizer 失败。源码级 API 和 Ghostscript/libspectre
最小实验确认问题来自库，不代表缓存根运行就是完整包事务验收。

参考：[libpaper](https://github.com/rrthomas/libpaper)、
[`paper.h` 生命周期约定](https://github.com/rrthomas/libpaper/blob/v2.2.8/lib/paper.h)、
[Arch 官方配方](https://gitlab.archlinux.org/archlinux/packaging/packages/libpaper)。
