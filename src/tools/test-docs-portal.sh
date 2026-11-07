#!/bin/bash
# test-docs-portal.sh NEW_OUTPUT: the document service with the packaged
# xdg-document-portal and xdg-permission-store (tools/DOCUMENTS.md), in a
# user namespace with its own mount and PID namespaces and /run: no root of
# the machine, no host bus. locachy-docs runs as the service does, as root
# of the namespace; the bus, the portal and the server as its user 1000 (a
# user of its own, /etc/subuid), so the portal's FUSE mount is that user's
# and root cannot look into it, as on LOCachy. That user keeps
# CAP_SYS_ADMIN of the namespace (ambient, no setuid fixup): fusermount3 is
# not setuid in a user namespace, and mounts with it. A "container" is a
# mount namespace whose view link/locachy-app's own functions make
# (docs_views_mounted, docs_bind, docs_reattach, sourced from it), run as
# root of the namespace, to which the user's FUSE mount is closed as it is
# to the real root.
# Checked: the view directory made before the service runs, and attached
# into the running container when it starts; a container started while the
# service runs (its view attached already) gets it once its start is over;
# a drag's documents readable nowhere until the drop; after it, by that
# container only; deleted when its cgroup empties; a service started again
# attaches the views again, into both running containers.
set -euo pipefail
[ "$#" = 1 ] || { echo "usage: test-docs-portal.sh NEW_OUTPUT" >&2; exit 2; }
out=$(realpath -m "$1")
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
for f in /usr/lib/xdg-document-portal /usr/lib/xdg-permission-store /usr/bin/fusermount3 /dev/fuse; do
    [ -e "$f" ] || { echo "SKIP test-docs-portal: no $f"; exit 0; }
done
mkdir -p "$out"
src=$(cd "$(dirname "$0")/.." && pwd)
t=/run/test-docs
defs="-DCGROUP_ROOT=\"$t/cgroup\" -DDOCS_CGROUP=\"$t/cgroup/locachy-docs\" -DBUS_CONF=\"$t/docs-bus.conf\" -DSELF=\"$t/locachy-docs\""
# shellcheck disable=SC2086
gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE $defs "$src/progs/locachy-docs.c" \
    $(pkg-config --cflags --libs gio-unix-2.0) -o "$out/locachy-docs"
# shellcheck disable=SC2086
gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE $defs "$src/tools/test-docs.c" \
    $(pkg-config --cflags --libs gio-unix-2.0) -o "$out/test-docs"
export out src
status=0
unshare --map-auto --map-root-user --mount --propagation private --pid --fork --mount-proc bash -euo pipefail -c '
    t=/run/test-docs
    mount -t tmpfs -o mode=0755 tmpfs /run
    mkdir -p $t/cgroup/locachy-docs $t/cgroup/app_edge $t/cgroup/app_other $t/cgroup/app_late /run/home /run/ctr /run/ctr2 /run/ctr3
    for f in cgroup.procs cgroup.kill; do : >$t/cgroup/locachy-docs/$f; done
    printf "populated 0\nfrozen 0\n" >$t/cgroup/locachy-docs/cgroup.events
    printf "populated 1\nfrozen 0\n" >$t/cgroup/app_edge/cgroup.events
    printf "populated 1\nfrozen 0\n" >$t/cgroup/app_other/cgroup.events
    printf "populated 1\nfrozen 0\n" >$t/cgroup/app_late/cgroup.events
    install -m 755 "$out/locachy-docs" "$out/test-docs" $t/
    # The definitions of link/locachy-app (all before its operations), with
    # the service, cgroups and runtime of this test.
    sed "/^operation=start/,\$d" "$src/link/locachy-app" >$t/app-defs.sh
    printf "source %s\nDOCS_TOOL=%s CGROUP_BASE=%s APP_RUNTIME=/run/test-apps\n" \
        $t/app-defs.sh $t/locachy-docs $t/cgroup >$t/app.sh
    install -m 644 "$src/link/docs-bus.conf" $t/docs-bus.conf
    # The user 1000 of the namespace, with a home of its own.
    { grep -v "^[^:]*:[^:]*:1000:" /etc/passwd; echo "tester:x:1000:1000::/run/home:/bin/bash"; } >$t/passwd
    { grep -v "^[^:]*:[^:]*:1000:" /etc/group; echo "tester:x:1000:"; } >$t/group
    mount --bind $t/passwd /etc/passwd
    mount --bind $t/group /etc/group
    mkdir -p /run/home/Documents /run/home/folder
    echo "private words" >/run/home/secret.txt
    echo "in a folder" >/run/home/folder/inside.txt
    chown -R 1000:1000 /run/home
    as_user() { setpriv --reuid=1000 --regid=1000 --clear-groups -- "$@"; }
    fails=0
    check() { if eval "$1"; then echo "PASS $2"; else echo "FAIL $2 (${3:-})"; fails=$((fails + 1)); fi; }
    service() {
        setpriv --inh-caps=+sys_admin --ambient-caps=+sys_admin --securebits=+no_setuid_fixup -- \
            $t/locachy-docs >>"$out/service.log" 2>&1 &
        svc=$!
        for _ in $(seq 1 200); do grep -q "serving" "$out/service.log" 2>/dev/null && [ "$(grep -c serving "$out/service.log")" -ge "$1" ] && break; sleep 0.05; done
    }

    # The container starts before the service: its view directory is made,
    # nothing attached yet.
    r=0; $t/locachy-docs --attach edge 2>>"$out/attach.log" || r=$?
    check "[ \$r = 1 ] && [ -d /run/locachy/docs/views/edge ]" \
        "--attach before the service runs: the view directory made, nothing attached (status 1)" "status $r"
    $t/locachy-docs --attach other 2>>"$out/attach.log" || true
    # A container: its own mount namespace, the view directory bound as a
    # slave at its doc by link/locachy-app (docs_bind).
    check "bash -c \"source $t/app.sh; docs_views_mounted\"" \
        "the views mount is there for locachy-app (the mount table)"
    unshare --mount --propagation slave bash -c "
        source $t/app.sh
        docs_bind edge /run/ctr 1000 1000 && docs_bind other /run/ctr2 1000 1000 && echo ready >/run/ctr/ready
        exec sleep 600" &
    ctr=$!
    for _ in $(seq 1 100); do [ -f /run/ctr/ready ] && break; sleep 0.05; done
    in_ctr() { nsenter -t "$ctr" -m -- setpriv --reuid=1000 --regid=1000 --clear-groups -- "$@"; }

    service 1
    mounted() { grep -q " /run/user/1000/doc " /proc/self/mountinfo; }
    check "grep -q serving \"\$out/service.log\" && mounted" \
        "the service starts the bus, the packaged portal (its FUSE mount at /run/user/1000/doc) and the server" \
        "$(tail -n 3 "$out/service.log" | tr "\n" " ")"
    check "pgrep -x xdg-document-po >/dev/null && pgrep -x xdg-permission- >/dev/null && pgrep -x dbus-daemon >/dev/null" \
        "the portal and the permission store are started on the documents bus by their packaged service files"
    check "[ \"\$(stat -c %U /run/locachy/docs/control)\" = root ] && [ \"\$(stat -c %a /run/locachy/docs/control)\" = 600 ]" \
        "the control socket is root'"'"'s alone (0600)" "$(stat -c "%U %a" /run/locachy/docs/control)"
    rc=0; ls /run/user/1000/doc/ >/dev/null 2>&1 || rc=$?
    check "[ \$rc != 0 ] && as_user ls /run/user/1000/doc/ >/dev/null" \
        "root cannot look into the user'"'"'s FUSE mount (no allow_other); the user can"
    check "in_ctr stat /run/ctr/doc >/dev/null && [ \"\$(in_ctr stat -c %d /run/ctr/doc)\" = \"\$(as_user stat -c %d /run/user/1000/doc)\" ]" \
        "the view attached by the service reached the container that started before it (slave of the shared view)"

    # A container started while the service runs, as link/locachy-app starts
    # one: --attach (its view attached on the host now), the attach again
    # forked before the launcher enters the cgroup (docs_reattach, under the
    # control lock), the container binding the directory (docs_bind), then
    # the start over (the lock free).
    $t/locachy-docs --attach late 2>>"$out/attach.log"
    rc=0; stat /run/locachy/docs/views/late >/dev/null 2>&1 || rc=$?
    check "[ \$rc != 0 ] && bash -c \"source $t/app.sh; docs_views_mounted\"" \
        "attached, the view is closed to root (stat fails); locachy-app tells the views from the mount table"
    # The view selection of locachy-app itself, as root against the attached
    # view (a FUSE mount of the user): a [ -d ] of it fails here.
    printf "source %s\nmkdir -p /run/test-apps/\$1\ndocs_select \"\$1\"\nprintf \"%%s\\\\n\" \"\$docs_name\"\n" \
        $t/app.sh >$t/select.sh
    got=$(bash $t/select.sh edge 2>>"$out/attach.log" || true)
    check "[ ! -d /run/locachy/docs/views/edge ] && [ \"\$got\" = edge ]" \
        "docs_select picks the attached view of a container that root cannot stat (F-DOCS-1)" "got=$got"
    mkfifo /run/late.go
    bash -c "source $t/app.sh; lock_app late; docs_reattach late; read -r _ </run/late.go; unlock_app; wait" &
    late=$!
    for _ in $(seq 1 100); do [ -e /run/test-apps/late/control.lock ] && break; sleep 0.05; done
    unshare --mount --propagation slave bash -c "
        source $t/app.sh
        docs_bind late /run/ctr3 1000 1000 && echo bound >/run/ctr3/bound
        exec sleep 600" &
    ctr3=$!
    for _ in $(seq 1 100); do [ -f /run/ctr3/bound ] && break; sleep 0.05; done
    in_ctr3() { nsenter -t "$ctr3" -m -- setpriv --reuid=1000 --regid=1000 --clear-groups -- "$@"; }
    check "[ -f /run/ctr3/bound ] && [ -z \"\$(in_ctr3 ls /run/ctr3/doc)\" ] && [ \"\$(in_ctr3 stat -c %d /run/ctr3/doc)\" != \"\$(as_user stat -c %d /run/user/1000/doc)\" ]" \
        "the container bound the bare view directory, not a copy of the view attached now"
    echo go >/run/late.go
    wait "$late" || true
    for _ in $(seq 1 100); do [ "$(in_ctr3 stat -c %d /run/ctr3/doc)" = "$(as_user stat -c %d /run/user/1000/doc)" ] && break; sleep 0.05; done
    check "[ \"\$(in_ctr3 stat -c %d /run/ctr3/doc)\" = \"\$(as_user stat -c %d /run/user/1000/doc)\" ] && nsenter -t \"\$ctr3\" -m findmnt -no PROPAGATION /run/ctr3/doc | grep -q slave" \
        "its start over, the view attached again reached it; its doc is a slave of the views" \
        "$(nsenter -t "$ctr3" -m findmnt -no SOURCE,PROPAGATION /run/ctr3/doc | tr "\n" " ")"

    # A drag: begin as files, drop as labwc, end; the container reads.
    mkfifo /run/drag.in
    $t/test-docs drag edge /run/home/secret.txt /run/home/folder </run/drag.in >"$out/drag.out" 2>&1 &
    drag=$!
    exec 7>/run/drag.in
    for _ in $(seq 1 100); do [ -s "$out/drag.out" ] && break; sleep 0.05; done
    doc=$(sed -n 2p "$out/drag.out"); dir=$(sed -n 3p "$out/drag.out")
    check "[[ \$(head -n 1 \"\$out/drag.out\") == \"ok \"* && \$doc == /run/user/1000/doc/*/secret.txt && \$dir == /run/user/1000/doc/*/folder ]]" \
        "begin: the real portal made a document of the file and of the folder, paths in its mount" "$(tr "\n" " " <"$out/drag.out")"
    rc=0; in_ctr cat "/run/ctr/doc/${doc#/run/user/1000/doc/}" >/dev/null 2>&1 || rc=$?
    check "[ \$rc != 0 ] && [ -z \"\$(in_ctr ls /run/ctr/doc)\" ]" \
        "before the drop the container sees no document and cannot read the file (added with no permission)"
    echo >&7
    for _ in $(seq 1 100); do grep -q "^drop" "$out/drag.out" && break; sleep 0.05; done
    check "grep -qx \"drop ok\" \"\$out/drag.out\"" "drop on the container: ok after GrantPermissions" "$(grep ^drop "$out/drag.out" || true)"
    got=$(in_ctr cat "/run/ctr/doc/${doc#/run/user/1000/doc/}" 2>&1 || true)
    check "[ \"\$got\" = \"private words\" ]" "after the drop the container reads the file at the same path in its view" "$got"
    got=$(in_ctr cat "/run/ctr/doc/${dir#/run/user/1000/doc/}/inside.txt" 2>&1 || true)
    check "[ \"\$got\" = \"in a folder\" ]" "and the folder'"'"'s files (a DIRECTORY document)" "$got"
    rc=0; in_ctr cat "/run/ctr2/doc/${doc#/run/user/1000/doc/}" >/dev/null 2>&1 || rc=$?
    check "[ \$rc != 0 ] && [ -z \"\$(in_ctr ls /run/ctr2/doc)\" ]" "another container (its own view) sees nothing of it"
    rc=0; in_ctr sh -c "echo x >>/run/ctr/doc/${doc#/run/user/1000/doc/}" 2>/dev/null || rc=$?
    check "[ \$rc != 0 ] && [ \"\$(cat /run/home/secret.txt)\" = \"private words\" ]" "read only: the container cannot write it"
    echo >&7
    exec 7>&-
    wait "$drag" || true
    check "grep -qx \"end sent\" \"\$out/drag.out\"" "end done: unanswered, taken (the connection still served)"
    # The container ends: its cgroup is empty, its documents go.
    printf "populated 0\nfrozen 0\n" | dd of=$t/cgroup/app_edge/cgroup.events conv=notrunc status=none
    gone=0
    for _ in $(seq 1 100); do in_ctr test -e "/run/ctr/doc/${doc#/run/user/1000/doc/}" || { gone=1; break; }; sleep 0.05; done
    check "[ \$gone = 1 ] && ! as_user test -e \"\$doc\"" "the container'"'"'s cgroup empty: its documents deleted (gone from its view and the portal)"
    printf "populated 1\nfrozen 0\n" | dd of=$t/cgroup/app_edge/cgroup.events conv=notrunc status=none
    # A drop on the container started while the service ran.
    printf "x\nx\n" >/run/drag-late.in
    $t/test-docs drag late /run/home/secret.txt </run/drag-late.in >"$out/drag-late.out" 2>&1 || true
    doc=$(sed -n 2p "$out/drag-late.out")
    got=$(in_ctr3 cat "/run/ctr3/doc/${doc#/run/user/1000/doc/}" 2>&1 || true)
    check "grep -qx \"drop ok\" \"\$out/drag-late.out\" && [ \"\$got\" = \"private words\" ]" \
        "the container started while the service ran reads a file dropped on it" "$got"

    # The service ends and starts again: the views are attached again.
    kill -TERM "$svc"
    wait "$svc" || true
    for _ in $(seq 1 60); do pgrep -x xdg-document-po >/dev/null || pgrep -x dbus-daemon >/dev/null || mounted || break; sleep 0.05; done
    check "! pgrep -x xdg-document-po >/dev/null && ! pgrep -x dbus-daemon >/dev/null && ! mounted" \
        "the service ends: bus, portal and server gone, the FUSE mount and the views detached" "$(pgrep -a . | tr "\n" ";")"
    service 2
    printf "x\nx\n" >/run/drag2.in
    $t/test-docs drag edge /run/home/secret.txt </run/drag2.in >"$out/drag2.out" 2>&1 || true
    doc=$(sed -n 2p "$out/drag2.out")
    got=$(in_ctr cat "/run/ctr/doc/${doc#/run/user/1000/doc/}" 2>&1 || true)
    check "grep -qx \"drop ok\" \"\$out/drag2.out\" && [ \"\$got\" = \"private words\" ]" \
        "started again: its views are attached again; a new drop is read in the running container" "$got"
    printf "x\nx\n" >/run/drag-late2.in
    $t/test-docs drag late /run/home/secret.txt </run/drag-late2.in >"$out/drag-late2.out" 2>&1 || true
    doc=$(sed -n 2p "$out/drag-late2.out")
    got=$(in_ctr3 cat "/run/ctr3/doc/${doc#/run/user/1000/doc/}" 2>&1 || true)
    check "grep -qx \"drop ok\" \"\$out/drag-late2.out\" && [ \"\$got\" = \"private words\" ]" \
        "started again: the container started while it ran before gets its view again; a new drop is read" "$got"
    # Granted, then cancelled (labwc gave up waiting, or the pointer left the
    # target): no longer readable, the document gone.
    mkfifo /run/drag3.in
    $t/test-docs drag edge /run/home/secret.txt </run/drag3.in >"$out/drag3.out" 2>&1 &
    drag=$!
    exec 7>/run/drag3.in
    for _ in $(seq 1 100); do [ -s "$out/drag3.out" ] && break; sleep 0.05; done
    doc=$(sed -n 2p "$out/drag3.out")
    echo >&7
    for _ in $(seq 1 100); do grep -q "^drop" "$out/drag3.out" && break; sleep 0.05; done
    got=$(in_ctr cat "/run/ctr/doc/${doc#/run/user/1000/doc/}" 2>&1 || true)
    echo cancelled >&7
    exec 7>&-
    wait "$drag" || true
    rc=0; in_ctr cat "/run/ctr/doc/${doc#/run/user/1000/doc/}" >/dev/null 2>&1 || rc=$?
    check "[ \"\$got\" = \"private words\" ] && grep -qx \"end sent\" \"\$out/drag3.out\" && [ \$rc != 0 ] && ! as_user test -e \"\$doc\"" \
        "granted, then end cancelled: revoked and deleted, no longer readable in the container" "$got rc=$rc"
    kill -TERM "$svc"; wait "$svc" || true
    kill "$ctr" "$ctr3" 2>/dev/null || true
    exit "$fails"
' >"$out/test.log" 2>&1 || status=1
grep -E '^(PASS|FAIL|SKIP)' "$out/test.log" || true
if [ "$status" != 0 ] || ! grep -q '^PASS' "$out/test.log"; then
    echo "FAIL test-docs-portal: $out/test.log"
    status=1
fi
exit "$status"
