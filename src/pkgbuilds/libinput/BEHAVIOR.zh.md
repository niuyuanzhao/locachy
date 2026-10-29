# LOCachy libinput 行为发布验证

`test-libinput.sh` 只验证已安装库的基本可用性。它不创建或读取输入设备，`gate.json` 也明确标注了这一范围。只有这份结果，不能发布使用 `libinput-circular-scroll-v2` 合约的版本。

发布前必须执行打过补丁的上游 litest 测试和 LOCachy 无盘 VM 指针测试。两者只能在一次性测试目标中创建 uinput 设备，绝不能对正在使用的真实显示环境运行。

## 软件包配方

跟踪式适配器读取已经锁定的 Arch 配方，在 `prepare()` 中先应用 `library.patch`，再应用 `circular-scroll.patch`，保留上游 `build()` 和 `check()`：

```sh
makepkg --verifysource
makepkg --syncdeps --cleanbuild --noconfirm
```

不要传入 `--nocheck`。构建和测试必须在一次性 Linux 测试目标中运行，不能在日用系统上执行。上游 `check()` 使用 `meson test -C build --print-errorlogs`。

## 上游专项 litest 验证

在 Linux 上构建已经应用补丁的源码树：

```sh
meson setup build -Ddocumentation=false -Ddebug-gui=false -Dtests=true
meson compile -C build
```

在一次性 VM 中，从同一棵构建树运行专项测试：

```sh
build/libinput-test-suite --filter-test='touchpad_circular_scroll_*' --filter-device='LITEST_PANASONIC_CF_FV1'
build/libinput-test-suite --filter-test='quirks_parse_circular_scroll' --filter-device='LITEST_MOUSE'
```

圆周测试覆盖默认无量纲环带边界、300ms 停留及其取消、双指水平和垂直滚动，以及三指滑动。`AttrCircularScrollRingWidth` 是 1 到 50 的无符号、无量纲整数，默认 25，表示所报告半径外侧的比例，不是物理距离校准。`AttrCircularScrollDwell` 是 0 到 2000 的无符号毫秒值，默认 300。quirks 测试覆盖这些参数的有效值和无效值。缺少测试运行器意味着发布前提不成立，不能退回仅验证库能加载的检查。

## LOCachy VM 验证

使用现有无盘 VM 脚本，指定实际构建的库和补丁中的 quirks：

```sh
LIBINPUT_SOURCE=PATCHED_SOURCE src/tools/test-pointer-libinput.sh KERNEL UINPUT_MODULE RESULT_DIRECTORY
```

`PATCHED_SOURCE` 中要有 Meson 的 `build/` 目录。`KERNEL` 是可读取的 Linux 内核镜像，`UINPUT_MODULE` 是与它匹配的 `uinput.ko` 或 `.ko.zst`，`RESULT_DIRECTORY` 不能已经存在。脚本启动带独立 devtmpfs 的 QEMU，并在 VM 中执行指针测试。它需要 KVM，不需要正在运行的 labwc。

所需证明包括圆周和 2FG 方法共存、停留前不滚动、停留后圆周滚动、第二指取消圆周候选、双指水平及垂直滚动，以及三指手势。指针测试检查组合方法、等待停留后才圆周移动，并验证圆周模式下双指两轴；上游专项用例另行检查边界和停留前后取消。脚本严格接收三个参数，可选 `LIBINPUT_SOURCE` 和 `PREPARE_ONLY=1`，使用匹配的内核和 `.ko`/`.ko.zst` 模块及新的输出目录。`PREPARE_ONLY=1` 只准备 VM，不证明运行时行为。

## 验收

发布报告必须同时包含合约编号为 `libinput-circular-scroll-v2` 的已安装库 `gate.json`，以及上游专项 litest 和指针 VM 的日志。仅有 smoke 检查结果不能证明 v2 行为正确。

候选证据：已完成 Linux 库和 test-suite 的完整构建；无盘 KVM 客体通过全部 18 项圆周用例及 quirk 边界用例，无跳过项。另一个指针层 VM 通过顺逆圆周、双指两轴双方向、普通手势及输入热插拔。日志必须与精确补丁哈希对应；这些结果仍不能单独代表真实设备手感或最终集成版本验收。
