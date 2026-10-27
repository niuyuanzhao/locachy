#!/bin/sh
# i18n-update.sh: po/locachy.pot from the interface text marked in the
# sources (_, N_, C_, P_), and every po/LL_CC.po of
# po/LINGUAS brought up to it (msgmerge; a new language: msginit). Run it
# after changing marked text, then translate what is new or fuzzy.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$src"
xgettext --language=C --from-code=UTF-8 --add-comments=TRANSLATORS: \
    --keyword=_ --keyword=N_ --keyword=C_:1c,2 --keyword=P_:1,2 \
    --package-name=LOCachy --add-location=file --sort-by-file \
    -o po/locachy.pot base/*.c progs/*.c progs/*.h progs/*.inc
for lang in $(cat po/LINGUAS); do
    if [ -f "po/$lang.po" ]; then
        msgmerge --quiet --update --backup=none --add-location=file "po/$lang.po" po/locachy.pot
    else
        msginit --no-translator --locale="$lang.UTF-8" -i po/locachy.pot -o "po/$lang.po"
    fi
    printf '%s: ' "$lang"
    msgfmt --check --statistics -o /dev/null "po/$lang.po"
done
