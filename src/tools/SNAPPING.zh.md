# 多屏窗口边缘分屏

[English](SNAPPING.md)

## 报告、复现原因和修复

用户报告的**不是**外边缘分屏，也不是能否把指针精确停在两屏交界后松手。正常
拖窗时，指针直接越过屏幕交界，无法顶住边缘选择半屏分屏。之前的 174 项精确
坐标几何检查没有测试这个问题，不能再用它们声称该问题已验收。

已在 labwc 0.20.2、三个真实无头输出上用连续相对指针运动复现。两处屏幕交界的
四个方向，无论合成器标题栏还是客户端标题栏，都无法顶住。未打修复补丁时，
同一份最终测试为 **21 PASS、53 FAIL**（`INNER_EDGE_FAILURES=53`），包含下文补充的
纵向、混合缩放和生命周期检查。

上游的 `screenEdgeStrength` 只给**窗口矩形**的边缘施加阻力，不会把指针挡在
内边缘的分屏触发点。`snapping/range/inner` 只识别指针当前坐标，预览延迟只影响
提示出现时间，都不能挡住连续指针运动。对应上游代码是 `resistance.c` 的
`resistance_move_apply`、`input/cursor.c` 的 `preprocess_cursor_motion`，以及
`interactive.c` 的 `edge_from_cursor`。

跟踪配方中的 `pkgbuilds/labwc-locachy/inner-edge-hold.patch` 在合成器层补上这个
行为，不使用单机补丁：

- 仅在交互式移动浮动窗口时，指针试图跨过真正相邻的输出边缘，会在原屏幕一侧
  停留 750 毫秒。
- 超过该有限阻力时间后继续拖动，即可穿过交界。反向退回立即取消停留；停留期间
  松手，走 labwc 原来的半屏吸附。原有的 500 毫秒预览有时间出现。
- 普通指针移动和交互式调整大小不受影响。外边缘仍使用原有输出布局约束；不相邻
  的屏幕或布局空隙不会产生虚假的内边缘。
- 补丁的上游文档说明 `<snapping><innerEdgeHold>750</innerEdgeHold></snapping>`；
  设零关闭，且要求内边缘吸附范围非零。不写入新的单机配置。

配方适配器在原有剪贴板闸门补丁之后应用此小补丁，要求零 fuzz。不可变输入清单和
补丁版本已更新，正常 Check/Apply 会重编并验证跟踪的稳定包。行为合约编号为
`labwc-clipboard-inner-edge-v2`，不再复用只检查剪贴板的旧合约。

## 连续运动验收

在一次性 Linux 测试根中指定候选合成器：

```sh
LOCACHY_TEST_LABWC=/absolute/path/to/candidate-labwc \
  bash tools/test-snapping.sh /absolute/new/inner-result --inner-edge
```

测试编译当前 LOCachy 顶栏和屏幕控制器，通过真实控制器配置三个并排的 1280x720
输出。虚拟指针重复发送**相对**运动，而不是抵达精确坐标后松手。针对两处交界的
各自两个方向、SSD 和 CSD 客户端分别检查：

1. 连续推向交界能被顶住：额外输入 150 像素后，截图中的拖动抓取点仍在交界。
2. 松手之前，原屏幕一侧实际出现分屏预览轮廓。
3. 松手后吸附到原屏幕一侧的半屏区域。
4. 有意超过有限停留时间后继续拖动，能跨到相邻屏幕。
5. 不拖窗时普通指针立即穿过相同交界，用接收客户端的指针事件验证。

额外的小范围检查包括纵向交界双向、缩放 1/1.5 交界双向、立即退回取消后在同一
窗口重新拖动、关闭 `innerEdgeHold`，以及一个跨过两处交界的 1900 像素事件
（必须停在第一处）。错位相邻屏幕的斜向测试复现了原型的端点检查错误：运动线段
穿过共享边界，但终点已经离开共享区间。补丁现在求运动线段与边界的交点并停在
交点，不再仅检查终点。

2026-10-30，剪贴板和内边缘补丁同时应用的候选版本通过 **74 项检查，零失败**。
开发服务器证据：

- `~/x230-dev/snap-agent/inner-expanded-final/` 和 `inner-expanded-final.out`：带补丁候选，
  `INNER_EDGE_FAILURES=0`，含按住鼠标键时的中间截图。
- `~/x230-dev/snap-agent/inner-expanded-stock/` 和 `inner-expanded-stock.out`：同一份最终
  测试、不带内边缘补丁，`INNER_EDGE_FAILURES=53`。
- `~/x230-dev/snap-agent/build-combined.log`：两个跟踪补丁零 fuzz 应用并编译合成器。
  此隔离验证构建关闭了 Xwayland、图标和 SVG，不等于完整的包构建。

这验证了隔离候选中的已复现合成器缺陷，**不等于用户机器上的物理设备验收**。
物理指针加速、交互式调整大小不受影响，以及完整发行包功能构建，不能由这次
精简合成器原型验证来证明。合约在没有可选 Rime 模块时也能编译 bar；在一次性
覆盖根中移除 Rime 头文件和库后，实际 74 项内部边缘测试通过
（`~/x230-dev/test-snap-no-rime-fd0af365-rerun.log`）。没有为让测试通过而额外安装包依赖。对应的启用 Rime 构建及 74 项实测也通过
（`~/x230-dev/test-snap-with-rime-fd0af365.log`）；ELF 依赖核对确认两次构建分别走了预期的可选路径。

## 独立的精确目标几何回归

```sh
LOCACHY_TEST_LABWC=/absolute/path/to/candidate-labwc \
  bash tools/test-snapping.sh /absolute/new/geometry-result
```

测试使用双输出或三输出私有无头合成器、真实输出管理、当前编译的顶栏及其预留
区域和可调整大小的窗口。真实输入设备不可见，不打开 uinput 或物理显示器。
日志和截图（`.ppm.gz`）保留在结果目录。

矩阵包括四种双屏布局（横向、缩放 1/1.5、负 X、负 Y 纵向），以及三屏横向和
三屏混合缩放 1/1.5/1.25。在每个输出上分别拖动 SSD、CSD 窗口到左、右、下、上、
左上、右下。测试会有意越过短暂阻力抵达最终坐标；它**不是**连续顶边测试。

输出状态回读、xdg-shell 尺寸和吸附/最大化状态、截图像素必须一致。客户端矩形
只能出现在目标输出，扣除装饰和顶栏后必须符合指定分屏区域。
带补丁候选通过 **174 项检查（168 次拖动、6 次输出回读）**：
`~/x230-dev/snap-agent/geometry-patched-final/` 和 `geometry-patched-final.out`，
`SNAPPING_FAILURES=0`。两种三屏布局占其中 72 次拖动和 2 次配置检查。

## 上游依据

- [labwc-config(5)](https://labwc.github.io/labwc-config.5.html)
- [0.20.2 interactive.c](https://github.com/labwc/labwc/blob/0.20.2/src/interactive.c)
- [0.20.2 input/cursor.c](https://github.com/labwc/labwc/blob/0.20.2/src/input/cursor.c)
- [0.20.2 resistance.c](https://github.com/labwc/labwc/blob/0.20.2/src/resistance.c)
- [0.20.2 view.c](https://github.com/labwc/labwc/blob/0.20.2/src/view.c)
- [上游问题 2602](https://github.com/labwc/labwc/issues/2602) 把默认识别范围从 1 增至
  10，但没有增加指针停留阻力。
