# Wayland 窗口中程序的剪贴板与拖放

[English](WAYLAND-DATA.md)

直连程序在 Wayland 窗口中显示（`LOCACHY_BACKEND=wayland`）时，使用该窗口的标准
数据设备（`base/wayland-data.c`）：剪贴板和拖放用 `wl_data_device`，主选区用
`zwp_primary_selection_device_v1`。不使用合成器私有协议，也不经宿主剪贴板 socket。
同一程序在直连屏幕上仍使用宿主剪贴板服务。

## 选区

- `clipboard_set`/`clipboard_set_png` 以窗口最近的键盘或指针 serial 提供文本
  （`text/plain;charset=utf-8`、`UTF8_STRING`、`text/plain`）或 `image/png`。
- `clipboard_get` 与宿主服务一样，返回带确切大小的密封 memfd。读取其他客户端的
  选区时，从其管道读取，并沿用宿主服务的限制：2.5 秒（超时为 BUSY）和 64 MiB。
  程序自己的选区直接从内存返回：向合成器索取会等待同一个事件循环。
- 其他客户端的请求以非阻塞方式写出：同时最多 16 个，每个在 3 秒期限、写完或读取方
  关闭时结束。读取方先关闭不会产生 SIGPIPE。

## 拖到其他程序

在 files 中，离开窗口的项目拖动会变成标准拖放：百分号编码的 `file://` URI 组成的
`text/uri-list`，使用真实的按下 serial，只复制（文件不会移动或消失）。窗口目前还不
是放置目标：拖到它上面的内容会被拒绝。

真实路径（解析符号链接后）位于共享文件夹中的文件，即 `user_dirs`（默认是
Documents、Downloads、Music、Pictures、Videos）和 Drives 中的文件，以其自身路径
提供：隔离程序在相同路径看到这些文件夹。

用户主目录中的其他文件，作为宿主文档服务的文档提供（[DOCUMENTS.zh.md](DOCUMENTS.zh.md)）。
拖放开始前，files 以用户身份打开每个文件（`O_PATH`，像用户的路径那样跟随链接），交给
文档服务；服务把它做成一个暂时没有任何程序可读的文档。随后拖放提供以下内容（只构建
一次）：

- `text/uri-list`：这些文件用文档路径（`/run/user/UID/doc/ID/NAME`），共享文件用
  真实路径；私人文件的真实路径从不提供；
- `application/vnd.portal.filetransfer` 和 `application/vnd.portal.files`：同样这些
  路径组成的密钥，容器里的 `RetrieveFiles` 会把它解码。

只有放置目标所在的程序可以读取：合成器在放行放置之前先询问文档服务。拖放结束时，
files 告知服务（目标读取了放置的内容时为 `end SESSION done`，否则为 `cancelled`），
然后关闭连接。这种拖放需要数据设备版本 3，因为该版本会告知拖放结束。

全有或全无：文档服务没有运行，或拒绝其中任何一个文件时，拖放不会开始，并提示原因。
主目录之外的文件（指向 `/etc` 的链接、在主目录外浏览的文件夹）会被拒绝。

## 检查

- `tools/test-wayland-data.sh 新目录`（在开发根中以 root 运行）：写队列单元检查
  （普通与 ASan/UBSan 两种构建），包括带密钥的拖放（各类型的字节、结束只告知一次）；
  随后在真实无头 labwc 中创建窗口，用虚拟键盘和指针：
  与 data-control 客户端双向收发选区、主选区、`image/png`、不写数据的来源以 BUSY
  结束、清除、拖放到 foot（由 foot 读取 URI），以及放到窗口自身时被取消。未定义
  `CONFIG_WAYLAND` 构建时，该文件不需要任何 Wayland 库。
- `tools/test-files.sh`：直连屏幕上拖过窗口边缘的项目仍是内部拖动；在窗口中，共享
  文件以按下 serial 和编码后的 URI 启动一次拖放；指向主目录之外的链接被拒绝；文档服务
  未运行、或服务拒绝时，私人文件被拒绝；配合替身服务时，发送的是该文件的 O_PATH 描述符，
  URI 列表和密钥中是其文档路径（从不是它自己的路径），拖放的结束（done、cancelled）
  会告知服务。

真实浏览器接收拖入文件的验收不在这些检查之内。
