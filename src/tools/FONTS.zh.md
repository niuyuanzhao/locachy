# 字体

[English](FONTS.md)

LOCachy 自己的程序不写死任何字体文件。进程 1 和其他静态 musl 程序只画 Terminus
（PSF），从不链接 fontconfig。用 FreeType 构建的 glibc 程序（`CONFIG_FREETYPE`，
`make.sh`）找其他字体的方式与 GTK、Qt、foot 相同，都通过 fontconfig
（`base/fontmatch.c`）：

- **主字体**：fontconfig 对 `monospace` 的匹配结果，经过系统和用户的配置
  （`/etc/fonts/conf.d`、`~/.config/fontconfig/fonts.conf`）以及区域设置的语言。
  内容字体（`base/font.c`，编辑器）使用它；也可以给 `content_font_new` 一个明确的
  文件。粗体、斜体、粗斜体是同一字族里真实存在的字形（fontconfig 有时才用），
  不借用别的字族，也不合成。
- **回退**：主字体缺少的字符，取 fontconfig 为同一模式排序的列表
  （`FcFontSort`，已裁剪）里第一个字符集含有它的字体，与 fcft（foot）的做法相同。
  屏幕文字（`base/screen.c`：Terminus 没有的字符）和顶栏（`base/panel.c`：宽字符）
  使用同一查找。任何已安装的 CJK 字族都可以：Noto CJK、文泉驿、思源、文鼎。
- **开销**：fontconfig 每个进程只初始化一次，在第一个需要它的字形时进行。列表在
  第一次使用时排序一次。字形第一次需要某个字体时才打开它。哪个字体含有某个字符会
  记住（每个列表 256 格），包括"都没有"，所以绘制时不再重复查找。在一台
  Intel i7-8086K、装有 1188 个字体的台式机上实测：`FcInit` 10–35 ms，一次排序 1.6–4 ms。这些程序以任意像素
  大小混合灰度覆盖率，所以跳过纯位图字体和彩色字体。

## 其他系统怎么做

- **Ubuntu** 默认安装 CJK 字体。平台种子
  [desktop-common](https://git.launchpad.net/~ubuntu-core-dev/ubuntu-seeds/+git/platform/plain/desktop-common?h=stonking)
  把 `(fonts-noto-cjk)` 列为 `ubuntu-desktop` 的 Recommends。按语言的支持来自
  language-selector：
  [`check-language-support -l LANG`](https://manpages.ubuntu.com/manpages/noble/man1/check-language-support.1.html)
  根据
  [data/pkg_depends](https://git.launchpad.net/ubuntu/+source/language-selector/tree/data/pkg_depends)
  （`fn:zh-hans::fonts-noto-cjk`、`im:zh-hans::fcitx5`、`im:zh-hans:ibus:ibus-libpinyin` 等）
  列出缺少的包，安装程序会装上它们
  （[subiquity locale.py](https://raw.githubusercontent.com/canonical/subiquity/main/subiquity/models/locale.py)）。
  language-selector-common 还启用 fontconfig 文件
  （[links](https://git.launchpad.net/ubuntu/+source/language-selector/plain/debian/language-selector-common.links)）：
  - `64-language-selector-cjk-prefer.conf`：不判断语言，按 JP、KR、SC、TC、HK 优先；
  - `69-language-selector-zh-cn.conf`：语言为 `zh-cn` 时把 Noto Sans CJK SC 放到最前。
- **Fedora** 工作站的 `fonts` comps 组默认安装 `default-fonts-cjk-{sans,serif,mono}`
  （[comps](https://forge.fedoraproject.org/releng/fedora-comps/raw/branch/main/comps-f45.xml.in)）。
  `langpacks-zh_CN` 依赖两个包
  （[mdapi](https://mdapi.fedoraproject.org/rawhide/pkg/langpacks-core-zh_CN)）：
  - `langpacks-core-zh_CN`，它会带来 `ibus-libpinyin`（图形界面系统上）和
    `default-fonts-zh_CN`；
  - `langpacks-fonts-zh_CN`。

  Noto CJK 包的
  [fontconfig 文件](https://src.fedoraproject.org/rpms/google-noto-sans-cjk-vf-fonts/raw/rawhide/f/65-google-noto-sans-cjk-vf-fonts.conf)
  让每个地区字形只声明自己的语言，并按语言前置字族。
- **Arch** 的 [base](https://archlinux.org/packages/core/any/base/) 不含任何字体。
  [noto-fonts-cjk](https://archlinux.org/packages/extra/any/noto-fonts-cjk/files/)
  不带 fontconfig 文件。它在 2024 年删掉了源自 Debian 的 `70-noto-cjk.conf`，因为
  `binding="strong"` 会压过用户自己的回退顺序
  （[提交 95d88e4d](https://gitlab.archlinux.org/archlinux/packaging/packages/noto-fonts-cjk)）。
  [Localization/Simplified Chinese](https://wiki.archlinux.org/title/Localization/Simplified_Chinese)
  （"Chinese characters displayed as variant (Japanese) glyphs"）给出一个
  `64-language-selector-prefer.conf`：Noto Sans/Serif/Sans Mono CJK 先 SC，再 TC，再 JP。
  [Fonts](https://wiki.archlinux.org/title/Fonts) 说明：默认字体缺字时 fontconfig
  会换用别的字体；在中文区域设置下，设 `FC_LANG=en-US:zh-CN` 可让拉丁文字仍用拉丁字体。
- **fontconfig** 在替换阶段以弱绑定加入区域设置的语言（依次取 `FC_LANG`、`LC_ALL`、
  `LC_CTYPE`、`LANG`；
  [fontconfig-user](https://www.freedesktop.org/software/fontconfig/fontconfig-user.html)、
  `src/fccfg.c`、`src/fcdefault.c`）。语言的优先级高于 `<alias><prefer>` 加入的弱绑定
  字族（`src/fcmatch.c`）。
  [FcFontSort](https://www.freedesktop.org/software/fontconfig/fontconfig-devel/fcfontsort.html)
  开启裁剪时去掉不增加覆盖的字体。`60-latin.conf` 把 Noto Sans Mono 作为第一个
  monospace。在 Noto CJK 各字族中，`65-nonlatin.conf` 只列了韩文的。
- **应用程序**：[fcft](https://codeberg.org/dnkl/fcft)（foot）用 `FcFontSort` 加
  `FcCharSetHasChar` 回退，LOCachy 现在也这样做。
  [Pango](https://gitlab.gnome.org/GNOME/pango/-/raw/main/pango/pango-language.c)
  对汉字没有样本语言；除非 `PANGO_LANGUAGE`/`LANGUAGE` 指定，否则由字族顺序决定。

用 Arch 的包实测（fontconfig 2.18.3，noto-fonts-cjk 20240730）：在 `monospace` 下，
汉字在所有区域设置（C、en_US、zh_CN、zh_TW、ja_JP）都来自
**Noto Sans Mono CJK KR**。加上 Wiki 的优先文件后，来自 Noto Sans Mono CJK SC。
在 zh_CN 区域设置下，fontconfig 还会让 CJK 等宽字体成为拉丁文字的主字体（即上面
`FC_LANG` 那条提示所说的情况）。

## LOCachy 的做法

- **不加 LOCachy 自己的字体设置。** 设置就是 fontconfig 的设置，上面的系统都如此，
  也都没有为这件事设计按程序区分的字体设置。内容字体仍可指定明确的文件
  （`content_font_new`）。
- **中文拼音依赖 CJK 字体。** 这仿照 Ubuntu 的语言支持和 Fedora 的 langpacks，它们
  安装输入法时会一并带上字体。`link/modules.json` 把输入法声明为单独选择的模块
  （`ime`、`ime-zh`，`"choice": "ime"`，不出现在安装程序的模块列表里）。`ime-zh` 有
  `"requires": ["ime", "cjk-fonts"]`。这一依赖在三处生效：
  - 安装程序（`make-root.sh`，`WITH_IME=zh`）；
  - 设置：打开中文拼音时，若 fontconfig 没有 `zh` 字体，就一并加入 CJK 字体模块的包；
  - 更新：`declare-packages` 对新出现的依赖声明一次；用户之后删掉的保持删除。

  删除中文拼音（`locachy-pkg remove`）不会安装任何东西。若没有其他已声明的模块依赖
  CJK 字体模块、它的包都未安装，且它只是作为拼音的依赖被声明（更新写入的
  `(required by ime-zh)` 一组）、从未被单独选择，则一并取消它的声明。已安装或单独
  选择的字体保留，与 Ubuntu 和 Fedora 一样（它们也用于阅读中文），由它自己的那一行
  删除。之后的更新只在拼音存在时才再次声明这一依赖。

  CJK 字体模块带有 `"fontconfig_lang": "zh"`：只要 fontconfig 有中文字体（相当于
  `fc-list :lang=zh`），不论是哪个字族，它就算已安装。设置据此显示，设置和
  `declare-packages` 也都不再加入 noto-fonts-cjk。输入法任务结束后，设置向
  `locachy-bar` 发 SIGHUP，顶栏不重启共享显示，重新加载两样东西：
  - Rime，仅当中文方案或 Rime 部署有变化时（`ime_reload`）；
  - 它的 CJK 字体，仅当 fontconfig 的配置文件或字体目录有变化时
    （`panel_fonts_reload`，`FcConfigUptoDate`）。
- **汉字用哪种字形**：`link/64-locachy-cjk-prefer.conf`，即 Arch Wiki 的优先顺序
  （先简体中文，再繁体中文，再日文）。`make.sh` 和安装介质把它装到
  `/etc/fonts/conf.d`。这保持了 LOCachy 以前的显示：那时程序打开固定文件里的 SC 字形。
  它和所有 `<alias>` 一样在 64 位置弱绑定，所以用户自己的 `fonts.conf`（50）仍然在前。
  它只写 Noto CJK 字族；换成其他 CJK 字体时，就按 fontconfig 的排序使用那个字体。

## 选用其他字体

写在 `~/.config/fontconfig/fonts.conf`；要对所有用户生效则写在 `/etc/fonts/local.conf`：

```xml
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <alias>
    <family>monospace</family>
    <prefer>
      <family>DejaVu Sans Mono</family>
      <family>WenQuanYi Zen Hei Mono</family>
    </prefer>
  </alias>
</fontconfig>
```

修改之后启动的程序会使用它。`fc-match monospace` 与 `fc-match monospace:charset=4e2d`
显示程序将得到的字体。

## 测试

`tools/test-fonts.sh`（在 `test-interaction-regressions.sh` 中）用本机已安装的字体搭建
夹具配置（`tools/font-fixtures.sh`）。每个夹具有自己的字体目录和缓存，并隔离 `HOME`。
它检查以下情形，普通构建和 ASan/UBSan 各跑一遍：

- 配置的 monospace 是主字体；
- 有 LOCachy 的文件时汉字来自 Noto Sans Mono CJK SC，没有时来自 KR；
- 用户更早的偏好优先（Liberation Mono、JP）；
- 其他 CJK 字族可用（已安装时为文鼎或思源）；
- 没有 CJK 字体时不出错：没有字形、两格 `.notdef`、顶栏显示 `?`、没有 `zh` 字体；
- 顶栏启动后才安装的 CJK 字体，在顶栏重新加载后能画出来。

`tools/test-content-font.sh` 在同样的夹具里运行内容字体的测试。
`tools/test-declare-packages.py` 检查 modules.json、`make-root.sh` 和设置中的依赖；
`tools/test-pkg-sync.py`（`RequirementRemoval`）检查随拼音删除时的处理。

在安装 Rime 之前构建的顶栏没有编入输入法。pacman 钩子会在它运行时构建新的
`locachy-bar`。收到设置的 SIGHUP 时，顶栏发现自己的可执行文件已是新文件，就原地运行它
（同一 PID、同样的 argv 和环境）。屏幕上有选择框、提问、通知或正在输入的组合时，它会
等这些结束；哪些状态带过去、哪些重新建立，见 `progs/bar.c`。
`tools/test-bar-keyboard.c` 测试了这一点。

这些测试没有覆盖：设置任务结束后，真实共享显示里的顶栏重新加载 Rime 并原地重启自己。
