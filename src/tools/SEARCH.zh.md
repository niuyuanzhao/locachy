# 全局搜索（locachy-searchd）

[English](SEARCH.md)

LOCachy 的全局搜索分两层，由同一个快捷键打开：`/etc/locachy/rc.conf` 中的
`key_search`，默认 `Super+Space`。

- **第一层（名称，即时）：** 已配置搜索根目录下的文件名，直接从 SQLite 名称索引中
  回答，不等待文档扫描。搜索程序还会按名称列出宿主程序（如设置）和容器。
- **第二层（内容）：** 文档和文本文件正文的全文检索。结果在第一层之后陆续出现，
  无需切换模式。

## 组件与构建

| 部件 | 安装位置 | make.sh 何时构建 | 依赖 |
|---|---|---|---|
| 索引守护进程 | `/usr/local/lib/locachy/locachy-searchd`（`progs/locachy-searchd.c`、`progs/search-index.c`） | `pkg-config --exists sqlite3` | SQLite；守护进程拒绝 3.43 之前的库（FTS5 `contentless_delete`），推荐 3.45 以上 |
| 文本提取器 | `/usr/local/bin/locachy-docscan`（`progs/locachy-docscan.c`） | `pkg-config --exists libarchive` | libarchive；PDF 需要 MuPDF（可选） |
| 搜索程序 | `/usr/local/bin/search`（`progs/search.c`） | `progs/search.c` 存在时 | 窗口传输目标文件、输入法与 FreeType，同 `files` |

make.sh 只检查 sqlite3 是否存在；版本检查在守护进程打开索引时进行
（`SEARCH_ERR_SQLITE`）。

### locachy-docscan 中的 MuPDF

开关是 `DOCSCAN_NO_MUPDF`，而不是 `HAVE_MUPDF`：MuPDF 默认编入
（`locachy-docscan.c`）。make.sh 与 `build-check.sh` 的选择方式与
`tools/test-docscan.sh` 相同：

- `pkg-config --exists mupdf`：使用它的 `--cflags` 与 `--libs`，再加 `-lm`；
- 否则若 `/usr/include/mupdf/fitz.h` 存在：`-lmupdf -lm`；
- 否则加 `-DDOCSCAN_NO_MUPDF`。二进制照常构建，遇到 PDF 时回答「未编入 PDF 支持」
  （退出码 4，不支持）；DOCX、ODT、EPUB 和文本仍可用。

提取器总是链接 libarchive（`pkg-config --cflags --libs libarchive`），与
`archive-worker` 一样用 `$GCC` 构建。

## 服务与存储

`locachy-searchd` 作为宿主服务运行（`/etc/locachy/rc.conf` 中的 `service_search`），
并链接可移植的索引引擎（`progs/search-index.h`、`progs/search-index.c`）。

- **数据库：** `/var/lib/locachy/search/index.db`。
- **编译指示：** `auto_vacuum=INCREMENTAL`（在创建第一张表之前设置）、
  `journal_mode=WAL`、`synchronous=NORMAL`、`temp_store=MEMORY`、
  `cache_size=-4000`，以及用于兼容性检查的 `user_version=1`。
- **表：**
  - `meta(key, value)`：保存扫描的 `generation` 代数计数器。
  - `dirs(id, parent, name, mtime, gen)`，`UNIQUE(parent, name)`：目录树。移动目录
    是对 `dirs` 的一条 `UPDATE`，删除子树是对 `parent` 的一次递归查询。完整路径由
    父链重建，并带有内存中的热点缓存。
  - `files(id, dir, name, size, mtime, kind, gen, text_state)`，`UNIQUE(dir, name)`：
    文件元数据，索引包括 `(dir, name)`、`gen`、`mtime`、`name COLLATE NOCASE`，以及
    针对待提取或过期 `text_state` 的部分索引。
  - `names_fts`：基于 `files.name` 的外部内容 FTS5 trigram 表，由 `INSERT`、
    `DELETE` 和 `UPDATE OF name` 触发器保持同步。
  - `docs_fts`：无内容的 FTS5 trigram 表（`content=''`、`contentless_delete=1`，
    rowid 为 `files.id`），用于 3 个及以上字符的内容查询。
  - `docs_uni`：`unicode61` 的 FTS5 表（rowid 相同），用于 1–2 个字符的前缀内容查询
    （`"q"*`）以及摘要上下文。

## 查询匹配与安全

- 3 个及以上 UTF-8 字符的查询使用 `names_fts`（第一层）和 `docs_fts`（第二层）。
  所有用户输入都经过双引号转义，并通过绑定参数传递，从不拼接进 SQL。
- 1–2 个 UTF-8 字符的查询：第一层使用 `NOCASE` 前缀范围扫描，加上针对最近修改的
  2000 个文件的 `LIKE ... ESCAPE '\'` 扫描；第二层使用 `docs_uni` 前缀匹配（支持
  中日韩文字前缀）。
- 每次查询最多返回 200 条结果。回复写出之前，前 20 个路径用 `faccessat(F_OK)`
  校验；已在磁盘上消失的条目从回复中剔除，并排入数据库删除队列。

## 扫描、排除与实时更新

- **搜索根目录：** `/etc/locachy/rc.conf` 中的 `search_roots`，以冒号分隔的绝对路径。
  未设置时为 `/etc/passwd` 中 uid 1000 用户的主目录。`SIGHUP` 会重新加载配置并开始
  一次全新扫描。
- **排除项：** 隐藏文件和目录（以 `.` 开头：`.git`、`.cache`、`.local/share/Trash` 等）、
  `node_modules`、`__pycache__`，以及 `<根目录>/Drives`。
- **挂载安全：** 扫描器绝不跨越文件系统边界（在 `lstat`/`fstatat` 以及已打开的目录
  描述符上检查 `st_dev`）。它读取 `/proc/self/mountinfo`，跳过 FUSE（`fuse*`，包括
  rclone 的 `~/Drives` 和 `sshfs`）、网络文件系统（`nfs*`、`cifs*`、`smb*`、`davfs`、
  `9p`、`afs`、`ceph`、`glusterfs`）、`autofs` 以及内核虚拟文件系统（`proc`、`sysfs`、
  `devtmpfs`、`devpts`、`cgroup*`、`debugfs`、`tracefs`）。
- **实时更新：** `inotify` 监视的数量上限为 `/proc/sys/fs/inotify/max_user_watches`
  的一半。事件约每 250 毫秒合并为一次 SQLite 事务。`IN_MOVED_FROM`/`IN_MOVED_TO`
  通过 cookie 配对，就地更新 `dirs`/`files`。
- **溢出与离线删除：** 超出监视预算的目录每 10 分钟通过 `timerfd` 重新检查（没有此类
  目录时定时器解除），跳过 `mtime` 未变化的目录。每次完整扫描都会递增代数计数器；
  扫描完成后清理代数较旧的行，并通过增量清空回收页面。

## 电源与文档提取

- 守护进程以 `nice 19` 和 `IOPRIO_CLASS_IDLE` 运行。
- 没有待处理的扫描、批量刷新、移动过期、幽灵清理或提取工作时，`poll()` 无超时阻塞
  （空闲时零唤醒）。
- 文档提取绝不在守护进程内进行。符合条件的文件（文档类型 `pdf`、`docx`、`odt`、
  `epub`，以及文本和代码扩展名；大小从 1 字节到 8 MiB）进入上限 512 的队列，逐个交给
  `/usr/local/bin/locachy-docscan FILE`。它在标准输出写出至多 1 MiB 的 UTF-8 文本，
  成功时以 0 退出。每次运行有 5 秒超时，超时即被结束。
- 提取器在沙箱中运行：降为文件的属主（属主为 root 时降为 `nobody`），设置
  `PR_SET_NO_NEW_PRIVS`，地址空间限制 512 MiB、CPU 时间限制 10 秒，关闭继承的文件
  描述符，并拒绝符号链接。
- 当 `rc.conf` 中 `search_docs="0"`，或者在电池供电且电量低于 20% 时，内容提取暂停
  （读取 `/sys/class/power_supply`，忽略外设电池）。文件名索引继续进行。

## 查询 socket 协议

- **socket：** `/run/locachy/search.sock`（`AF_UNIX`，`SOCK_STREAM`，权限 0666）。
- **访问控制：** `SO_PEERCRED` 只接受 uid `0` 或 uid `1000`；其他对端立即被关闭。
- **请求：** 一行，最多 256 字节，不含 NUL 字节：`Q <范围> <查询>\n`，`<范围>` 为
  `names`、`content` 或 `all`。
- **响应：** 零行或多行结果，之后是 `E\n`。总计最多 200 条结果；先发送第一层行，再
  发送第二层行：`R <层> <类型>\t<路径>\t<名称>\t<摘要>\n`，其中 `<层>` 为 `1` 或 `2`，
  `<类型>` 为 `file`、`text`、`doc`、`image`、`media`、`archive`、`dir` 之一。

## 配置（`rc.conf`）

| 键 | 默认值 | 含义 | make.sh 写入 |
|---|---|---|---|
| `service_search` | `"/usr/local/lib/locachy/locachy-searchd"` | 在进程 1 下启动守护进程 | 守护进程已构建且该键不存在时 |
| `search_roots` | uid 1000 的主目录 | 要索引的绝对路径，以冒号分隔 | 否 |
| `search_docs` | `"1"` | `"1"` 提取文档正文（第二层）；`"0"` 暂停 | 该键不存在时 |
| `key_search` | `"Super+Space"` | 打开搜索的全局快捷键 | 该键不存在时 |

用于测试和手动运行的覆盖项：`LOCACHY_RC_CONF`、`LOCACHY_SEARCH_DB`、
`LOCACHY_SEARCH_SOCK`、`LOCACHY_DOCSCAN_BIN` 和 `LOCACHY_POWER_SUPPLY_DIR`。

升级前的 rc.conf 只有在守护进程二进制存在时才会加入 `service_search`，并通知进程 1
重新读取服务。守护进程未构建时不会删除已有的该行；此时它什么也不启动。

## 状态

- **搜索程序：** `progs/search.c` 已在源码树中。在共享 Wayland 显示上（由
  `init/apps.c` 传入 `LOCACHY_WINDOW_FLOATING=1`），`wayland-backend.c` 通过
  `wlr-layer-shell` 以置顶居中浮窗打开它（`LAYER_OVERLAY`、逻辑尺寸 `760×480`、
  独占键盘焦点、无标题栏；合成器不支持 layer-shell 时回退为无标题栏的
  `xdg_toplevel`），再次按下 `key_search` 时通过 `LOCACHY_REQUEST_FD` 发送
  `close` 切换关闭。在宿主帧缓冲上，它在压暗背景上绘制居中浮窗卡片，支持点击卡片外
  区域或按 `Esc` 关闭。不依赖硬件的测试已在构建主机通过（`test-search-ui`、
  `test-search-index`、`test-docscan`）；目标机实机界面尚未验证。只要该文件存在，
  make.sh 和 `build-check.sh` 就从它构建 `search`（以及 `search-window`）。
- **共享显示：** `link/labwc-rc.xml` 把 `SEARCH-KEY` 绑定到
  `locachy-seat --window search`，seat 接受程序 `search`；设置写出 labwc 配置时
  （`labwc_line`）用 `key_search` 替换 `SEARCH-KEY`。seat 自己的回退（仅在
  `settings --apply` 失败时使用）把 `SEARCH-KEY` 绑定为默认的 `W-space`（Super+Space）。
- **宿主帧缓冲：** 进程 1 处理 `K_ACT_SEARCH`（`init/init.c`）：在焦点屏幕上打开
  搜索；会话已锁定、被共享显示占用，或焦点任务占用屏幕时忽略。`search.c` 本身也响应同一个
  键。其他帧缓冲程序都不分发它。
- **设置：** 设置程序有全局搜索键（可编辑，带冲突检查，也可删除）、搜索索引开关和文档内容
  索引（`search_docs`）的行。`search_roots` 没有设置行：请在 `rc.conf` 中修改。
- **构建检查：** `tools/build-check.sh` 在构建主机上以 `-Werror` 构建搜索候选程序。


## 测试

- `tools/test-search-index.c`：可移植引擎的测试，覆盖编译指示（WAL、
  `auto_vacuum=INCREMENTAL`、`user_version`）、外来或版本不符的数据库、目录与文件
  移动、递归删除子树、1–2 个字符与 3 个及以上字符的 ASCII 和中文查询、
  `contentless_delete=1` 的更新、FTS5 与 SQL 转义、200 条结果上限、代数清理、幽灵
  过滤和过期剪除。
- `tools/test-docscan.c` 与 `tools/test-docscan.sh`：按上文的 MuPDF 选择构建
  `locachy-docscan`（`--require-pdf` 在没有 MuPDF 时失败），并运行提取器的测试。
