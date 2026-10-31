# 应用预设

[English](APPS.md)

容器可选的预设（Edge、Firefox、微信等）放在单独的仓库 `locachy-apps` 中，不在本源码里。
本仓库只保留宿主程序的预设（`src/containers/`：`foot.conf`、`code.conf`，`host="1"`）：
它们的软件包属于宿主清单，`code.conf` 运行的是系统自带的包装脚本。应用预设绝不能是宿主程序；
应用仓库里出现 `host=` 会使整个目录被拒绝。

读取和安装目录只有一个辅助模块：`src/link/locachy_apps.py`（由 `install-pkg-tools` 安装到
`/usr/local/lib/locachy/`）。更新器加载它；`make.sh`、live 入口和 `make-installer.sh` 以
`python3 -I locachy_apps.py validate|catalog|bootstrap ...` 运行它。

## 仓库（格式 1）

```
FORMAT          一行：1
apps/NAME.conf  预设
README.md, README.zh.md, LICENSE
tools/check.py  用某个系统自己的辅助模块检查本仓库：
                python3 -I tools/check.py PATH/TO/src/link/locachy_apps.py
```

其中的文件都是数据：只解析、校验，从不 source 或执行。第一个无效文件会使整个目录被拒绝，
已安装的目录保持不变：

- `FORMAT` 必须是本系统能读的格式（`1`），否则报 `not compatible: format N`。
- `NAME` 为 `[a-z0-9][a-z0-9._-]{0,62}`。每个预设是普通文件（不是链接），至多 16 KiB，UTF-8。
  `apps/` 中不能有其他东西，且至少有一个预设。
- 每行是空行、`#` 注释或 `KEY="VALUE"`。键为 `locachy-app` 的键去掉 `host`：`package`、
  `extras`、`command`、`arguments`、`network`、`camera`、`directories`、`desktop`、`x11`。
  未知键、重复键、`host=` 或其他任何行都会拒绝整个目录。
- 值中不能有 `"`、`\`、`$`、反引号或控制字符。`package` 是一个 pacman 包名；`extras` 是空格分隔的包名；
  `command` 是不含 `..` 和空格的绝对路径；`network`、`camera`、`x11` 为 `0` 或 `1`；
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
供测试和 live 安装器使用）。搜索仍会跳过其中任何 `host="1"` 预设。

## 更新

Check 把 `apps_repo` 的 `apps_branch` 获取到 `repo.git`，验证签名并校验内容，再与已安装目录的提交比较。
计划行（目标 `apps`）：

- 两者不同时：`repo locachy-apps OLD NEW_LABEL` 和 `catalog "app presets from branch BRANCH" OLD COMMIT`；
- 每份将跟随新预设的配置：`merge NAME "KEY KEY ..."`；若这会改变容器的 `package` 或 `extras`，
  另有 `NAME rebuild "its preset names other packages" OLD NEW`，让同一次 Apply 重建它；
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

对每个 `/etc/locachy/containers/NAME.conf`：若不是宿主程序且 `NAME` 在新目录中，其基准为
`baseline/NAME.conf`，否则为旧目录的 `NAME.conf`；两者都没有，说明是用户自己建的配置，从不改动。
对基准或新预设中的每个键：配置仍是基准的值而新预设的值不同时，配置改用新值（新预设没有该键则删去）。
注释、其他键和用户改过的值都保留。文件一步替换，然后其 baseline 改为新预设。从目录中删去的预设不影响对应配置。
Apply 会打印每处改动。

## 拆分后的第一次更新

`make.sh` 在替换 `/usr/local/share/locachy/containers`（现在只有宿主程序的预设）之前，对它运行
`locachy_apps.py bootstrap`：若还没有目录，旧系统装在那里的应用预设成为目录（`origin="bootstrap"`，
无提交，`system_commit` 取自 `/usr/local/lib/locachy/source-commit`）。已有的目录从不改动。
第一次 Apply 运行的是旧更新器的 Check；新更新器在源码安装后只重新规划，不获取应用预设。
该次 Apply 末尾的 Check 以及之后的每次 Check 都会获取；在装上第一份来自应用仓库的目录之前，引导说明一直显示。

## 安装

- live 入口（`installer/locachy-install`）用获取源码的同样方式获取应用仓库：`--apps-repo` 和
  `--apps-branch`，否则为被继续的那次安装所用的，否则为 `/etc/locachy/source-origin.json` 的
  `apps_repo`、`apps_branch`。由获取到的源码自己的辅助模块生成目录。失败时使用之前从同一处获取的目录，
  否则使用介质自带的目录（`/usr/local/share/locachy/apps-catalog`）。它导出 `LOCACHY_APPS_CATALOG`
  和 `LOCACHY_APPS`（`fetched`、`fetched earlier`、`fetched before: WHY`、`bundled: WHY`）。
- `locachy-install` 提供宿主程序（`SOURCE/containers`，仅宿主）和目录中的预设。`--describe-json`
  含 `"apps": {"text", "not_newest"}` 及 `defaults.apps_repo`/`apps_branch`。选了目录中没有的应用，
  会在动任何磁盘之前停止（`Unknown app: NAME`）；不再为未知名称生成通用预设。整个目录复制到目标的
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
覆盖安装器、live 入口和介质自带的目录。
