#!/bin/bash
# The CLI-only tools contract in a disposable target root. No physical
# input devices or display are needed; /dev/input is private and empty.
set -euo pipefail
out=${1:?usage: test-libinput-tools.sh RESULT_DIRECTORY}
mkdir -p "$out"
exec > >(tee "$out/cli.log") 2>&1
[ ! -e /usr/lib/libinput/libinput-debug-gui ]
[ ! -e /usr/libexec/libinput/libinput-debug-gui ]
libinput --version
timeout 10 libinput list-devices >"$out/devices.txt"
timeout 10 libinput debug-events --help >"$out/debug-events-help.txt"
grep -q 'Usage:' "$out/debug-events-help.txt"
# Keep all upstream CLI helpers, not just the two used by gestures.
for name in libinput libinput-debug-events libinput-list-devices libinput-record libinput-replay; do
    path=$(pacman -Qlq libinput-tools | grep -E "/${name}$")
    [ -n "$path" ] && [ -x "$path" ]
done
while read -r path; do
    [ -f "$path" ] || continue
    if readelf -h "$path" >/dev/null 2>&1; then
        readelf -d "$path" >"$out/$(basename "$path").dynamic"
        ! grep -Eq 'NEEDED.*\[(libgtk|libgdk|libcairo|libglib|libX11|libwayland-client)' "$out/$(basename "$path").dynamic"
        ldd "$path" >"$out/$(basename "$path").ldd"
        ! grep -q 'not found' "$out/$(basename "$path").ldd"
    fi
done < <(pacman -Qlq libinput-tools)
# The library is LOCachy's (pkgbuilds/libinput: the raised epoch) of the tools'
# own release, both from the same locked recipe.
tools=$(pacman -Q libinput-tools | cut -d' ' -f2)
library=$(pacman -Q libinput | cut -d' ' -f2)
printf 'libinput-tools %s, libinput %s\n' "$tools" "$library"
[[ $library == [1-9]*:* && ${library#*:} == "${tools#*:}" ]]
# The packaged runtime dependency set must not reintroduce the removed GUI.
LC_ALL=C pacman -Qi libinput-tools >"$out/package.txt"
python3 -I - "$out/package.txt" "$out/gate.json" <<'PY'
import json, re, sys
text = open(sys.argv[1]).read()
match = re.search(r'^Depends On\s*:\s*(.*?)(?=^\S)', text, re.M | re.S)
if not match: raise RuntimeError('package dependency metadata is missing')
deps = {re.split(r'[<>=]', item)[0] for item in match[1].split()}
if deps & {'gtk4', 'cairo', 'glib2', 'libx11', 'wayland'}:
    raise RuntimeError('CLI package still has GUI runtime dependencies')
if not {'libinput', 'python', 'python-libevdev', 'python-pyudev', 'python-yaml'} <= deps:
    raise RuntimeError('CLI helper dependencies were removed')
json.dump({'name': 'libinput-tools', 'id': 'libinput-tools-cli-v1', 'passed': True,
           'tests': ['cli-commands', 'no-debug-gui', 'helper-files', 'elf-dependencies', 'runtime-dependencies',
                     'library-release']},
          open(sys.argv[2], 'w'), sort_keys=True)
PY
printf '%s\n' 'PASS CLI tools remain usable without the GTK debug interface'
