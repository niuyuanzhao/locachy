#!/bin/bash
# test-docs-view.sh [NAME]: the document view selection of link/locachy-app as
# the REAL root of a running LOCachy system (VM acceptance only: a user
# namespace's root is not real root; tools/DOCUMENTS.md section 8). The
# definitions of the installed locachy-app before its operations run:
# docs_select, which asks the mount table, not the view. locachy-docs
# --attach NAME attaches NAME's view of the portal (the user's FUSE mount, no
# allow_other) on DOCS_VIEWS/NAME; root's stat of it must fail while
# docs_select still names NAME. NAME is a name no container has; its view is
# detached and its directory removed at the end. Needs locachy-docs serving.
# Run as root.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "test-docs-view: run as root" >&2; exit 2; }
name=${1:-docsview-test}
case "$name" in *[!a-zA-Z0-9._-]*|'') echo "test-docs-view: bad NAME" >&2; exit 2 ;; esac
[ ! -e "/etc/locachy/containers/$name.conf" ] || { echo "test-docs-view: $name is a container" >&2; exit 2; }
views=/run/locachy/docs/views
# The mount table, not mountpoint(1) or stat: root's stat of the portal fails.
mounted() { # PATH TYPE
    awk -v p="$1" -v t=" - $2 " '$5 == p && index($0, t) { found = 1 } END { exit !found }' /proc/self/mountinfo
}
mounted /run/user/1000/doc fuse.portal || { echo "SKIP test-docs-view: the document portal is not mounted"; exit 0; }
defs=$(mktemp /var/tmp/locachy-docs-view.XXXXXX)
trap 'umount -l "$views/$name" 2>/dev/null || true; rmdir "$views/$name" 2>/dev/null || true
      rm -f "$defs"; rm -rf "/run/locachy/apps/$name"' EXIT
sed '/^operation=start/,$d' /usr/local/bin/locachy-app >"$defs"
mkdir -p "/run/locachy/apps/$name"
fails=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1 ($3)"; fails=$((fails + 1)); fi; }

/usr/local/lib/locachy/locachy-docs --attach "$name" 2>/dev/null || true
check "locachy-docs --attach attached the view of $name (fuse.portal on the views mount)" \
    "mounted '$views/$name' fuse.portal" "not in mountinfo"
check "root cannot stat the attached view (the user's FUSE mount, no allow_other)" "! [ -d '$views/$name' ]" \
    "stat succeeded"
sel=$(bash -c "set -euo pipefail; source '$defs'; docs_select '$name'; printf '%s\n' \"\$docs_name\"" 2>/dev/null || true)
check "docs_select of locachy-app, as root, names the view of $name" "[ '$sel' = '$name' ]" "docs_name='$sel'"
[ "$fails" = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit "$fails"
