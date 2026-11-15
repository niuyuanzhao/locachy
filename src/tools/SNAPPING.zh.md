# 多屏窗口边缘分屏

[English](SNAPPING.md)

## 拖窗跨屏边缘停留

上游 labwc 的 `screenEdgeStrength` 仅对窗口矩形边缘施加阻力，不在屏幕交界处拦住光标。`pkgbuilds/labwc-locachy/inner-edge-hold.patch` 在交互式移动浮动窗口时提供内边缘停留：

- 交互式拖动浮动窗口跨过相邻屏幕交界时，光标在原屏幕边缘停留 750 毫秒（`<snapping><innerEdgeHold>750</innerEdgeHold></snapping>`；设为 `0` 关闭）。
- 停留期间松手触发原屏幕的半屏分屏；继续推动超过 750 毫秒穿过交界进入相邻屏幕；反向拉回立即取消停留。
- 普通指针移动和调整窗口大小不受影响。

## 多屏交界处的概览热角

`pkgbuilds/labwc-locachy/corner-edge-hold.patch` 在指针斜向接近屏幕左上角 30 逻辑像素内的上／左边界时停留至多 350 毫秒，使顶栏 75 毫秒的左上角概览热角在共享屏幕交界处同样能够触发。纯水平或纯垂直移动仅在 1 像素触发角捕获；拖窗、调整大小、按键拖拽和全屏窗口覆盖时不受影响。

## 测试

```sh
bash tools/test-bar-corner.sh /tmp/locachy-corner-unit
LOCACHY_TEST_LABWC=/path/to/labwc bash tools/test-snapping.sh /tmp/locachy-inner --inner-edge
LOCACHY_TEST_LABWC=/path/to/labwc bash tools/test-snapping.sh /tmp/locachy-corner --corner
LOCACHY_TEST_LABWC=/path/to/labwc bash tools/test-snapping.sh /tmp/locachy-geometry
```
