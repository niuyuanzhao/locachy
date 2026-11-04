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
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" -I"$src/init" \
    -DAPP_CONF="\"$root/conf\"" -DAPP_RUN="\"$root/runtime\"" -DAPP_TOOL="\"$src/tools/test-app-tool.sh\"" \
    -DAPP_CGROUP_ROOT="\"$root/cgroup\"" -DAPP_DISPLAY="\"$root/locachy-0\"" \
    -DAPP_DISPLAY_READY="\"$root/wayland-0\"" \
    -o "$out/test-app-registry" "$src/tools/test-app-registry.c" "$src/init/apps.c" "$src/base/say.c"
"$out/test-app-registry" >"$out/registry.log"
tail -1 "$out/registry.log"
' _ "$src" "$out"
