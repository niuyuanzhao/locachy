# LOCachy 空闲息屏与显示输出归属

[English](DISPLAY-POWER.md)

## 独立开关

- **安全与隐私 → 锁屏 → 自动锁屏**控制空闲锁屏以及合盖、睡眠和低电量休眠前的锁屏。空闲锁屏时间由 `lock_after` 保存，默认 10 分钟；`0` 只关闭由空闲触发的锁屏。空闲锁屏后还有「免密码解锁时间」（`lock_grace`，默认 5 秒；`0` 表示立即要求密码），这段时间内的输入会直接解锁；它不适用于锁屏键、合盖、睡眠、低电量或高威胁烈度锁屏。关掉自动锁屏开关仍会保留时间。高威胁烈度下不能关闭自动锁屏，也会关闭免密码窗口。
- **电源与电池 → 显示 → 自动息屏**只控制显示输出。`auto_blank="1"` 开启；省略或设为 `0` 时关闭。`blank_after` 单独保存息屏时间，范围为 1–60 分钟，默认 5 分钟。缺失或无效时使用默认值；关闭开关不清除时间。
- 两种空闲计时互不影响。操作键鼠等输入设备会点亮屏幕，不会认证或解除已有锁屏。息屏不暂停系统、不改亮度或合盖动作，也不影响手动锁屏。共享桌面上，视频等应用的 Wayland 空闲抑制对两种计时仍然有效。

## 显示输出归属

只有进程 1 持有 DRM master 时，它才通过连接器的 DRM DPMS 属性息屏，并且只恢复自己关闭的输出。扩展屏任务在前台时明确不自动息屏：配对过程要看配对码，连接后仍沿用原来的延迟空闲锁屏规则。不更改输出租借，也不会因此停止、重连或唤醒发送端。

共享桌面的 DRM master 由 labwc 持有时，由受信任的 `labwc-control` 使用合成器已有的输出电源协议。只记录和恢复本次息屏更改的输出，响应输入或座席交接，不改保存的显示器模式和位置。若输出电源控制丢失，进程 1 经原有交接流程重新接管显示，避免会话停在黑屏。沙箱容器看不到输出电源协议。

## 电源配置与 zram

两者都是 rc.conf 中的键。只有缺少它们的旧 rc.conf 才会由 make.sh 写入：

- `power_profile`（**设置：电源与电池**）：`auto`（电池供电省电、接电源时均衡，
  默认）、`power`、`balanced` 或 `performance`。它的设计目标是设置 CPU 能效偏好和
  ACPI 平台配置文件。
- `zram`：`"1"`（默认）在内存中保留压缩交换区 `/dev/zram0`，位于磁盘交换区之上，机器内存
  至少 512 MiB 时启用；`"0"` 关闭它。休眠前会关闭它，使休眠映像写入磁盘交换区
  （`init/init.c` 中的 `teardown_zram_swap`）。

状态：`init/init.c` 应用 `power_profile`（`apply_power_profile`）：设置变化、交流/电池状态
变化时写入 CPU 的 `energy_performance_preference`（intel_pstate 或 amd_pstate）和 ACPI 的
`platform_profile`，收到 SIGHUP 时重新应用。尚未在真机上验证。

## 内核与驱动默认值

make.sh 安装以下 LOCachy 默认值。同名文件放在优先级更高的目录中时，会替换对应的默认
文件。

- **音频：** `/usr/local/lib/modprobe.d/50-locachy-pm.conf`（来自
  `link/50-locachy-pm.conf`）：`options snd_hda_intel power_save=1
  power_save_controller=Y`。HDA 编解码器静默 1 秒后断电。若编解码器唤醒时有爆音，
  在 `/etc/modprobe.d/50-locachy-pm.conf` 中写入 `power_save=0`，它会遮盖本文件。Wi-Fi
  省电由 NetworkManager 管理，不是模块选项。
- **PCI 与 USB 运行时电源管理、SATA 链路电源：**
  `/usr/local/lib/udev/rules.d/85-locachy-pm.rules`（来自
  `link/85-locachy-pm.rules`）：
  - PCI 设备设为 `power/control=auto`，但显示控制器（类 `0x03`）和通信控制器（`0x07`，
    如 Intel MEI 与串口）保留驱动自己的策略。
  - USB 设备设为 `auto`，但 HID（`03`）和无线（`e0`）设备除外。含有 HID（`03`）、音频
    （`01`）、视频（`0e`）、无线（`e0`）、CDC（`02`、`0a`）或打印机（`07`）接口的设备，
    其父设备保持 `power/control=on`。接口规则同样在 `bind` 时运行，因为 udev 不保证
    不同设备事件的先后。
  - SATA 主机设为 `link_power_management_policy=med_power`；未采用 DIPM 变体，因为它
    在某些 SSD 上会造成 I/O 停顿。
  - I2C 设备不做处理：触摸控制器和手写笔就在那里。
  - 要修改单条规则，在 `/etc/udev/rules.d` 中放同名文件；要整体停用，在那里把它链接到
    `/dev/null`。
  - 安装后可在机器上检查：`grep -r . /sys/bus/usb/devices/*/power/control`。
- **Wi-Fi：** `/etc/NetworkManager/conf.d/30-locachy-network.conf`（来自
  `link/NetworkManager.conf`）设置 `wifi.powersave=3`，即开启 Wi-Fi 省电。
- **回写与交换区预读：** `/usr/local/lib/sysctl.d/50-locachy.conf`（来自
  `link/50-locachy-sysctl.conf`）：`vm.dirty_writeback_centisecs = 1500`（每 15 秒回写
  一次，而不是 5 秒，减少磁盘唤醒）和 `vm.page-cluster = 0`（不做交换区预读，在 zram 上
  只会白白耗时）。NMI 看门狗保持开启：它负责报告硬锁死。

## 验证

`tools/test-init-screen.c` 覆盖独立计时、失败不空转、直连唤醒、保留扩展屏连接、共享桌面显示归属，以及关闭自动锁屏后拒绝延迟到达的空闲锁屏请求。`tools/test-screen-power.sh` 检查恢复进程 1 关闭的输出时不触碰租借或本来就关闭的输出。labwc 测试覆盖座席私有消息与输出电源协议；不以无设备测试的通过替代实体面板验收。
