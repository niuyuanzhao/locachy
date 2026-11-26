# Windows 安装镜像（`locachy-winiso`）

[English](WINISO.md)

Windows 虚拟机应用（[VM-APPS.zh.md](VM-APPS.zh.md)）和维护用 Windows（[WINMAINT.zh.md](WINMAINT.zh.md)）都要用到两张光盘镜像：Windows 安装镜像和 virtio-win 驱动镜像。这两张都由 `locachy-winiso` 提供，用户不必自己找路径：

- **virtio-win** 来自 AUR 软件包 `virtio-win`（属于 `winmaint` 模块）。它的构建配方从 Fedora 的 `fedorapeople.org` 归档下载一个固定版本，用 PKGBUILD 里写定的 SHA-256 核对，然后装到 `/var/lib/libvirt/images/virtio-win.iso`。新版本随软件包更新而来，LOCachy 自己不去下载。
- **Windows** 由 `locachy-winiso fetch windows` 从微软下载，只有 SHA-256 与微软公布的值一致才会留下。

## 命令

```
locachy-winiso path windows|virtio-win [--lang L]
locachy-winiso fetch windows [--lang L]
locachy-winiso check [--lang L]
```

- `path virtio-win` 输出软件包里的那张镜像；`path windows` 输出最近一次成功的 `fetch` 核对过的 Windows 11 镜像（带 `--lang` 时输出对应语言的那张）。成功时标准输出只有一行绝对路径、退出码为 0；镜像不在时退出码为 3。`locachy-app`（Windows 自定义虚拟机）和 `locachy-winmaint` 都靠它取路径。
- `fetch windows` 下载 Windows 11。语言依次取 `--lang`、`/etc/locachy/rc.conf` 里的 `language`，都没有就用 `en-US`。语言标签（`de-DE`）和 locale 写法（`zh_CN.UTF-8`）都认；只写语言不写地区时，取微软为该语言提供的主要版本（`zh` 即 `zh-CN`，`pt` 即 `pt-PT`）。微软没有该语言的镜像时：用 `--lang` 指定的直接报错；来自 rc.conf 的改用 `en-US`，并明确提示。

- `check [--lang L]` 按相同规则选择语言，检查微软的下载流程，但不下载或安装镜像。不需要镜像目录的写权限，也不提交系统任务。

退出码：0 完成，1 本地失败，2 用法错误，3 镜像不在（`path`），4 微软拒绝或服务响应异常（`fetch` 或 `check`），5 下载的镜像 SHA-256 不符（`fetch`）。

## `fetch` 的步骤

每次执行只走一遍请求链，每步只发一次。微软拒绝或任一响应未返回成功时，请求链立刻结束：不重试，不改请求头、User-Agent 或会话去绕过，也不试其他地址、语言或版本。

1. 读微软的下载页（`https://www.microsoft.com/en-us/software-download/windows11`）。页面上“Verify your download”表格给出该语言对应的 SHA-256（行名如 `English 64-bit`、`Chinese Simplified 64-bit`）；找不到恰好一个值，就不发后续请求。页面同时给出要查询的产品版本编号（`option value`）。
2. 如果该语言已经留有一张 Windows 11 镜像，且 SHA-256 正是第 1 步取到的值，到此结束，不再发第 3 到第 7 步的请求。
3. 登记会话：每次拉取开始时，先清理 `.partial/dl/` 下遗留的 `.session.*.cookies` 普通文件（用 `lstat` 检查，不跟随符号链接，以 root 运行时仅限当前进程 uid 与 `nobody` 所拥有的文件）。随后请求会话许可（`vlscppe.microsoft.com/tags`），带上组织编号和新的会话编号；再完成 `ov-df.microsoft.com` 的两步会话检查（`mdt.js` 及其回复）。整条请求链共用 `.partial/dl/` 下的临时 cookie 文件（链接步骤结束后即删），确保微软下载服务看到同一会话。每次元数据与接口请求都设有 20 秒连接超时和 60 秒总超时。
4. 用同一会话查询该产品版本的语言列表（`getskuinformationbyproductedition`），取出对应语言的 SKU。
5. 用同一会话、以微软下载页为 Referer 申请下载链接（`GetProductDownloadLinksBySku`）。Sentinel 拒绝先从解析后的 JSON `Errors` 条目（`Key`、`Value` 或 `Type` 字段）判断，仅在响应不是合法 JSON 时才退回原文文本匹配。在 `ProductDownloadOptions` 中，优先选择 `DownloadType == 1` 的选项；只有不存在此类选项时，才退回匹配 URL 路径（不含查询串）里不区分大小写包含 `x64` 的选项；若候选项不是恰好一个则直接拒绝。取出的 x64 链接一天内有效。
6. curl（以 root 运行时降为 `nobody`）把文件下载到 `/var/lib/locachy/isos/.partial/dl/`（连接超时 30 秒）；同一构建版本之前没下完的部分会接着下。传输中途断开时本次 fetch 直接结束（退出码 4），不立即重试；已下完的部分留在磁盘上，下次 `fetch` 接着下。
7. root 不跟随符号链接打开该文件，确认它是只有单个硬链接的普通文件，通过这个文件描述符计算 SHA-256 并与微软公布的值比对。不一致就删掉下载内容（退出码 5）。一致则把已核对的同一个 inode 硬链接到 `/var/lib/locachy/isos/windows-11-语言.iso`（root 所有、权限 0644，虚拟机所用的用户可以读取），删除 `.partial/dl/` 里的旧名字，写入记录文件 `windows-11-语言.iso.json`（SHA-256、大小、微软的文件名、表格行名、时间），并让 `windows.iso` 指向它。

以 root 运行时，所有联网步骤都降为 `nobody` 执行：下载页、会话登记、下载服务接口以及镜像传输。`nobody` 只能写入 `.partial/dl/`；root 绝不信任 `nobody` 能偷换的路径，只核对并安装它已经打开的那个 inode。已经持有写入描述符的 `nobody` 进程，在 root 接管文件之后仍能继续写入（改属主不会收回已打开的描述符），因此可能在哈希之后改动内容；这一残余风险如实记录，尚未消除。所有联网步骤都在 `locachy-net` 里进行，经过当前开启的各网络层。每次请求的 User-Agent 都是 `winiso/1`，不携带任何关于系统、主机、个人或邮箱的信息。

**没有第二个来源。** 微软拒绝自动下载时（经 Tor、VPN 或某些网络时很常见，响应里会带有“Sentinel marked this request as rejected”，或直接返回错误状态），`fetch` 以退出码 4 停止，并提示用户用浏览器下载镜像、改为提供路径（虚拟机应用写 `vm_iso="..."`，维护用 Windows 用 `locachy-winmaint vm --iso 路径`）。它绝不会改从镜像站或其他网站下载。

## `check` 的步骤

即使本机已有镜像，`check` 仍会执行第 1、3、4、5 步。会话 cookie 放在独立的临时目录里，取链接的步骤结束后即清理。它不读取或改动已安装的镜像、`.partial/` 或 `windows.iso`。

确认 x64 链接以允许的微软 URL 前缀开头后，curl 只发送一次带 `Range: bytes=0-0` 的 GET 请求，使用 `winiso/1`，连接超时 20 秒，总超时 60 秒。有效的 HTTP 206 必须给出 `Content-Range: bytes 0-0/<size>`，且实际传输恰好一个字节。若服务器忽略范围返回 HTTP 200，curl 会限制体积，检查失败且不保存镜像。这一步不跟随重定向、不重试，也不换下载来源。成功时标准输出给出语言、镜像文件名、微软公布的 SHA-256 和 HTTP 状态，如 `ok: windows-11 en-US <filename> sha256=<64hex> range=206`。这并不核对镜像内容。遇到微软拒绝、意外的 HTTP 状态或无法解析的响应时，以退出码 4 结束。

## 系统任务

在运行中的系统上，`fetch` 作为固定的系统任务 `winiso-fetch`（见 [JOBS.zh.md](JOBS.zh.md)，已登记在 `progs/locachy-jobs.c`）运行：执行 `locachy-winiso fetch windows --lang 语言`，语言在提交任务前就已确定。终端会跟随任务输出；关掉终端不会中断下载。若本身已在任务中，或处于离线环境（chroot），则直接在当前进程执行。任务服务拒绝或不在时一律算失败，绝不绕开队列直接执行。

## 文件

| 路径 | 内容 |
| --- | --- |
| `/var/lib/locachy/isos/windows-11-en-US.iso` | 已核对的 Windows 11 镜像（0644；仅接受 `windows-11-<语言>.iso` 命名） |
| `/var/lib/locachy/isos/windows-11-en-US.iso.json` | 它的记录；`path` 需要它，且大小必须相符 |
| `/var/lib/locachy/isos/windows.iso` | 符号链接，指向 `path windows` 输出的那张 |
| `/var/lib/locachy/isos/.partial/` | root 0711（`nobody` 可穿过但不可列出）：记录文件 `*.part.json` 与 `.lock`（root 0600） |
| `/var/lib/locachy/isos/.partial/dl/` | `nobody` 0700：传输中的 `<名称>.part` 及本次拉取用的 `.session.*.cookies`（拉取开始时会清理遗留的普通 cookie 文件） |
| `/var/lib/libvirt/images/virtio-win.iso` | virtio-win 软件包里的镜像 |

同一语言出了新的构建版本时，会替换掉旧文件。

## 软件包与许可证

| 软件包 | 位置 | 来源 | 许可证 |
| --- | --- | --- | --- |
| `virtio-win` | AUR（`winmaint` 模块） | `fedorapeople.org/groups/virt/virtio-win/direct-downloads/archive-virtio/virtio-win-版本/virtio-win.iso`，SHA-256 写在 PKGBUILD 里 | BSD-3-Clause（软件包） |

该软件包直接按 AUR 配方构建，不做改动；它不在 Arch 官方仓库里。下载 Windows 镜像不需要额外安装其他软件包：申请链接的步骤已写在 `locachy-winiso` 里，直接读取微软的页面和接口响应。

**致谢。** 申请链接的请求顺序参考了 Fido（github.com/pbatard/Fido，GPL-3.0 许可证，仅支持 Windows）、Mido（github.com/ElliotKillick/Mido，MIT 许可证）、dockur/windows（github.com/dockur/windows，`src/mido.sh`，MIT 许可证，持续维护）和 quickget（github.com/quickemu-project/quickemu，MIT 许可证，quickemu 项目）及其所使用的微软页面常量。这些项目既不安装也不运行，只作参考；若微软调整接口流程，这边随之跟进。

## 测试接口

仅供测试的环境变量（不会被当作后备来源使用）：`LOCACHY_ISO_DIR`、`LOCACHY_VIRTIO_WIN_ISO`、`LOCACHY_RC_CONF`、`LOCACHY_WINISO_MS_BASE`（下载页与下载服务接口）、`LOCACHY_WINISO_TAGS_BASE`（会话许可）、`LOCACHY_WINISO_OVDF_BASE`（`ov-df` 会话检查）和 `LOCACHY_WINISO_LINK_PREFIX`（允许的镜像链接前缀）；后四项只接受 `https://`。

## 测试覆盖范围

`tools/test-winiso.py` 运行真实的程序和 curl，对面是一个本地 HTTPS 服务器，扮演微软：带表格的下载页、会话许可、`ov-df` 两步会话检查、会话 cookie、语言列表、下载链接（或拒绝响应）和支持分段请求的镜像地址，另有任务队列的替身。覆盖：`path`、走完整请求链的正常下载、会话 cookie 传递与临时及遗留 cookie 文件清理（不跟随符号链接）、从解析后的 JSON `Errors` 条目及非 JSON 原文退回路径识别 Sentinel 拒绝（退出码 4，不重试）、非 Sentinel 的 JSON 错误与 HTTP 错误响应、x64 链接挑选（优先 `DownloadType == 1`，退回不区分大小写匹配 URL 路径中的 `x64` 且忽略查询串，候选不唯一时拒绝）、`windows-11-*` 镜像命名限制、元数据请求携带 `--connect-timeout 20`、SHA-256 不符时删除、传输中断时不重试并留待下次续传、续传之前未下完的文件、丢弃其他构建版本的残留文件、已下完的残留文件（HTTP 416）、任务队列路径、请求头与外发内容（本地服务器逐条检查通用客户端名称、会话编号、cookie、Referer，以及不含项目名、当前运行用户名、主机名和 `@`），以及源码致谢。另外覆盖：两级目录的权限布局、被放置的符号链接与硬链接会被拒绝、基于文件描述符的安装（同一个 inode；打开后被取消链接、替换或改名挪走的部分文件会被拒绝），以及降权的参数顺序（静态检查，并用替身模拟降权）。未在离线测试里运行：真正的降权（真实 root 下用 `setpriv` 降为 `nobody`，以及 `nobody` 看到的 root 所有 `.partial/` 的情况），需要真实 root 的主机，标记为未验证。

`tools/test-winiso.py` 默认只运行离线测试。`check` 的测试覆盖语言选择、完整请求链和单字节 HTTP 206 响应、服务器忽略范围时受大小上限约束的 HTTP 200 响应、Sentinel 拒绝和 HTTP 403、404 等意外状态下不重试、以及链接前缀检查。测试还确认镜像目录不存在或只读时仍能运行，不提交系统任务，不泄露主机或个人标识，并清理临时 cookie。

从仓库根目录执行 `python3 src/tools/test-winiso.py --live`（或设 `LOCACHY_WINISO_LIVE=1`）时，离线测试结束后会对微软实际运行一次 `locachy-winiso check --lang en-US`。结果会区分成功、微软拒绝或服务错误、以及响应格式或解析错误。线上结果取决于微软服务和当前网络，不影响离线测试的结论。
