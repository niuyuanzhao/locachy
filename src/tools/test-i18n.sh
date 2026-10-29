#!/bin/sh
# test-i18n.sh: the interface's translation. A catalog and
# the zh_CN locale are made in a scratch directory (LOCPATH: the system's
# locales are not changed); test-i18n runs base/i18n.c over them. Then every
# po/LL_CC.po compiles with msgfmt --check, as make.sh compiles it.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/var/tmp/locachy-i18n-test}
rm -rf "$out"
mkdir -p "$out/catalogs/zh_CN/LC_MESSAGES" "$out/locales"
cat >"$out/test.po" <<'EOF'
msgid ""
msgstr ""
"Project-Id-Version: LOCachy\n"
"PO-Revision-Date: 2026-10-24 00:00+0000\n"
"Last-Translator: test\n"
"Language-Team: test\n"
"Language: zh_CN\n"
"MIME-Version: 1.0\n"
"Content-Type: text/plain; charset=UTF-8\n"
"Content-Transfer-Encoding: 8bit\n"
"Plural-Forms: nplurals=1; plural=0;\n"

msgid "Save"
msgstr "保存"

msgctxt "verb"
msgid "Open"
msgstr "打开"

msgctxt "state"
msgid "Open"
msgstr "已打开"

msgid "%d item"
msgid_plural "%d items"
msgstr[0] "%d 项"
EOF
msgfmt --check -o "$out/catalogs/zh_CN/LC_MESSAGES/locachy.mo" "$out/test.po"
# Another library's catalog, as PAM's: its domain is not bound to UTF-8.
cat >"$out/other.po" <<'OTHER'
msgid ""
msgstr ""
"Language: zh_CN\n"
"MIME-Version: 1.0\n"
"Content-Type: text/plain; charset=UTF-8\n"
"Content-Transfer-Encoding: 8bit\n"

msgid "Password: "
msgstr "密码："
OTHER
msgfmt --check -o "$out/catalogs/zh_CN/LC_MESSAGES/other-library.mo" "$out/other.po"
localedef -i zh_CN -f UTF-8 "$out/locales/zh_CN.UTF-8"
localedef -i C -f UTF-8 "$out/locales/C.UTF-8"
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" \
    "$src/tools/test-i18n.c" "$src/base/i18n.c" "$src/base/rc.c" "$src/base/say.c" "$src/base/ui.c" \
    -o "$out/test-i18n"
LOCPATH="$out/locales" "$out/test-i18n" "$out/catalogs"
for lang in $(cat "$src/po/LINGUAS"); do
    printf '== po/%s.po: ' "$lang"
    msgfmt --check --statistics -o /dev/null "$src/po/$lang.po"
done
