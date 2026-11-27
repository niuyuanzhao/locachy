# 自适应充电

[English](BATTERY-CHARGING.md)

**设置 → 电源与电池**中的自适应充电默认关闭，原有的手动启停阈值仍可使用。在支持
的硬件上，它采用 ChromiumOS `powerd` 的自适应充电框架：模型为
`ADAPTIVE_CHARGING_20230314`，并使用其 `ExamplePreprocessor` 预处理器配置。
`battery-history` 以纯 C 在本地运行该模型，推理时不需要 Python 或 TensorFlow 运行
时。它根据本机充电历史识别长时间插电的使用习惯，把电池维持在约 80%，而不是一直充满，
并在预测的拔电时间之前恢复充到 100%。已经超过目标的电池不会被主动放电。历史数据不足
或模型文件未通过校验时，保留正常充电或已配置的手动阈值。

**立即充满** 在支持的硬件上、接通电源时临时请求充到 100%，手动模式和自适应模式下都
可以使用。它不会更改手动设置，也不会清空已学习的历史。拔电或达到界面显示的期限（最多
24 小时）后结束。固件自身的充电策略或温度限制仍可能推迟充电。

## 模型文件

ChromiumOS 模型文件绝不随 LOCachy 源码树或 ISO 镜像分发，只有用户明确开启自适应充电
（`battery_charge_adaptive="1"`）之后才会下载。

- **固定清单。** `/usr/local/share/locachy/battery/manifest.json`（由
  `src/link/battery-adaptive-manifest.json` 安装）与编译进 `battery-history` 的副本
  （`src/progs/battery-model-pins.h`）固定了同样两个文件：
  - `mlservice-model-adaptive_charging-20230314.tflite`：124,324 字节
  - `mlservice-model-adaptive_charging-20230314-preprocessor.pb`：31,082 字节

  两个文件都固定了精确的 SHA-512 摘要。下载来源是 ChromiumOS 的 distfiles 镜像
  （`storage.googleapis.com/chromeos-localmirror/distfiles/`）。
- **下载。** 唯一的下载途径是系统任务 `battery-model-fetch`（无参数，见
  [JOBS.zh.md](JOBS.zh.md)），它以 root 运行 `locachy-battery-model fetch`。该工具在
  `locachy-net` 网络命名空间内，以非特权用户 `nobody`（uid 与 gid 均为 65534，清除附加
  组，设置 `no_new_privs`）通过 HTTPS 调用 `curl` 下载。下载只允许 HTTPS（TLS 1.2 及以
  上，重定向也只能指向 HTTPS），大小不得超过固定的字节数。两个文件都先下载到暂存区，各
  自的字节数与 SHA-512 都与固定值一致后，才会安装其中任何一个；之后在旁边写入
  `manifest.installed.json`。
- **位置。** 通过校验的文件位于 `/var/lib/locachy/battery/model/`。
- **删除。** 系统任务 `battery-model-remove` 以 root 运行 `locachy-battery-model remove`，
  不访问网络，删除两个模型文件、安装记录以及残留的暂存文件。关闭自适应充电不会删除这些
  文件，需要运行删除任务。
- **通知。** 下载或删除改动了文件之后，该工具向进程 1 发送 `SIGHUP`；进程 1 重新加载设
  置，并把 `SIGHUP` 转发给 `battery-history`，使新状态立即生效。
- **离线检查。** `locachy-battery-model status [--json]` 与
  `locachy-battery-model verify` 在本地检查已安装的文件，不建立网络连接。

`battery-history` 从不下载任何文件。它会在启动时、重新加载时以及每次自适应评估时，自行
校验这些文件。

## 校验失败时的保守回退

只有两个模型文件全部通过校验，自适应控制才会运行：每个文件都必须是普通文件（不能是符号
链接、FIFO 或目录），大小与固定值完全一致，SHA-512 与编译内置的值一致；并且 TFLite
FlatBuffer 与预处理器 protobuf 都能在受支持的子集内解析（FLOAT32 张量、FULLY_CONNECTED
与 SOFTMAX 算子）。

任一条件不满足时，自适应控制不施加保持电量的上限，也绝不降低充电上限，而是恢复手动设置，
或恢复接管前保存的阈值。保存的阈值以隐藏的 `.charge-*` 文件存放在
`/var/lib/locachy/battery/` 下，恢复后才删除。对应状态如下：

- `model-missing`：已开启自适应，但一个或两个模型文件缺失。
- `model-invalid`：模型文件或模型目录是符号链接、不是普通文件、大小不符、SHA-512 不符，
  或无法解析。若一个文件缺失而另一个无效，状态为 `model-invalid`。
- `unsupported`：机器上并非恰好有一块已插入的系统电池，或电池没有可用的序列号（为空、
  `0` 或 `unknown`），或既缺少 Linux 标准的 `charge_control_end_threshold`（若存在启动阈
  值，还需要标准的 `charge_control_start_threshold`），也缺少可控制的 `charge_behaviour`
  属性（同时提供 `auto` 与 `inhibit-charge`）。多块电池不会被假定拥有独立的控制器。
- `error`：读取或写入保存的基线、sysfs 读回、电池身份核对，或阈值与 `charge_behaviour`
  写入失败。若 sysfs 中存在 `charge_control_end_threshold` 但无法读取或格式异常，状态直
  接为 `error`，不会回退到 `charge_behaviour`。

## 硬件阈值与软件 `charge_behaviour`

`battery-history` 优先使用硬件阈值：

1. **硬件阈值（`charge_control_end_threshold`）。** 只要存在就始终优先使用，即使内核同
   时暴露了 `charge_behaviour` 也不改变原有行为。写入的阈值由 EC 自身执行，睡眠和关机期间
   同样有效。
2. **软件 `charge_behaviour`（`auto` / `inhibit-charge`）。** 仅在不存在
   `charge_control_end_threshold`，且 `charge_behaviour` 同时暴露 `auto` 与
   `inhibit-charge`（并有且仅有一个带方括号的当前项）时使用。自适应充电和手动启停限制都通
   过插电时的软件滞回区间实现：
   - 电量达到或高于停止上限（`end`，20–99%）：先保存 `-1 -1 <原状态> charge_behaviour` 基
     线文件，再写入 `inhibit-charge`。
   - 电量降至或低于启动下限（`start`，默认为 `end - 5`）：写入 `auto` 并删除基线文件。
   - 电量介于 `start` 与 `end` 之间：保持当前状态，避免频繁切换。
   - **安全释放。** 软件限制只在 `battery-history` 运行且接通电源时生效。遇到拔电、休眠前
     通知（`SIGUSR1`）、服务退出（`SIGTERM`/`SIGINT`，手动与自适应模式均如此）、**立即充
     满**、自适应 `top-up` 或保守回退时，服务会立即把 `inhibit-charge` 释放回 `auto`；启
     动时若发现遗留的 `inhibit-charge` 处于未托管、未插电或电量不高于启动下限的状态，也会
     自动恢复为 `auto`。`battery-history --once` 从不锁存 `inhibit-charge`。机器睡眠或
     关机后，充电会恢复正常进行。
   - **低唤醒轮询。** 仅当采用软件限制的电池正在插电充电且距离停止上限不超过 3% 时，服务
     才会每 15 秒轮询一次以减少过充；进入 `Not charging`（已暂停充电）后，轮询间隔自动回
     落到 5 分钟。

## 控制器条件

控制器移植自 ChromiumOS `powerd` 的 `ChargeHistory` 与 `AdaptiveChargingController`。
由于 LOCachy 所用的 sysfs 路径没有 EC 电流限制或强制放电控制，慢充与 Charge Limit 功能被省略。

- **保持电量：** 上限为 `max(80%, 当前电量百分比)`；存在启动阈值时，启动阈值为上限减 5。
  电池已充满或电量达到 96% 及以上时，不开始保持。
- **启用条件：** 至少保留 **14 天**历史，且满电与保持时间合计占插电时间的比例不低于
  **50%**（`>= 0.5`）。两个条件都满足之前，状态为 `learning`，按正常充电处理。
- **预测：** 由本地历史（时长按 15 分钟粒度向下取整）得到 201 项特征，在插电时以及之后约每 30 分钟，
  输入 9 档拔电时长模型。从第 0 档开始累加概率，直到总和达到 **>= 0.35**。
- **延迟上限：** 自开始保持起，预计的剩余插电时间不超过已完成插电时长的**第 30 百分位**。
- **提前充满：** 在预测的拔电时间（或受上限截断的拔电时间）之前 **2 小时**，恢复充到
  100%（启动阈值 99%，上限 100%），状态为 `top-up`。
- **保留期限：** 每块电池最多保留 **30 天**的每日汇总和 **50 次**完整插电记录，仅在自适应
  充电开启期间保存到 `/var/lib/locachy/battery/charge-history-<pack_id>.bin`。

立即充满生效期间（接通电源时），阈值写为启动 99%、上限 100%。

## 状态

`battery-history` 把每块电池的状态写入 `/var/lib/locachy/battery/charge-status`：

| 状态 | 含义 |
|---|---|
| `manual` | 自适应充电已关闭，适用手动阈值（如有）。 |
| `full` | 接通电源时，**立即充满** 生效。 |
| `model-missing`、`model-invalid`、`unsupported`、`error` | 见「校验失败时的保守回退」。 |
| `on-battery` | 未接通电源。 |
| `learning` | 尚未满足启用条件。 |
| `hold` | 暂缓充满，保持在保持电量附近。 |
| `top-up` | 本次插电期间已保持，现已恢复充向 100%（即在预计拔电前充满）。 |
| `inactive` | 已开启自适应并接通电源，但当前没有保持：例如开始保持前电量已达 96% 及以上、预计的拔电时间太近而不宜保持、预测失败，或电源状态未知。 |

## 历史与时钟

睡眠、关机以及缺失的 AC 记录，都不能证明机器一直插着电。挂钟时间的跳变会对照单调开机
时间校正。服务不会为了调整充电目标而唤醒休眠中的机器。没有可靠 AC 记录的旧历史仍可查
看，但不会用于学习。

## 配置

- `battery_charge_adaptive="0"`：支持的硬件上设为 `"1"` 才启用。
- `battery_charge_start`、`battery_charge_end`：保存的手动启停百分比。
- `battery_charge_full_until`：由 **立即充满** 写入的临时期限，不是需要长期保留的充电上限。

关闭自适应后恢复手动值，未设置的字段恢复接管前的值。电池读数、历史曲线以及硬件已有的手
动设置仍可使用。

软件测试覆盖纯 C 推理引擎（`test-battery-tflite.c`）、控制器状态机
（`test-battery-controller.c`）、清单与下载校验（`test-battery-model.py`），以及用模拟电源
设备检查的阈值与 `charge_behaviour` 写入、读回、滞回控制和安全释放路径。这些测试不能代替对
具体笔记本固件 EC 实际停充、恢复充电行为的验证。
