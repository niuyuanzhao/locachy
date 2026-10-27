#!/bin/bash
# The library contract in a disposable target root (local-package-gate). No
# input devices are created or read: /dev/input is private and empty. What the
# patch does with a finger is tested in a VM (test-pointer-libinput.sh);
# here, that this root has LOCachy's build of it.
set -euo pipefail
out=${1:?usage: test-libinput.sh RESULT_DIRECTORY}
mkdir -p "$out"
exec > >(tee "$out/library.log") 2>&1
version=$(pacman -Q libinput | cut -d' ' -f2)
printf 'libinput %s\n' "$version"
[[ $version == [1-9]*:* ]] # the raised epoch: this build, not a repository's
LC_ALL=C pacman -Qi libinput >"$out/package.txt"
grep -Eq '^Provides +: .*libinput\.so' "$out/package.txt"
# Panasonic's round touchpads, as the patch adds them to libinput's list.
quirks=/usr/share/libinput/50-system-panasonic.quirks
for model in CF-SV1 CF-SV8 CF-FV1; do
    awk -v want="[Panasonic Let's Note $model circular trackpad]" '
        $0 == want { inside = 1; next }
        /^\[/ { inside = 0 }
        inside && $0 == "ModelCircularTouchpad=1" { found = 1 }
        END { exit !found }' "$quirks"
done
grep -q 'pnCFFV1' "$quirks"
# Programs link it and load it (the direct programs, labwc through wlroots).
gcc -O2 -Wall -Werror -x c - -o "$out/load" -linput <<'EOF'
#include <libinput.h>
#include <stdio.h>
static int open_restricted(const char *path, int flags, void *data) { (void)path; (void)flags; (void)data; return -1; }
static void close_restricted(int fd, void *data) { (void)fd; (void)data; }
static const struct libinput_interface interface = { open_restricted, close_restricted };
int main(void)
{
    struct libinput *li = libinput_path_create_context(&interface, NULL);
    if (!li) return 1;
    libinput_unref(li);
    puts("context made and freed");
    return 0;
}
EOF
"$out/load"
python3 -I - "$out/gate.json" <<'PY'
import json, sys
json.dump({'name': 'libinput', 'id': 'libinput-circular-scroll-v1', 'passed': True,
           'tests': ['raised-epoch', 'provides', 'panasonic-round-quirks', 'links-and-loads']},
          open(sys.argv[1], 'w'), sort_keys=True)
PY
printf '%s\n' "PASS LOCachy's libinput: raised epoch, Panasonic's round touchpads, links and loads"
