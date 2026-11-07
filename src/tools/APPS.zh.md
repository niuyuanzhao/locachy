# 应用预设

[English](APPS.md)

应用商店提供的配方（Edge、Firefox、微信、ChatGPT 等）放在单独的仓库 `locachy-apps` 中，
不在本源码里。本仓库只保留内置宿主工具的预设（`src/containers/`：`foot.conf`、`code.conf`）；
`code.conf` 运行系统自带的包装脚本。格式 2 的商店配方可用 `host="1"` 申请真实系统访问；
Packages 显示申请的模式，并在保存新授权之前要求确认。不写该键或写 `host="0"` 时，应用使用普通隔离文件系统。

ChatGPT 直接从商店配方选择安装，不必先手动安装宿主包。其代理需要实际的根目录、完整 home、
宿主工具和终端。它仍有进程命名空间、cgroup、自身的显示/总线接线，以及 home 管理的启动、冻结、恢复和停止。
它以用户身份运行，sudo 仍需认证。

读取和安装目录只有一个辅助模块：`src/link/locachy_apps.py`（由 `install-pkg-tools` 安装到
`/usr/local/lib/locachy/`）。更新器加载它；`make.sh`、live 入口和 `make-installer.sh` 以
`python3 -I locachy_apps.py validate|catalog|bootstrap ...` 运行它。

## 仓库（格式 2）

```
FORMAT          一行：2
apps/NAME.conf  预设
README.md, README.zh.md, LICENSE
tools/check.py  用某个系统自己的辅助模块检查本仓库：
                python3 -I tools/check.py PATH/TO/src/link/locachy_apps.py
```

其中的文件都是数据：只解析、校验，从不 source 或执行。第一个无效文件会使整个目录被拒绝，
已安装的目录保持不变：

- `FORMAT` 必须是本系统能读的格式（`1` 或 `2`），否则报 `not compatible: format N`。
  格式 2 增加 `host`，格式 1 仍不允许该键。先更新系统再更新应用目录：旧系统会明确拒绝格式 2，保留之前的目录。
- `NAME` 为 `[a-z0-9][a-z0-9._-]{0,62}`。每个预设是普通文件（不是链接），至多 16 KiB，UTF-8。
  `apps/` 中不能有其他东西，且至少有一个预设。
- 每行是空行、`#` 注释或 `KEY="VALUE"`。键为 `package`、`extras`、`command`、`arguments`、
  `network`、`camera`、`directories`、`desktop`、`x11`，格式 2 还有 `host`。
  未知键、重复键或其他任何行都会拒绝整个目录。
- 值中不能有 `"`、`\`、`$`、反引号或控制字符。`package` 是一个 pacman 包名；`extras` 是空格分隔的包名；
  `command` 是不含 `..` 和空格的绝对路径；`network`、`camera`、`x11`、`host` 为 `0` 或 `1`；
  `directories` 是冒号分隔、不含 `..` 的绝对路径；`desktop` 是 `*.desktop` 文件名。
  `package` 和 `command` 必须有。

## 已安装系统上

- `/var/lib/locachy/apps/catalog/`：`NAME.conf` 和 `CATALOG`（`format`、`origin` 为
  `repository`、`medium` 或 `bootstrap`、`repo`、`branch`、`commit`、`installed`，引导目录另有
  `system_commit`）。
- `/var/lib/locachy/apps/repo.git`：浅获取的仓库。
- `/var/lib/locachy/apps/baseline/NAME.conf`：每份配置上次跟随的预设。
- `rc.conf`：`apps_repo`（缺省：宿主上为 `https://github.com/niuyuanzhao/locachy-apps.git`；
  `""`、`0` 或 `off`：不更新应用预设）和 `apps_branch`（缺省 `main`）。`LOCACHY_APPS_REPO`、
  `LOCACHY_APPS_BRANCH` 覆盖它们，与源码的 `LOCACHY_UPDATE_REPO`、`LOCACHY_UPDATE_BRANCH` 相同。
- 签名：`/etc/locachy/trusted-signers` 非空时，目录的提交必须有其中某个密钥的有效 SSH 签名，
  与源码提交完全相同。不会自动导入任何密钥。

`locachy-pkg search`、Packages 表单和安装器读取同一个目录（`LOCACHY_APPS_CATALOG` 可指定其位置，
供测试和 live 安装器使用），包括申请 `host="1"` 的配方。已装目录按它自身 `CATALOG` 中的格式校验，
不会直接按辅助模块支持的最新格式解释。

## 更新

Check 把 `apps_repo` 的 `apps_branch` 获取到 `repo.git`，验证签名并校验内容，再与已安装目录的提交比较。
计划行（目标 `apps`）：

- 两者不同时：`repo locachy-apps OLD NEW_LABEL` 和 `catalog "app presets from branch BRANCH" OLD COMMIT`；
- 每份将跟随新预设的配置：`merge NAME "KEY KEY ..."`；若这会改变容器的 `package` 或 `extras`，
  另有 `NAME rebuild "its preset names other packages" OLD NEW`，让同一次 Apply 重建它（已授权真实系统访问的应用也如此）；
- 配方申请了不同访问模式时：`note "NAME: preset requests real-system access; review Access in packages; current access is kept"`
  （或 `isolated access`）；
- 无法获取、验证或校验时：`note "not checked: WHY"`；目录仍是引导目录时：
  `note "bootstrap catalog from the system source, not from the apps repository"`。
  关闭应用预设更新时完全没有 `apps` 行。

Apply 的顺序：先源码（同以前），再 `apps`、`host`、`locachy`、各容器。`apps` 从 `repo.git` 安装
Check 固定的那个提交，不再获取：Check 之后远端又有新提交，也不改变所应用的内容。不做检查的重新规划
（源码安装之后）在 `repo.git` 仍有该提交时沿用它，否则应用预设部分等下一次 Check。`apps` 失败时显示
`===> apps: not done`，不阻止其他任何部分；因合并而要重建的容器留待以后。

仅含应用预设的 Apply 从不运行 `make.sh`、`locachy-boot`、pacman 或 `locachy-build`，也不写
source-unbuilt 标记。系统更新从不删除或覆盖该目录。

新目录写在旧目录旁边，两者一步互换（`renameat2` 的 `RENAME_EXCHANGE`）。中断后留下的要么是完整的旧目录，
要么是完整的新目录；下一次切换会清除换下来的那份。

### 合并

对每个 `/etc/locachy/containers/NAME.conf`：若 `NAME` 在新目录中，其基准为 `baseline/NAME.conf`，
否则为旧目录的 `NAME.conf`；两者都没有，说明是用户自己建的配置，从不改动。对基准或新预设中的每个普通键：
配置仍是基准的值而新预设的值不同时，配置改用新值（新预设没有该键则删去）。**`host` 绝不合并：**
无论 baseline 如何变化，现有值（包括省略该键）都原样保留。申请有变化时提示审阅，不在后台授予或撤销权限。
已授权真实系统访问的应用可以跟随普通参数和软件包变化。注释、其他键和用户改过的值都保留。
文件一步替换，然后其 baseline 改为新预设。从目录中删去的预设不影响对应配置。Apply 会打印每处改动。

## 拆分后的第一次更新

`make.sh` 在替换 `/usr/local/share/locachy/containers`（现在只有宿主程序的预设）之前，对它运行
`locachy_apps.py bootstrap`：若还没有目录，旧系统装在那里的应用预设成为目录（`origin="bootstrap"`，
无提交，`system_commit` 取自 `/usr/local/lib/locachy/source-commit`）。已有的目录从不改动。
第一次 Apply 运行的是旧更新器的 Check；新更新器在源码安装后只重新规划，不获取应用预设。
该次 Apply 末尾的 Check 以及之后的每次 Check 都会获取；在装上第一份来自应用仓库的目录之前，引导说明一直显示。

## 安装

在 Packages 中搜索配方，或在新建应用表单中选择配方。表单显示 Access；保存一份新的真实系统授权时要求明确确认。
两种模式的“保存并构建”都使用 `locachy-app build NAME`。真实系统访问模式通过 `locachy-pkg install` 安装
`package` 和 `extras`，检查命令之后才写构建成功标记。失败会显示且能重试。这些包是宿主共享的：
移除应用配置不会自动卸载它们。

用过原先系统 ChatGPT 预设的安装保留配置和数据。`make.sh` 只在首次需要时把原来安装的预设存成合并基准，
然后删除过时的系统预设副本。之后 ChatGPT 来自商店目录。

- live 入口（`installer/locachy-install`）用获取源码的同样方式获取应用仓库：`--apps-repo` 和
  `--apps-branch`，否则为被继续的那次安装所用的，否则为 `/etc/locachy/source-origin.json` 的
  `apps_repo`、`apps_branch`。由获取到的源码自己的辅助模块生成目录。失败时使用之前从同一处获取的目录，
  否则使用介质自带的目录（`/usr/local/share/locachy/apps-catalog`）。它导出 `LOCACHY_APPS_CATALOG`
  和 `LOCACHY_APPS`（`fetched`、`fetched earlier`、`fetched before: WHY`、`bundled: WHY`）。
- `locachy-install` 提供宿主程序（`SOURCE/containers`，仅宿主）和目录中的预设。`--describe-json`
  含 `"apps": {"text", "not_newest"}` 及 `defaults.apps_repo`/`apps_branch`。选了目录中没有的应用，
  会在动任何磁盘之前停止（`Unknown app: NAME`）；不再为未知名称生成通用预设。选择真实系统配方时，已有的宿主程序
  标签会说明其访问模式，并把 `package` 和 `extras` 加入目标宿主清单；继续安装时使用目标已固定的目录也是如此。
  整个目录复制到目标的
  `/var/lib/locachy/apps/catalog`，所选预设复制到 `/etc/locachy/containers`，`apps_repo`
  （非 `main` 时还有 `apps_branch`）写入 `rc.conf`。继续安装的记录保存目录的提交；继续的安装使用其目标上已有的目录。
- `make-installer.sh` 需要 `LOCACHY_APPS_SRC`，即应用仓库的一份检出。缺失或无效时，在生成任何东西之前
  说明原因并停止；介质上绝不会有空目录。`LOCACHY_APPS_REPO`、`LOCACHY_APPS_BRANCH`、
  `LOCACHY_APPS_COMMIT`（否则取自检出的 git）记录到 `source-origin.json` 和目录中。
  `.github/workflows/release-iso.yml` 为此检出 `niuyuanzhao/locachy-apps`。

## 测试

`tools/test-apps-catalog.py`（已加入 `test-interaction-regressions.sh`）以本地 git 仓库充当应用仓库，
覆盖每条校验规则、保留旧目录的不兼容格式和获取失败、中断的切换、Check 之后远端前进、签名要求、合并规则、
重建行、什么都不构建的仅应用预设 Apply、引导和搜索。`test-install.py` 与 `test-installer-profile.py`
覆盖安装器、live 入口和介质自带的目录，包括格式 2、宿主软件包及 extras 的选择。

应用的真实流程验收还必须从商店 UI 开始，再作为重启后的第一个应用从 home 打开，并验证第二次显示会话。
有窗口、网络路径或进程检查不等于账号登录或登录后的 Codex 终端执行已通过；后两项须单独报告。
