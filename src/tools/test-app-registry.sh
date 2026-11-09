#!/bin/bash
# test-app-registry.sh OUT: process 1's application registry (init/apps.c)
# with test-app-tool.sh in place of locachy-app, its fixtures under
# /run/locachy-registry-test in a mount namespace of its own (a tmpfs on /run;
# nothing of the host is written). Unprivileged: a user namespace maps root.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-app-registry.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
exec unshare --map-root-user --mount bash -c '
set -euo pipefail
src=$1 out=$2
mount -t tmpfs tmpfs /run
root=/run/locachy-registry-test
mkdir -p "$root/conf" "$root/runtime" "$root/cgroup"
for n in alpha broken gamma refused; do
    printf "package=\"x\"\ncommand=\"/bin/true\"\n" >"$root/conf/$n.conf"
    cg=$root/cgroup/app_$n
    mkdir -p "$cg"
    printf "populated 0\nfrozen 0\n" >"$cg/cgroup.events"
    echo 0 >"$cg/memory.current"
    echo 0 >"$cg/memory.swap.current"
    touch "$cg/cgroup.freeze" "$cg/cgroup.kill" "$cg/cgroup.procs"
done
touch "$root/locachy-0" "$root/wayland-0" # the shared display sockets
# Native windows: their programs (one double), the clipboard service, the
# cgroups the kernel would make (named as the registry names them), documents.
mkdir -p "$root/bin"
for p in view files settings term; do
    ln -s "$src/tools/test-native-window.sh" "$root/bin/$p"
done
for n in window-view-1 window-view-2 window-files-1 window-files-2 window-settings-1 window-mixer-1 window-bluetooth-1 window-network-1 window-term-1 window-term-2; do
    cg=$root/cgroup/app_$n
    mkdir -p "$cg"
    printf "populated 0\nfrozen 0\n" >"$cg/cgroup.events"
    touch "$cg/cgroup.freeze" "$cg/cgroup.kill" "$cg/cgroup.procs"
done
printf "text\n" >"$root/doc.txt"
touch "$root/fail-doc" "$root/silent-doc"
# Record the exact argument vector at the real registry-to-script boundary.
cat >"$root/bin/app-tool" <<EOF
#!/bin/bash
printf "%s\\n" "\$@" >"$root/runtime/tool.argv"
if [ "\${2:-}" = --own-tor ]; then set -- "\$1" "\${@:3}"; fi
exec "$src/tools/test-app-tool.sh" "\$@"
EOF
chmod 755 "$root/bin/app-tool"
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" -I"$src/init" \
    -DAPP_CONF="\"$root/conf\"" -DAPP_RUN="\"$root/runtime\"" -DAPP_TOOL="\"$root/bin/app-tool\"" \
    -DAPP_CGROUP_ROOT="\"$root/cgroup\"" -DAPP_DISPLAY="\"$root/locachy-0\"" \
    -DAPP_DISPLAY_READY="\"$root/wayland-0\"" -DAPP_NATIVE_DIR="\"$root/bin\"" \
    -DAPP_CLIPBOARD="\"$src/tools/test-clipboard-double.sh\"" -DAPP_WAYLAND_RUNTIME="\"$root\"" \
    -DAPP_WINDOW_CONTROL="\"$root/no-window-control.sock\"" \
    -o "$out/test-app-registry" "$src/tools/test-app-registry.c" "$src/init/apps.c" "$src/base/say.c"
"$out/test-app-registry" >"$out/registry.log"
tail -1 "$out/registry.log"
' _ "$src" "$out"
