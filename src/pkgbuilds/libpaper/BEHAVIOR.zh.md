# libpaper 生命周期补丁

[English](BEHAVIOR.md)

`libpaper` 软件包在 Arch 官方 `libpaper` 配方基础上应用 `lifecycle.patch`。

libpaper 2.2.8 的 `paperdone()` 未释放目录解析行缓冲与目录链表，且从 `HOME` 路径切换到 `XDG_CONFIG_HOME` 时未复位路径所有权标志。`lifecycle.patch` 在复制字段后释放行缓冲、释放空行缓冲、在 `paperdone()` 中清理目录与诊断全局变量，并复位路径所有权标志。

## 测试

```sh
src/tools/test-libpaper.sh
```

参考：[libpaper](https://github.com/rrthomas/libpaper)、[`paper.h`](https://github.com/rrthomas/libpaper/blob/v2.2.8/lib/paper.h)、[Arch 配方](https://gitlab.archlinux.org/archlinux/packaging/packages/libpaper)。
