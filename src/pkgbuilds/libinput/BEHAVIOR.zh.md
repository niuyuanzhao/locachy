# LOCachy libinput 补丁

[English](BEHAVIOR.md)

`libinput` 软件包基于 Arch 官方配方构建，在 `prepare()` 中应用 `library.patch` 和 `circular-scroll.patch`。

## 圆周滚动

`circular-scroll.patch` 为触摸板提供外圈圆周滚动支持：

- `AttrCircularScrollRingWidth`：无符号整数 `1..50`（默认 `25`），表示外侧环带占触摸板半径的百分比。
- `AttrCircularScrollDwell`：无符号整数 `0..2000` 毫秒（默认 `300`），进入外圈后触发圆周滚动所需的停留时间。
- 双指水平／垂直滚动和三指手势与圆周滚动并存。

## 测试

上游 litest 测试：

```sh
meson setup build -Ddocumentation=false -Ddebug-gui=false -Dtests=true
meson compile -C build
build/libinput-test-suite --filter-test='touchpad_circular_scroll_*' --filter-device='LITEST_PANASONIC_CF_FV1'
build/libinput-test-suite --filter-test='quirks_parse_circular_scroll' --filter-device='LITEST_MOUSE'
```

无盘 VM 指针层测试：

```sh
LIBINPUT_SOURCE=PATCHED_SOURCE src/tools/test-pointer-libinput.sh KERNEL UINPUT_MODULE RESULT_DIRECTORY
```
