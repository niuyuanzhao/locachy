# Fonts

[简体中文](FONTS.zh.md)

LOCachy's own programs name no font file. Process 1 and the other static
musl programs draw Terminus (PSF) only and never link fontconfig. The glibc
programs built with FreeType (`CONFIG_FREETYPE`, `make.sh`) find every other
font the way GTK, Qt and foot do, through fontconfig (`base/fontmatch.c`):

- **Primary face**: fontconfig's match for `monospace`, after the system's
  and the user's configuration (`/etc/fonts/conf.d`, `~/.config/fontconfig/fonts.conf`)
  and the locale's language. The content font (`base/font.c`, the editor)
  uses it, or an explicit file given to `content_font_new`. Bold, oblique and
  bold oblique are that family's real faces when fontconfig has them, never
  another family's and never synthesized.
- **Fallback**: a character the primary face lacks comes from the first font
  in fontconfig's sorted list for the same pattern (`FcFontSort`, trimmed)
  whose charset has it. This is what fcft (foot) does. The screen text
  (`base/screen.c`: what Terminus lacks) and the bar (`base/panel.c`: wide
  characters) use the same lookup. Any installed CJK family works: Noto CJK,
  WenQuanYi, Source Han, AR PL.
- **Cost**: fontconfig is initialised once per process, at the first glyph
  that needs it. A list is sorted once, at its first use. A face opens when a
  character first needs it. Which font has a character is remembered (256
  slots per list), including "none", so drawing never looks anything up again.
  Measured on an Intel i7-8086K desktop with 1188 fonts: `FcInit` 10–35 ms, one sort 1.6–4 ms.
  Bitmap-only and colour fonts are skipped, because these programs blend grey
  coverage at any pixel size.

## How other systems do it

- **Ubuntu** installs CJK fonts by default. The platform seed
  [desktop-common](https://git.launchpad.net/~ubuntu-core-dev/ubuntu-seeds/+git/platform/plain/desktop-common?h=stonking)
  lists `(fonts-noto-cjk)` as a Recommends of `ubuntu-desktop`. Per-language
  support comes from language-selector:
  [`check-language-support -l LANG`](https://manpages.ubuntu.com/manpages/noble/man1/check-language-support.1.html)
  lists the missing packages, using
  [data/pkg_depends](https://git.launchpad.net/ubuntu/+source/language-selector/tree/data/pkg_depends)
  (`fn:zh-hans::fonts-noto-cjk`, `im:zh-hans::fcitx5`, `im:zh-hans:ibus:ibus-libpinyin`, …).
  The installer installs them
  ([subiquity locale.py](https://raw.githubusercontent.com/canonical/subiquity/main/subiquity/models/locale.py)).
  language-selector-common also enables fontconfig files
  ([links](https://git.launchpad.net/ubuntu/+source/language-selector/plain/debian/language-selector-common.links)):
  - `64-language-selector-cjk-prefer.conf` prefers JP, KR, SC, TC, HK with
    no language test;
  - `69-language-selector-zh-cn.conf` prepends Noto Sans CJK SC when the
    language is `zh-cn`.
- **Fedora** Workstation's `fonts` comps group installs
  `default-fonts-cjk-{sans,serif,mono}` by default
  ([comps](https://forge.fedoraproject.org/releng/fedora-comps/raw/branch/main/comps-f45.xml.in)).
  `langpacks-zh_CN` requires two packages
  ([mdapi](https://mdapi.fedoraproject.org/rawhide/pkg/langpacks-core-zh_CN)):
  - `langpacks-core-zh_CN`, which pulls `ibus-libpinyin` (on graphical
    systems) and `default-fonts-zh_CN`;
  - `langpacks-fonts-zh_CN`.

  The Noto CJK package's
  [fontconfig file](https://src.fedoraproject.org/rpms/google-noto-sans-cjk-vf-fonts/raw/rawhide/f/65-google-noto-sans-cjk-vf-fonts.conf)
  restricts each regional face to its own languages and prepends per language.
- **Arch** installs no fonts in [base](https://archlinux.org/packages/core/any/base/).
  [noto-fonts-cjk](https://archlinux.org/packages/extra/any/noto-fonts-cjk/files/)
  ships no fontconfig file. It dropped its Debian-derived `70-noto-cjk.conf`
  in 2024 because `binding="strong"` overrode the user's fallback order
  ([commit 95d88e4d](https://gitlab.archlinux.org/archlinux/packaging/packages/noto-fonts-cjk)).
  [Localization/Simplified Chinese](https://wiki.archlinux.org/title/Localization/Simplified_Chinese)
  ("Chinese characters displayed as variant (Japanese) glyphs") prescribes a
  `64-language-selector-prefer.conf` that prefers Noto Sans/Serif/Sans Mono
  CJK SC, then TC, then JP.
  [Fonts](https://wiki.archlinux.org/title/Fonts) notes that fontconfig
  switches to another font for characters the default lacks. It also says
  that in a Chinese locale `FC_LANG=en-US:zh-CN` keeps Latin text in a Latin
  font.
- **fontconfig** adds the locale's language weakly while substituting
  (`FC_LANG`, then `LC_ALL`, `LC_CTYPE`, `LANG`;
  [fontconfig-user](https://www.freedesktop.org/software/fontconfig/fontconfig-user.html),
  `src/fccfg.c`, `src/fcdefault.c`). The language outranks the weak families
  that `<alias><prefer>` adds (`src/fcmatch.c`).
  [FcFontSort](https://www.freedesktop.org/software/fontconfig/fontconfig-devel/fcfontsort.html)
  with trim drops fonts that add no coverage. `60-latin.conf` makes Noto Sans
  Mono the first monospace. Of the Noto CJK families, `65-nonlatin.conf`
  names only the Korean ones.
- **Applications**: [fcft](https://codeberg.org/dnkl/fcft) (foot) falls back
  through `FcFontSort` with `FcCharSetHasChar`. LOCachy now does the same.
  [Pango](https://gitlab.gnome.org/GNOME/pango/-/raw/main/pango/pango-language.c)
  has no sample language for Han. Unless `PANGO_LANGUAGE`/`LANGUAGE` names
  one, the family order decides.

Measured with Arch's packages (fontconfig 2.18.3, noto-fonts-cjk 20240730):
in `monospace`, Han comes from **Noto Sans Mono CJK KR** in every locale (C,
en_US, zh_CN, zh_TW, ja_JP). With the Wiki's prefer file it comes from
Noto Sans Mono CJK SC. In a zh_CN locale fontconfig also makes the CJK
monospace the primary face for Latin text (the `FC_LANG` tip above).

## What LOCachy does

- **No LOCachy font setting.** The setting is fontconfig's, as on every
  system above. None of them has a per-program font setting for this. An
  explicit file for the content font stays possible (`content_font_new`).
- **Chinese Pinyin requires the CJK fonts.** This follows Ubuntu's language
  support and Fedora's langpacks, which bring fonts with the input method.
  `link/modules.json` declares the input method as modules of its own choice
  (`ime`, `ime-zh`, `"choice": "ime"`, which are not in the installers'
  module lists). `ime-zh` has `"requires": ["ime", "cjk-fonts"]`. The
  requirement takes effect in three places:
  - the installers (`make-root.sh`, `WITH_IME=zh`);
  - settings: turning Chinese Pinyin on adds the CJK fonts module's packages
    when fontconfig has no font for `zh`;
  - updates: `declare-packages` declares a new requirement once. If the user
    removes it later, it stays removed.

  Removing Chinese Pinyin (`locachy-pkg remove`) installs nothing. It also
  unregisters the CJK fonts module when no other declared module requires it,
  none of its packages is installed, and it was declared only as Pinyin's
  requirement (an update's `(required by ime-zh)` group), never chosen on its
  own. Installed or chosen fonts stay, as in Ubuntu and Fedora (they serve
  reading Chinese too); their own row removes them. An update declares the
  requirement again only with Pinyin.

  The CJK fonts module has `"fontconfig_lang": "zh"`. It counts as present
  when fontconfig has a font for Chinese (as `fc-list :lang=zh`), whichever
  family that is: settings shows it so, and neither settings nor
  `declare-packages` adds noto-fonts-cjk then. After an input method job,
  settings sends `locachy-bar` SIGHUP, and the bar reloads two things without
  restarting the shared display:
  - Rime, only when the Chinese schema or Rime's deployment changed
    (`ime_reload`);
  - its CJK fonts, only when fontconfig's files or font directories changed
    (`panel_fonts_reload`, `FcConfigUptoDate`).
- **Which variant Han gets**: `link/64-locachy-cjk-prefer.conf`, the Arch
  Wiki's preference (Simplified Chinese, then Traditional, then Japanese).
  `make.sh` and the live medium install it in `/etc/fonts/conf.d`. It keeps
  what LOCachy showed before, when the programs opened the SC face of a fixed
  file. Like every `<alias>` it binds weakly at 64, so the user's own
  `fonts.conf` (50) still comes first. It names only Noto CJK families, so
  another CJK font is used as fontconfig sorts it.

## Choosing other fonts

`~/.config/fontconfig/fonts.conf`, or `/etc/fonts/local.conf` for every
user:

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

A program started after the change uses it. `fc-match monospace` and
`fc-match monospace:charset=4e2d` show what the programs will get.

## Tests

`tools/test-fonts.sh` (in `test-interaction-regressions.sh`) builds fixture
configurations from the host's installed fonts (`tools/font-fixtures.sh`).
Each has a fonts directory and a cache of its own, and `HOME` is isolated.
It checks these cases, plainly and with ASan/UBSan:

- the configured monospace is the primary face;
- Han comes from Noto Sans Mono CJK SC with LOCachy's file and from KR
  without it;
- a user's earlier preference wins (Liberation Mono, JP);
- another CJK family works (AR PL or Source Han, when installed);
- without a CJK font nothing fails: no face, `.notdef` in two cells, the
  bar's `?`, and `zh` absent;
- a CJK font added after the bar started is drawn after the bar's reload.

`tools/test-content-font.sh` runs the content font's tests in the same
fixtures. `tools/test-declare-packages.py` checks the requirement in
modules.json, `make-root.sh` and settings; `tools/test-pkg-sync.py`
(`RequirementRemoval`) its removal with Pinyin.

A bar built before Rime was installed has no input method compiled in.
pacman's hook builds a new `locachy-bar` while it runs. On settings' SIGHUP
the bar sees that its executable is a new file and runs it in place (same
PID, argv and environment). It waits while a chooser, question, notification
or composition is on the screen; `progs/bar.c` lists what goes across and
what is made again. `tools/test-bar-keyboard.c` tests it.

Not covered by these tests: the real shared display reloading Rime, and
re-executing itself, after a settings job.
