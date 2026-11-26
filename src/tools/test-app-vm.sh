#!/bin/bash
# test-app-vm.sh: end-to-end verification of vm="1" app encapsulation in
# locachy-app, locachy-pkg container planning, and declarative custom VMs.
#
# Runs unprivileged on Linux using an unshared user+mount namespace (with
# fixture cgroup files and a fake QMP server in place of QEMU) plus real
# qemu-img overlay creation and, when /dev/kvm is accessible, real
# qemu-system-x86_64 argv acceptance checks.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: test-app-vm.sh requires Linux"
    exit 77
fi
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-app-vm-test.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi

fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
check() {
    local label="$1"; shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

# Build real app-init for the readiness handshake and signal forwarding.
cc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -o "$out/app-init" "$src/progs/app-init.c"
pass "build: app-init compiles cleanly"

# Syntax check on locachy-app and locachy-pkg.
bash -n "$src/link/locachy-app"
pass "syntax: locachy-app parses cleanly"
python3 -I -m py_compile "$src/link/locachy-pkg"
pass "syntax: locachy-pkg compiles cleanly"

# ---------------------------------------------------------------------------
# 1. locachy-pkg: vm="1" configs are excluded from container update plans
# ---------------------------------------------------------------------------
python3 -I - "$src" "$out" <<'PY'
import importlib.machinery
import importlib.util
from pathlib import Path
import sys
from unittest import mock

src, out = Path(sys.argv[1]), Path(sys.argv[2])
loader = importlib.machinery.SourceFileLoader("locachy_pkg", str(src / "link/locachy-pkg"))
spec = importlib.util.spec_from_loader(loader.name, loader)
pkg = importlib.util.module_from_spec(spec)
loader.exec_module(pkg)

conf = out / "pkg-conf"
conf.mkdir(exist_ok=True)
(conf / "browser.conf").write_text('package="firefox"\ncommand="/usr/bin/firefox"\n', encoding="utf-8")
(conf / "agent.conf").write_text('host="1"\npackage="foot"\ncommand="/usr/bin/foot"\n', encoding="utf-8")
(conf / "winmaint.conf").write_text('vm="1"\nvm_kind="winmaint"\nvm_freeze="qmp"\nnetwork="1"\n', encoding="utf-8")
(conf / "ubuntu-dev.conf").write_text('vm="1"\nvm_kind="compute"\nvm_name="ubuntu-dev"\nnetwork="1"\n', encoding="utf-8")
(conf / "workvm.conf").write_text('vm="1"\nvm_kind="custom"\nvm_base="/tmp/base.qcow2"\n', encoding="utf-8")

with mock.patch.object(pkg, "CONF_DIR", str(conf)):
    got = pkg.containers()
    assert got == ["browser"], f"expected only ['browser'], got {got}"
    rows = [
        ("host", "repo", "base", "1", "2"),
        ("browser", "rebuild", "the host is updated", "", ""),
        ("agent", "rebuild", "its preset names other packages", "a", "b"),
        ("winmaint", "rebuild", "should never be selected", "", ""),
        ("ubuntu-dev", "rebuild", "should never be selected", "", ""),
    ]
    targets = pkg.targets_of(rows)
    assert targets == ["host", "browser", "agent"], f"unexpected targets: {targets}"
print("PASS: locachy-pkg skips vm=\"1\" configs in containers() and targets_of()")
PY

# ---------------------------------------------------------------------------
# 2. Fake QMP VM double + fixture cgroup harness inside an unprivileged
#    user+mount namespace (maps root so root-only LOCACHY_* seams apply,
#    while /proc/self/uid_map has range 1 so vm_real_root is false).
# ---------------------------------------------------------------------------
if ! unshare --map-root-user --mount true >/dev/null 2>&1; then
    echo "SKIP: unprivileged user+mount namespace unavailable"
    exit 77
fi

cat >"$out/fake-qmp-vm.py" <<'PY'
#!/usr/bin/env python3
"""Fake QEMU / launcher double that records argv+env and serves QMP commands."""
import json
import os
from pathlib import Path
import signal
import socket
import sys
import threading

log_file = Path(os.environ["TEST_VM_LOG"])
env_file = Path(os.environ["TEST_VM_ENV"])
qmp_path = os.environ.get("LOCACHY_VM_QMP_SOCK", "")
# Accept --qmp-sock or -qmp unix:PATH,... if present in argv too.
argv = sys.argv[1:]
for i, a in enumerate(argv):
    if a == "--qmp-sock" and i + 1 < len(argv):
        qmp_path = argv[i + 1]
    elif a == "-qmp" and i + 1 < len(argv) and argv[i + 1].startswith("unix:"):
        qmp_path = argv[i + 1][5:].split(",", 1)[0]

record = {
    "argv": sys.argv,
    "WAYLAND_DISPLAY": os.environ.get("WAYLAND_DISPLAY"),
    "XDG_RUNTIME_DIR": os.environ.get("XDG_RUNTIME_DIR"),
    "GDK_BACKEND": os.environ.get("GDK_BACKEND"),
    "SDL_VIDEODRIVER": os.environ.get("SDL_VIDEODRIVER"),
    "LOCACHY_VM_QMP_SOCK": os.environ.get("LOCACHY_VM_QMP_SOCK"),
    "PULSE_SERVER": os.environ.get("PULSE_SERVER"),
    "PIPEWIRE_REMOTE": os.environ.get("PIPEWIRE_REMOTE"),
    "uid": os.getuid(),
}
env_file.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")

stop_ev = threading.Event()
mode = os.environ.get("TEST_VM_MODE", "normal")


def log_event(ev):
    with open(log_file, "a", encoding="utf-8") as f:
        f.write(ev + "\n")
        f.flush()


def on_term(_sig, _frame):
    log_event("SIGNAL:SIGTERM")
    stop_ev.set()


signal.signal(signal.SIGTERM, on_term)
signal.signal(signal.SIGINT, on_term)

if mode == "exit-after-record":
    log_event("RECORDED")
    sys.exit(0)

if qmp_path:
    Path(qmp_path).parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(qmp_path):
        os.unlink(qmp_path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(qmp_path)
    srv.listen(8)
    srv.settimeout(0.1)
    log_event("QMP_READY")
    try:
        while not stop_ev.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            with conn:
                conn.sendall(b'{"QMP":{"version":{"qemu":{"major":11,"minor":1,"micro":2}}}}\n')
                f = conn.makefile("r", encoding="utf-8")
                for line in f:
                    req = json.loads(line)
                    cmd = req.get("execute", "")
                    if cmd == "qmp_capabilities":
                        conn.sendall(b'{"return":{}}\n')
                        continue
                    log_event(f"QMP:{cmd}")
                    conn.sendall(b'{"return":{}}\n')
                    if cmd == "system_powerdown" and mode != "ignore-powerdown":
                        stop_ev.set()
                    break
    finally:
        srv.close()
        try:
            os.unlink(qmp_path)
        except OSError:
            pass
else:
    log_event("NO_QMP_READY")
    stop_ev.wait(timeout=30)
PY
chmod 755 "$out/fake-qmp-vm.py"

cat >"$out/fake-helper.py" <<'PY'
#!/usr/bin/env python3
"""Minimal unix socket helper double standing in for swtpm or virtiofsd."""
import os
from pathlib import Path
import signal
import socket
import sys
import threading

sock_path = ""
for i, a in enumerate(sys.argv[1:]):
    if a.startswith("--socket-path="):
        sock_path = a.split("=", 1)[1]
    elif a.startswith("type=unixio,path="):
        sock_path = a.split("path=", 1)[1]

if not sock_path:
    sys.exit("fake-helper: no socket path in argv")
log = os.environ.get("TEST_HELPER_LOG", "")


def note(text):
    if log:
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"{text} {sock_path}\n")


stop = threading.Event()
signal.signal(signal.SIGTERM, lambda *_: stop.set())
signal.signal(signal.SIGINT, lambda *_: stop.set())
Path(sock_path).parent.mkdir(parents=True, exist_ok=True)
if os.path.lexists(sock_path):
    os.unlink(sock_path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
srv.listen(4)
note("started")
try:
    stop.wait(timeout=30)
finally:
    note("stopped")
    srv.close()
    try:
        os.unlink(sock_path)
    except OSError:
        pass
PY
chmod 755 "$out/fake-helper.py"

cat >"$out/fake-clipboard.sh" <<'SH'
#!/bin/bash
set -euo pipefail
log="${TEST_CLIP_LOG:?}"
case "${1:-}" in
sync)
    echo "CLIP:sync" >>"$log"
    ;;
freeze)
    cg="$2"
    echo "CLIP:freeze" >>"$log"
    printf '1\n' >"$cg/cgroup.freeze"
    printf 'populated 1\nfrozen 1\n' >"$cg/cgroup.events"
    ;;
*)
    exit 2
    ;;
esac
SH
chmod 755 "$out/fake-clipboard.sh"

# Wrapper around fake-qmp-vm.py that runs inside the unshared namespace.
ns_rc=0
unshare --map-root-user --mount bash -euo pipefail -s "$src" "$out" <<'NS_EOF' || ns_rc=$?
src=$1
out=$2
fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

run_root="$out/ns"
mkdir -p "$run_root"/{conf,cont,runtime,cg,xdg,vmstate,tools,imgs}

# Fake Wayland socket so locachy-app's display check passes.
python3 -I - "$run_root/xdg/locachy-0" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
s.listen(1)
PY

# Fake winmaint, compute, and custom launchers all delegating to fake-qmp-vm.py.
for t in winmaint compute custom-vm; do
    cat >"$run_root/tools/$t" <<EOF
#!/bin/sh
exec python3 "$out/fake-qmp-vm.py" "$t" "\$@"
EOF
    chmod 755 "$run_root/tools/$t"
done
# locachy-winmaint driver-seed double (the contract: IMG made, its path printed);
# every other command is the launcher double above.
mv "$run_root/tools/winmaint" "$run_root/tools/winmaint-vm"
cat >"$run_root/tools/winmaint" <<EOF
#!/bin/bash
if [ "\${1:-}" = driver-seed ]; then
    shift
    virtio= img=
    while [ \$# -gt 0 ]; do
        case \$1 in --virtio-iso) virtio=\$2; shift 2 ;; --out) img=\$2; shift 2 ;; *) shift ;; esac
    done
    printf 'driver-seed %s %s\n' "\$virtio" "\$img" >>"$run_root/driver-seed.calls"
    head -c 1048576 /dev/zero >"\$img"
    printf '%s\n' "\$img"
    exit 0
fi
exec "$run_root/tools/winmaint-vm" "\$@"
EOF
chmod 755 "$run_root/tools/winmaint"
: >"$run_root/driver-seed.calls"
# locachy-winiso double: path KIND prints TEST_WINISO_WINDOWS / TEST_WINISO_VIRTIO,
# or exits 3 (not available) when that one is unset.
cat >"$run_root/tools/winiso" <<'EOF'
#!/bin/bash
[ "${1:-}" = path ] || exit 2
case ${2:-} in
windows) v=${TEST_WINISO_WINDOWS:-} ;;
virtio-win) v=${TEST_WINISO_VIRTIO:-} ;;
*) exit 2 ;;
esac
[ -n "$v" ] || exit 3
printf '%s\n' "$v"
EOF
chmod 755 "$run_root/tools/winiso"

export LOCACHY_CGROUP_BASE="$run_root/cg"
export LOCACHY_CONT_BASE="$run_root/cont"
export LOCACHY_CONT_CONF="$run_root/conf"
export LOCACHY_APP_RUNTIME="$run_root/runtime"
export LOCACHY_APP_INIT="$out/app-init"
export LOCACHY_CLIPBOARD_TOOL="$out/fake-clipboard.sh"
export LOCACHY_WINMAINT_TOOL="$run_root/tools/winmaint"
export LOCACHY_COMPUTE_TOOL="$run_root/tools/compute"
export LOCACHY_VM_STATE="$run_root/vmstate"
export LOCACHY_WINISO_TOOL="$run_root/tools/winiso"
export XDG_RUNTIME_DIR="$run_root/xdg"
export LOCACHY_WAYLAND_DISPLAY="locachy-0"

# Fixture cgroup watcher: reflects cgroup.freeze and live PIDs into cgroup.events,
# and honors cgroup.kill writes by SIGKILLing PIDs recorded in cgroup.procs.
setup_cg() {
    local name="$1" cg="$run_root/cg/app_$1"
    mkdir -p "$cg"
    printf '0\n' >"$cg/cgroup.freeze"
    printf '0\n' >"$cg/cgroup.kill"
    : >"$cg/cgroup.procs"
    printf 'populated 0\nfrozen 0\n' >"$cg/cgroup.events"
    printf '0\n' >"$cg/memory.current"
    printf '0\n' >"$cg/memory.swap.current"
}

python3 -I - "$run_root/cg" "$run_root/watcher.pid" <<'PY' &
import os, signal, sys, time
from pathlib import Path

cg_root = Path(sys.argv[1])
Path(sys.argv[2]).write_text(str(os.getpid()), encoding="utf-8")

def alive(pid):
    try:
        st = Path(f"/proc/{pid}/stat").read_text()
        state = st.rsplit(")", 1)[1].strip().split()[0]
        return state != "Z"
    except OSError:
        return False

while True:
    for d in list(cg_root.glob("app_*")):
        if not d.is_dir():
            continue
        procs_f = d / "cgroup.procs"
        kill_f = d / "cgroup.kill"
        frz_f = d / "cgroup.freeze"
        ev_f = d / "cgroup.events"
        try:
            pids = [int(x) for x in procs_f.read_text().split() if x.isdigit()]
        except OSError:
            pids = []
        if kill_f.exists() and kill_f.read_text().strip() == "1":
            for p in pids:
                try:
                    os.kill(p, signal.SIGKILL)
                except OSError:
                    pass
            kill_f.write_text("0\n")
            Path(d / "killed.marker").write_text("1\n")
        live = [p for p in pids if alive(p)]
        frz = "1" if (frz_f.exists() and frz_f.read_text().strip() == "1") else "0"
        pop = "1" if live else "0"
        want = f"populated {pop}\nfrozen {frz}\n"
        try:
            if not ev_f.exists() or ev_f.read_text() != want:
                ev_f.write_text(want)
        except OSError:
            pass
    time.sleep(0.02)
PY
watcher_pid=$!
trap 'kill "$watcher_pid" 2>/dev/null || true' EXIT

app() {
    bash "$src/link/locachy-app" "$@"
}

# locachy-app remove goes through the system job queue, which does not exist in
# this namespace: do_remove runs from the definitions of locachy-app instead.
sed '/^operation=start/,$d' "$src/link/locachy-app" >"$out/app-defs.sh"
ln -sf "$src/link/locachy_config.py" "$out/locachy_config.py"
cat >"$out/remove-driver.sh" <<'SH'
#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/app-defs.sh"
do_remove "$1"
SH
chmod 755 "$out/remove-driver.sh"
remove_vm() { bash "$out/remove-driver.sh" "$1"; }

# A start that must be refused before anything runs: PHRASE is in its error.
expect_refused() { # NAME PHRASE
    local name="$1" phrase="$2" err
    if err=$(app start "$name" 2>&1); then
        fail "$name should be refused ($phrase)"
    elif grep -qF -- "$phrase" <<<"$err"; then
        pass "$name refused: $phrase"
    else
        fail "$name refused with an unexpected error: $err"
    fi
}

# --- Test A: build / install / rollback cleanly refuse vm="1" apps ---
touch "$run_root/imgs/fake-win.iso" "$run_root/imgs/fake-virtio.iso"
cat >"$run_root/conf/winmaint.conf" <<EOF
vm="1"
vm_kind="winmaint"
vm_freeze="qmp"
vm_memory="8192"
vm_cpus="4"
vm_iso="$run_root/imgs/fake-win.iso"
vm_nic="virtio"
vm_disk_bus="virtio"
vm_virtio_iso="$run_root/imgs/fake-virtio.iso"
network="1"
EOF
for op in build rollback; do
    err=$(app "$op" winmaint 2>&1 || true)
    if app "$op" winmaint >/dev/null 2>&1; then
        fail "$op should refuse vm=1 app"
    elif grep -q 'is a VM app: it has no container package build' <<<"$err"; then
        pass "$op cleanly refuses vm=1 app"
    else
        fail "$op unexpected error: $err"
    fi
done
err=$(app install winmaint sl 2>&1 || true)
if app install winmaint sl >/dev/null 2>&1; then
    fail "install should refuse vm=1 app"
elif grep -q 'is a VM app: it has no container package build' <<<"$err"; then
    pass "install cleanly refuses vm=1 app"
else
    fail "install unexpected error: $err"
fi

# --- Test B: winmaint kind start, Wayland env, QMP freeze/resume ordering, graceful stop ---
setup_cg winmaint
export TEST_VM_LOG="$run_root/winmaint.ev"
export TEST_VM_ENV="$run_root/winmaint.env.json"
export TEST_CLIP_LOG="$run_root/winmaint.clip"
: >"$TEST_VM_LOG"
: >"$TEST_CLIP_LOG"

app start winmaint >"$run_root/winmaint.out" 2>&1 &
start_pid=$!

# Wait for readiness and QMP socket.
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/winmaint.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
if [ -f "$run_root/runtime/winmaint.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG"; then
    pass "winmaint start: ready marker and QMP socket appeared"
else
    fail "winmaint start did not become ready: $(cat "$run_root/winmaint.out" 2>/dev/null || true)"
fi

# Verify argv + Wayland environment passed to launcher.
python3 -I - "$TEST_VM_ENV" "$run_root/xdg" "$run_root/vmstate/winmaint/qmp.sock" <<'PY'
import json, sys
rec = json.loads(open(sys.argv[1]).read())
xdg, qmp = sys.argv[2], sys.argv[3]
assert rec["WAYLAND_DISPLAY"] == "locachy-0", rec
assert rec["XDG_RUNTIME_DIR"] == xdg, rec
assert rec["GDK_BACKEND"] == "wayland", rec
assert rec["SDL_VIDEODRIVER"] == "wayland", rec
assert rec["LOCACHY_VM_QMP_SOCK"] == qmp, rec
argv = rec["argv"]
assert argv[1:3] == ["winmaint", "vm"], argv
assert "--display" in argv and argv[argv.index("--display") + 1] == "gtk", argv
assert "--qmp-sock" in argv and argv[argv.index("--qmp-sock") + 1] == qmp, argv
assert "--memory" in argv and argv[argv.index("--memory") + 1] == "8192", argv
assert "--cpus" in argv and argv[argv.index("--cpus") + 1] == "4", argv
assert "--iso" in argv and argv[argv.index("--iso") + 1].endswith("/imgs/fake-win.iso"), argv
assert "--nic" in argv and argv[argv.index("--nic") + 1] == "virtio", argv
assert "--disk-bus" in argv and argv[argv.index("--disk-bus") + 1] == "virtio", argv
assert "--virtio-iso" in argv and argv[argv.index("--virtio-iso") + 1].endswith("/imgs/fake-virtio.iso"), argv
print("PASS: winmaint receives Wayland env, QMP sock, configured memory/cpus/iso, and NIC/disk bus/virtio-win ISO")
PY

# Record freeze ordering by watching combined event order (QMP stop BEFORE cgroup.freeze=1).
# Replace fake-clipboard to also write to TEST_VM_LOG so ordering is captured in one file.
cat >"$out/fake-clipboard-ordered.sh" <<'SH'
#!/bin/bash
set -euo pipefail
case "${1:-}" in
sync)
    echo "CLIP:sync" >>"$TEST_VM_LOG"
    ;;
freeze)
    cg="$2"
    echo "CLIP:freeze" >>"$TEST_VM_LOG"
    printf '1\n' >"$cg/cgroup.freeze"
    printf 'populated 1\nfrozen 1\n' >"$cg/cgroup.events"
    ;;
esac
SH
chmod 755 "$out/fake-clipboard-ordered.sh"
LOCACHY_CLIPBOARD_TOOL="$out/fake-clipboard-ordered.sh" app freeze winmaint
if [ "$(cat "$run_root/cg/app_winmaint/cgroup.freeze")" = 1 ] && [ -f "$run_root/runtime/winmaint/vm-paused" ]; then
    pass "winmaint freeze: cgroup frozen and vm-paused marker set"
else
    fail "winmaint freeze state mismatch"
fi

# Verify QMP:stop happened BEFORE CLIP:freeze.
python3 -I - "$TEST_VM_LOG" <<'PY'
import sys
evs = [l.strip() for l in open(sys.argv[1]) if l.strip()]
assert "QMP:stop" in evs and "CLIP:freeze" in evs, evs
assert evs.index("QMP:stop") < evs.index("CLIP:freeze"), f"QMP stop must precede cgroup freeze: {evs}"
print("PASS: freeze sends QMP stop BEFORE freezing the cgroup")
PY

# Single-instance check: again/new/open on a paused VM resumes it, never spawns a second VM.
app again winmaint
if [ "$(cat "$run_root/cg/app_winmaint/cgroup.freeze")" = 0 ] && [ ! -e "$run_root/runtime/winmaint/vm-paused" ] &&
   grep -q '^QMP:cont$' "$TEST_VM_LOG"; then
    pass "again on frozen VM thaws cgroup and sends QMP cont without spawning a second instance"
else
    fail "again on frozen VM failed: $(cat "$TEST_VM_LOG")"
fi

# Failed clipboard handover must fail closed: QMP cont sent, vm-paused removed, cgroup NOT frozen.
cat >"$out/fake-clipboard-fail.sh" <<'SH'
#!/bin/bash
set -euo pipefail
case "${1:-}" in
sync) echo "CLIP:sync-fail" >>"$TEST_VM_LOG" ;;
freeze) echo "CLIP:freeze-fail" >>"$TEST_VM_LOG"; exit 1 ;;
esac
SH
chmod 755 "$out/fake-clipboard-fail.sh"
if LOCACHY_CLIPBOARD_TOOL="$out/fake-clipboard-fail.sh" app freeze winmaint >/dev/null 2>&1; then
    fail "freeze should fail closed when clipboard handover fails"
elif [ "$(cat "$run_root/cg/app_winmaint/cgroup.freeze")" = 0 ] && [ ! -e "$run_root/runtime/winmaint/vm-paused" ]; then
    pass "failed clipboard handover fails closed, keeps cgroup thawed, and resumes VM via QMP cont"
else
    fail "failed clipboard handover left bad state"
fi

# Freeze again, then stop: must thaw -> QMP cont -> QMP system_powerdown -> clean exit (NO cgroup.kill).
LOCACHY_CLIPBOARD_TOOL="$out/fake-clipboard-ordered.sh" app freeze winmaint
app stop winmaint
wait "$start_pid" 2>/dev/null || true
python3 -I - "$TEST_VM_LOG" "$run_root/cg/app_winmaint/killed.marker" <<'PY'
import os, sys
evs = [l.strip() for l in open(sys.argv[1]) if l.strip()]
# Expect three QMP:stop (initial freeze, failed-handover freeze, final freeze),
# three QMP:cont (again, rollback on failed handover, stop), and QMP:system_powerdown.
assert evs.count("QMP:stop") == 3, evs
assert evs.count("QMP:cont") == 3, evs
last_cont = len(evs) - 1 - evs[::-1].index("QMP:cont")
pd_idx = evs.index("QMP:system_powerdown")
assert last_cont < pd_idx, f"QMP cont must precede system_powerdown on stop: {evs}"
assert not os.path.exists(sys.argv[2]), "cgroup.kill must not be triggered when guest honors system_powerdown"
print("PASS: stop thaws + sends QMP cont -> QMP system_powerdown and exits cleanly without cgroup.kill")
PY

# --- Test C: compute kind + vm_freeze="none" (freeze is a no-op) ---
cat >"$run_root/conf/devbox.conf" <<'EOF'
vm="1"
vm_kind="compute"
vm_name="ubuntu-dev"
vm_display="sdl"
vm_freeze="none"
network="1"
EOF
setup_cg devbox
export TEST_VM_LOG="$run_root/devbox.ev"
export TEST_VM_ENV="$run_root/devbox.env.json"
: >"$TEST_VM_LOG"

app start devbox >"$run_root/devbox.out" 2>&1 &
start_pid=$!
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/devbox.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
python3 -I - "$TEST_VM_ENV" "$run_root/vmstate/devbox/qmp.sock" <<'PY'
import json, sys
rec = json.loads(open(sys.argv[1]).read())
qmp = sys.argv[2]
argv = rec["argv"]
assert argv[1:4] == ["compute", "vm", "run"], argv
assert argv[4] == "ubuntu-dev", argv
assert "--display" in argv and argv[argv.index("--display") + 1] == "sdl", argv
assert "--qmp-sock" in argv and argv[argv.index("--qmp-sock") + 1] == qmp, argv
print("PASS: compute kind invokes locachy-compute vm run NAME --display sdl --qmp-sock ...")
PY

app freeze devbox
if [ "$(cat "$run_root/cg/app_devbox/cgroup.freeze")" = 0 ] && ! grep -q '^QMP:stop$' "$TEST_VM_LOG"; then
    pass "vm_freeze=\"none\" leaves VM running and sends no QMP stop or cgroup freeze"
else
    fail "vm_freeze=\"none\" should be a no-op"
fi

# Stop with ignore-powerdown would test SIGTERM fallback; test normal graceful stop here first.
app stop devbox
wait "$start_pid" 2>/dev/null || true
if grep -q '^QMP:system_powerdown$' "$TEST_VM_LOG" && [ ! -e "$run_root/cg/app_devbox/killed.marker" ]; then
    pass "compute VM stops cleanly via QMP system_powerdown"
else
    fail "compute VM stop failed"
fi

# --- Test D: SIGTERM fallback when guest ignores ACPI powerdown ---
setup_cg devbox
: >"$TEST_VM_LOG"
TEST_VM_MODE=ignore-powerdown app start devbox >"$run_root/devbox-term.out" 2>&1 &
start_pid=$!
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/devbox.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
# Reduce wait polls for this test only? No: do_stop waits up to ~8s then sends SIGTERM; that fits easily.
app stop devbox
wait "$start_pid" 2>/dev/null || true
if grep -q '^QMP:system_powerdown$' "$TEST_VM_LOG" && grep -q '^SIGNAL:SIGTERM$' "$TEST_VM_LOG" &&
   [ ! -e "$run_root/cg/app_devbox/killed.marker" ]; then
    pass "stop falls back to SIGTERM after ACPI powerdown timeout before cgroup.kill"
else
    fail "SIGTERM fallback did not behave as expected: $(cat "$TEST_VM_LOG")"
fi

# --- Test E: custom command= launcher ---
cat >"$run_root/conf/customcmd.conf" <<EOF
vm="1"
vm_kind="custom"
command="$run_root/tools/custom-vm"
arguments="--flag alpha"
network="0"
EOF
setup_cg customcmd
export TEST_VM_LOG="$run_root/customcmd.ev"
export TEST_VM_ENV="$run_root/customcmd.env.json"
: >"$TEST_VM_LOG"
app start customcmd >"$run_root/customcmd.out" 2>&1 &
start_pid=$!
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/customcmd.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
python3 -I - "$TEST_VM_ENV" <<'PY'
import json, sys
rec = json.loads(open(sys.argv[1]).read())
assert rec["argv"][1:] == ["custom-vm", "--flag", "alpha"], rec["argv"]
assert rec["WAYLAND_DISPLAY"] == "locachy-0", rec
print("PASS: custom command= VM runs configured command + arguments with Wayland + QMP env")
PY
app stop customcmd
wait "$start_pid" 2>/dev/null || true

# --- Test F: Declarative custom VM (real qemu-img overlay, closed key set,
#             Linux + Windows argv, swtpm + virtiofsd helpers, safe remove) ---
qemu-img create -f qcow2 "$run_root/imgs/base.qcow2" 16M >/dev/null
qemu-img create -f raw "$run_root/imgs/direct.raw" 8M >/dev/null
touch "$run_root/imgs/installer.iso" "$run_root/imgs/virtio-win.iso"

# Unknown vm_* key must be refused.
cat >"$run_root/conf/badkey.conf" <<EOF
vm="1"
vm_kind="custom"
vm_base="$run_root/imgs/base.qcow2"
vm_qemu_args="-cpu evil"
EOF
setup_cg badkey
expect_refused badkey "unknown vm_qemu_args"

# Keys that do not apply to a kind, and command= mixed with vm_base/vm_disk, must be refused.
cat >"$run_root/conf/badwinmaint.conf" <<EOF
vm="1"
vm_kind="winmaint"
vm_base="$run_root/imgs/base.qcow2"
EOF
setup_cg badwinmaint
expect_refused badwinmaint "vm_base is for vm_kind custom only"

cat >"$run_root/conf/badcompute.conf" <<EOF
vm="1"
vm_kind="compute"
vm_memory="2048"
EOF
setup_cg badcompute
expect_refused badcompute "vm_memory does not apply to vm_kind compute"

cat >"$run_root/conf/badname.conf" <<EOF
vm="1"
vm_kind="custom"
vm_name="other"
vm_base="$run_root/imgs/base.qcow2"
EOF
setup_cg badname
expect_refused badname "vm_name applies to vm_kind compute only"

cat >"$run_root/conf/cmdwithbase.conf" <<EOF
vm="1"
vm_kind="custom"
command="$run_root/tools/custom-vm"
vm_base="$run_root/imgs/base.qcow2"
EOF
setup_cg cmdwithbase
expect_refused cmdwithbase "command= is exclusive with vm_base and vm_disk"

# Both vm_base and vm_disk must be refused.
cat >"$run_root/conf/bothdisks.conf" <<EOF
vm="1"
vm_kind="custom"
vm_base="$run_root/imgs/base.qcow2"
vm_disk="$run_root/imgs/direct.raw"
EOF
setup_cg bothdisks
expect_refused bothdisks "not both"

# Relative path and path inside VM_STATE must be refused.
cat >"$run_root/conf/badpath.conf" <<EOF
vm="1"
vm_kind="custom"
vm_base="relative.qcow2"
EOF
setup_cg badpath
expect_refused badpath "must be an absolute path"

cat >"$run_root/conf/instate.conf" <<EOF
vm="1"
vm_kind="custom"
vm_disk="$run_root/vmstate/instate/direct.raw"
EOF
mkdir -p "$run_root/vmstate/instate"
cp "$run_root/imgs/direct.raw" "$run_root/vmstate/instate/direct.raw"
setup_cg instate
expect_refused instate "which remove deletes"

# Linux declarative VM with vm_audio="1": refused when restricted Pulse socket is absent,
# accepted once the socket exists (mounted in this private mount namespace).
cat >"$run_root/conf/linuxvm.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="$run_root/imgs/base.qcow2"
vm_memory="2048"
vm_cpus="2"
vm_gpu="gl"
vm_share_home="1"
vm_audio="1"
network="1"
EOF
setup_cg linuxvm
# Mount a clean tmpfs on /run/pulse inside this unshared mount namespace to test both states.
mount -t tmpfs tmpfs /run
mkdir -p /run/pulse/restricted
LOCACHY_QEMU_BIN="$out/fake-qmp-vm.py" LOCACHY_VIRTIOFSD="$out/fake-helper.py" \
    expect_refused linuxvm "needs the sound server"
python3 -I - /run/pulse/restricted/native <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
s.listen(1)
PY

export TEST_VM_LOG="$run_root/linuxvm.ev"
export TEST_VM_ENV="$run_root/linuxvm.env.json"
: >"$TEST_VM_LOG"
LOCACHY_QEMU_BIN="$out/fake-qmp-vm.py" LOCACHY_VIRTIOFSD="$out/fake-helper.py" \
    app start linuxvm >"$run_root/linuxvm.out" 2>&1 &
start_pid=$!
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/linuxvm.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
python3 -I - "$TEST_VM_ENV" "$run_root/vmstate/linuxvm/disk.qcow2" "$run_root/imgs/base.qcow2" <<'PY'
import json, subprocess, sys
rec = json.loads(open(sys.argv[1]).read())
overlay, base = sys.argv[2], sys.argv[3]
argv = rec["argv"]
assert "-device" in argv and "virtio-vga-gl" in argv, argv
assert "gtk,gl=on,window-close=off" in argv, argv
assert argv[argv.index("-name") + 1] == "locachy-vm[linuxvm]", argv
assert "memory-backend-memfd,id=mem,size=2048M,share=on" in argv, argv
assert any(a.startswith("vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=home") for a in argv), argv
assert any("virtio-blk-pci,drive=hd0,iothread=io0" == a for a in argv), argv
assert any(overlay in a and "aio=io_uring" in a for a in argv), argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=virtio-net-pci", argv
assert "-audiodev" in argv and "pa,id=snd0" in argv, argv
info = json.loads(subprocess.check_output(["qemu-img", "info", "--output=json", overlay], text=True))
assert info["format"] == "qcow2" and info["backing-filename"] == base, info
print("PASS: declarative Linux custom VM creates real qcow2 overlay, starts virtiofsd, and builds expected QEMU argv")
PY
app stop linuxvm
wait "$start_pid" 2>/dev/null || true

# Windows declarative VM with vm_disk + vm_tpm=1 + vm_iso + vm_virtio_iso + network=1 (default e1000e).
cat >"$run_root/conf/winvm.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/direct.raw"
vm_uefi="0"
vm_tpm="1"
vm_iso="$run_root/imgs/installer.iso"
vm_virtio_iso="$run_root/imgs/virtio-win.iso"
network="1"
EOF
setup_cg winvm
export TEST_VM_LOG="$run_root/winvm.ev"
export TEST_VM_ENV="$run_root/winvm.env.json"
: >"$TEST_VM_LOG"
LOCACHY_QEMU_BIN="$out/fake-qmp-vm.py" LOCACHY_SWTPM="$out/fake-helper.py" \
    app start winvm >"$run_root/winvm.out" 2>&1 &
start_pid=$!
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/winvm.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
python3 -I - "$TEST_VM_ENV" "$run_root/imgs/direct.raw" "$run_root/imgs/installer.iso" "$run_root/imgs/virtio-win.iso" \
    "$run_root/vmstate/winvm/drivers.img" "$run_root/driver-seed.calls" <<'PY'
import json, os, sys
rec = json.loads(open(sys.argv[1]).read())
direct, installer, virtio, seed, calls = sys.argv[2:7]
argv = rec["argv"]
assert "gtk,gl=off,window-close=off" in argv, argv
assert argv[argv.index("-name") + 1] == "locachy-vm[winvm]", argv
assert "virtio-vga" in argv and "virtio-vga-gl" not in argv, argv
assert any(f"file={direct},format=raw" in a for a in argv), argv
assert "virtio-blk-pci,drive=hd0,iothread=io0" in argv, argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=virtio-net-pci", argv
assert "tpm-tis,tpmdev=tpm0" in argv, argv
assert f"file={installer},media=cdrom,readonly=on,if=none,id=cd0" in argv and "ide-cd,drive=cd0,bus=ide.1,bootindex=1" in argv, argv
assert f"file={virtio},media=cdrom,readonly=on,if=none,id=cd1" in argv, argv
assert f"file={seed},format=raw,readonly=on,if=none,id=drv0" in argv, argv
assert "usb-storage,drive=drv0,bus=xhci.0,removable=on" in argv and "qemu-xhci,id=xhci" in argv, argv
assert f"driver-seed {virtio} {seed}" in open(calls).read().splitlines(), open(calls).read()
if os.access("/dev/kvm", os.R_OK | os.W_OK):
    cpu = argv[argv.index("-cpu") + 1]
    for flag in ("hv-relaxed", "hv-vapic", "hv-spinlocks=0x1fff", "hv-time",
                 "hv-vpindex", "hv-synic", "hv-stimer", "hv-stimer-direct", "hv-reset",
                 "hv-frequencies", "hv-tlbflush", "hv-ipi"):
        assert flag in cpu, f"missing {flag} in {cpu}"
print("PASS: declarative Windows custom VM with a virtio-win ISO on a blank disk: Hyper-V flags, virtio-blk, virtio-net, drivers seed on usb-storage, TPM helper")
PY
app stop winvm
wait "$start_pid" 2>/dev/null || true
if grep -qx 'vm_disk_bus="virtio"' "$run_root/conf/winvm.conf" && grep -qx 'vm_nic="virtio"' "$run_root/conf/winvm.conf"; then
    pass "winvm: the chosen defaults are recorded in its configuration"
else
    fail "winvm: defaults not recorded: $(cat "$run_root/conf/winvm.conf")"
fi

# Windows without a virtio-win ISO has no viostor, so it keeps NVMe (the bus Windows ships a driver for).
cat >"$run_root/conf/winnoiso.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/direct.raw"
vm_uefi="0"
vm_tpm="0"
vm_iso="$run_root/imgs/installer.iso"
network="1"
EOF
setup_cg winnoiso
export TEST_VM_LOG="$run_root/winnoiso.ev"
export TEST_VM_ENV="$run_root/winnoiso.env.json"
: >"$TEST_VM_LOG"
LOCACHY_QEMU_BIN="$out/fake-qmp-vm.py" LOCACHY_SWTPM="$out/fake-helper.py" \
    app start winnoiso >"$run_root/winnoiso.out" 2>&1 &
start_pid=$!
for _ in $(seq 1 150); do
    [ -f "$run_root/runtime/winnoiso.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
    sleep 0.05
done
python3 -I - "$TEST_VM_ENV" <<'PY'
import json, sys
argv = json.loads(open(sys.argv[1]).read())["argv"]
assert any(a.startswith("nvme,drive=hd0,serial=locachy-winnoiso") for a in argv), argv
assert not any("virtio-blk-pci" in a for a in argv), argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=e1000e", argv
assert not any("usb-storage" in a or "id=cd1" in a for a in argv), argv
print("PASS: a Windows custom VM without virtio-win (none set, none supplied) gets NVMe and e1000e, which Windows has drivers for")
PY
app stop winnoiso
wait "$start_pid" 2>/dev/null || true
if grep -q 'virtio-win is not available' "$run_root/winnoiso.out" &&
   grep -qx 'vm_disk_bus="nvme"' "$run_root/conf/winnoiso.conf" && grep -qx 'vm_nic="e1000e"' "$run_root/conf/winnoiso.conf"; then
    pass "winnoiso: the reason for NVMe/e1000e is logged and the defaults are recorded"
else
    fail "winnoiso: no reason logged or defaults not recorded: $(cat "$run_root/winnoiso.out") / $(cat "$run_root/conf/winnoiso.conf")"
fi

# start_vm NAME [ENV=VALUE...]: start NAME with the fake QEMU (env recorded in
# $run_root/NAME.env.json, output in NAME.out) and wait until it is ready.
start_vm() {
    local name="$1"; shift
    export TEST_VM_LOG="$run_root/$name.ev" TEST_VM_ENV="$run_root/$name.env.json"
    : >"$TEST_VM_LOG"
    rm -f "$TEST_VM_ENV"
    env "$@" LOCACHY_QEMU_BIN="$out/fake-qmp-vm.py" LOCACHY_SWTPM="$out/fake-helper.py" \
        bash "$src/link/locachy-app" start "$name" >"$run_root/$name.out" 2>&1 &
    start_pid=$!
    for _ in $(seq 1 150); do
        [ -f "$run_root/runtime/$name.ready" ] && grep -q '^QMP_READY$' "$TEST_VM_LOG" 2>/dev/null && break
        sleep 0.05
    done
}
stop_vm() {
    app stop "$1"
    wait "$start_pid" 2>/dev/null || true
}
mkdir -p "$run_root/isos"
touch "$run_root/isos/windows-11-en-US.iso" "$run_root/isos/virtio-win.iso"
supplied=(TEST_WINISO_WINDOWS="$run_root/isos/windows-11-en-US.iso" TEST_WINISO_VIRTIO="$run_root/isos/virtio-win.iso")

# Neither ISO set: locachy-winiso supplies both; a blank disk boots the installer
# first, with the drivers seed, virtio-blk and virtio-net.
cp "$run_root/imgs/direct.raw" "$run_root/imgs/auto.raw"
cat >"$run_root/conf/winauto.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/auto.raw"
vm_uefi="0"
vm_tpm="0"
network="1"
EOF
setup_cg winauto
start_vm winauto "${supplied[@]}"
python3 -I - "$run_root/winauto.env.json" "$run_root/isos" "$run_root/vmstate/winauto/drivers.img" "$run_root/driver-seed.calls" <<'PY'
import json, sys
argv = json.loads(open(sys.argv[1]).read())["argv"]
isos, seed, calls = sys.argv[2:5]
assert f"file={isos}/windows-11-en-US.iso,media=cdrom,readonly=on,if=none,id=cd0" in argv, argv
assert "ide-cd,drive=cd0,bus=ide.1,bootindex=1" in argv, argv
assert f"file={isos}/virtio-win.iso,media=cdrom,readonly=on,if=none,id=cd1" in argv, argv
assert f"file={seed},format=raw,readonly=on,if=none,id=drv0" in argv and "usb-storage,drive=drv0,bus=xhci.0,removable=on" in argv, argv
assert "virtio-blk-pci,drive=hd0,iothread=io0" in argv, argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=virtio-net-pci", argv
assert f"driver-seed {isos}/virtio-win.iso {seed}" in open(calls).read().splitlines(), open(calls).read()
print("PASS: a Windows custom VM without vm_iso/vm_virtio_iso gets both from locachy-winiso; blank disk: installer first, drivers seed, virtio-blk, virtio-net")
PY
stop_vm winauto
# Windows is on the disk now (data on it): the installer no longer boots first, the
# recorded defaults stay, and the seed is not made again (the ISO is not newer).
printf 'installed' | dd of="$run_root/imgs/auto.raw" conv=notrunc status=none
calls_before=$(wc -l <"$run_root/driver-seed.calls")
start_vm winauto "${supplied[@]}"
python3 -I - "$run_root/winauto.env.json" <<'PY'
import json, sys
argv = json.loads(open(sys.argv[1]).read())["argv"]
assert "ide-cd,drive=cd0,bus=ide.1" in argv and "ide-cd,drive=cd0,bus=ide.1,bootindex=1" not in argv, argv
assert "virtio-blk-pci,drive=hd0,iothread=io0" in argv, argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=virtio-net-pci", argv
assert "usb-storage,drive=drv0,bus=xhci.0,removable=on" in argv, argv
print("PASS: once the disk has data the supplied installer is attached without bootindex and the recorded virtio defaults stay")
PY
stop_vm winauto
if [ "$(wc -l <"$run_root/driver-seed.calls")" = "$calls_before" ]; then
    pass "winauto: the drivers seed is reused, not made again"
else
    fail "winauto: the drivers seed was made again: $(cat "$run_root/driver-seed.calls")"
fi

# A disk with data and no recorded defaults (a VM from before this change):
# the defaults it had then (NVMe, e1000e), even though virtio-win is supplied now.
cp "$run_root/imgs/auto.raw" "$run_root/imgs/legacy.raw"
cat >"$run_root/conf/winlegacy.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/legacy.raw"
vm_virtio_iso="$run_root/imgs/virtio-win.iso"
vm_uefi="0"
vm_tpm="0"
network="1"
EOF
setup_cg winlegacy
start_vm winlegacy "${supplied[@]}"
python3 -I - "$run_root/winlegacy.env.json" <<'PY'
import json, sys
argv = json.loads(open(sys.argv[1]).read())["argv"]
assert any(a.startswith("nvme,drive=hd0,serial=locachy-winlegacy") for a in argv), argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=e1000e", argv
assert "ide-cd,drive=cd0,bus=ide.1,bootindex=1" not in argv, argv
print("PASS: a Windows VM whose disk has data keeps NVMe/e1000e when virtio-win becomes available")
PY
stop_vm winlegacy
if grep -qx 'vm_disk_bus="nvme"' "$run_root/conf/winlegacy.conf" && grep -qx 'vm_nic="e1000e"' "$run_root/conf/winlegacy.conf"; then
    pass "winlegacy: its old defaults are recorded in its configuration"
else
    fail "winlegacy: defaults not recorded: $(cat "$run_root/conf/winlegacy.conf")"
fi

# Explicit keys win over the defaults, and are not rewritten.
cp "$run_root/imgs/direct.raw" "$run_root/imgs/explicit.raw"
cat >"$run_root/conf/winexplicit.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/explicit.raw"
vm_uefi="0"
vm_tpm="0"
vm_disk_bus="sata"
vm_nic="e1000e"
network="1"
EOF
cp "$run_root/conf/winexplicit.conf" "$run_root/winexplicit.conf.before"
setup_cg winexplicit
start_vm winexplicit "${supplied[@]}"
python3 -I - "$run_root/winexplicit.env.json" <<'PY'
import json, sys
argv = json.loads(open(sys.argv[1]).read())["argv"]
assert "ide-hd,drive=hd0,bus=ide.0" in argv and not any("virtio-blk-pci" in a for a in argv), argv
assert "-nic" in argv and argv[argv.index("-nic") + 1] == "user,model=e1000e", argv
assert "usb-storage,drive=drv0,bus=xhci.0,removable=on" in argv, argv
print("PASS: explicit vm_disk_bus/vm_nic win over the virtio defaults")
PY
stop_vm winexplicit
if cmp -s "$run_root/conf/winexplicit.conf" "$run_root/winexplicit.conf.before"; then
    pass "winexplicit: its configuration is left as it was"
else
    fail "winexplicit: configuration rewritten: $(cat "$run_root/conf/winexplicit.conf")"
fi

# SDL too: closing the window never quits the guest (window-close=off); the window tag.
cp "$run_root/imgs/direct.raw" "$run_root/imgs/sdl.raw"
cat >"$run_root/conf/winsdl.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/sdl.raw"
vm_uefi="0"
vm_tpm="0"
vm_display="sdl"
network="0"
EOF
setup_cg winsdl
start_vm winsdl
python3 -I - "$run_root/winsdl.env.json" <<'PY'
import json, sys
argv = json.loads(open(sys.argv[1]).read())["argv"]
assert "sdl,gl=off,window-close=off" in argv, argv
assert argv[argv.index("-name") + 1] == "locachy-vm[winsdl]", argv
print("PASS: an SDL custom VM gets window-close=off (closing its window does not quit the guest) and the tag locachy-vm[NAME]")
PY
stop_vm winsdl

# A helper that fails to start (virtiofsd here) stops the helpers already
# running (swtpm), and nothing is left in app_NAME, no ready marker.
cat >"$run_root/tools/fail-helper" <<'SH'
#!/bin/sh
echo "fail-helper: not starting" >&2
exit 1
SH
chmod 755 "$run_root/tools/fail-helper"
cat >"$run_root/conf/helperfail.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="$run_root/imgs/base.qcow2"
vm_uefi="0"
vm_tpm="1"
vm_share_home="1"
vm_display="none"
network="0"
EOF
setup_cg helperfail
: >"$run_root/helper.log"
if TEST_HELPER_LOG="$run_root/helper.log" LOCACHY_QEMU_BIN="$out/fake-qmp-vm.py" LOCACHY_SWTPM="$out/fake-helper.py" \
    LOCACHY_VIRTIOFSD="$run_root/tools/fail-helper" app start helperfail >"$run_root/helperfail.out" 2>&1; then
    fail "a VM whose virtiofsd does not start must not start"
else
    # Fixture cgroup.events is maintained by a background poller; give it a bounded
    # window under parallel test load to observe that every process in app_helperfail ended.
    for _ in $(seq 1 100); do
        grep -qx 'populated 0' "$run_root/cg/app_helperfail/cgroup.events" && break
        sleep 0.05
    done
    if grep -q 'a helper ended before it made' "$run_root/helperfail.out" &&
       grep -q '^started .*swtpm.sock$' "$run_root/helper.log" && grep -q '^stopped .*swtpm.sock$' "$run_root/helper.log" &&
       [ ! -e "$run_root/runtime/helperfail.ready" ] && grep -qx 'populated 0' "$run_root/cg/app_helperfail/cgroup.events"; then
        pass "a helper that fails to start stops the helpers already started (swtpm); app_NAME empty, no ready marker"
    else
        fail "helper failure left state behind: $(cat "$run_root/helperfail.out") / $(cat "$run_root/helper.log") / $(cat "$run_root/cg/app_helperfail/cgroup.events")"
    fi
fi

# An ISO that is not a regular file (a directory, a device) is refused, for
# custom VMs and for what is forwarded to winmaint.
cat >"$run_root/conf/isodir.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="$run_root/imgs/base.qcow2"
vm_iso="$run_root/imgs"
network="0"
EOF
setup_cg isodir
expect_refused isodir "vm_iso must be a regular file"
cat >"$run_root/conf/devnulliso.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="$run_root/imgs/base.qcow2"
vm_virtio_iso="/dev/null"
network="0"
EOF
setup_cg devnulliso
expect_refused devnulliso "vm_virtio_iso must be a regular file"
cat >"$run_root/conf/winisodir.conf" <<EOF
vm="1"
vm_kind="winmaint"
vm_iso="$run_root/imgs"
EOF
setup_cg winisodir
expect_refused winisodir "vm_iso must be a regular file"

# vm_name is checked even when start is given a command (vm_command skipped).
cat >"$run_root/conf/badvmname.conf" <<'EOF'
vm="1"
vm_kind="compute"
vm_name="bad/name"
EOF
setup_cg badvmname
if err=$(app start badvmname /bin/true 2>&1); then
    fail "start NAME COMMAND must refuse an invalid vm_name"
elif grep -qF 'invalid vm_name' <<<"$err"; then
    pass "start NAME COMMAND refuses an invalid vm_name before anything runs"
else
    fail "start NAME COMMAND refused with an unexpected error: $err"
fi

# The compute run directory's parent: uid 1000 passes through it, cannot list it.
# Static: that path runs only as real root.
if grep -q 'install -d -m 0711 /run/locachy/compute$' "$src/link/locachy-app"; then
    pass "/run/locachy/compute is made 0711 (static check)"
else
    fail "/run/locachy/compute is not made 0711"
fi

# vm_gpu="gl" must be refused for Windows guests.
cat >"$run_root/conf/wingl.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_disk="$run_root/imgs/direct.raw"
vm_uefi="0"
vm_tpm="0"
vm_gpu="gl"
EOF
setup_cg wingl
expect_refused wingl "Linux guests only"

# --- Test G: do_remove deletes conf + VM_STATE/NAME but NEVER deletes vm_base/vm_disk ---
remove_vm linuxvm
if [ ! -e "$run_root/conf/linuxvm.conf" ] && [ ! -e "$run_root/vmstate/linuxvm" ] &&
   [ -f "$run_root/imgs/base.qcow2" ]; then
    pass "remove deletes linuxvm.conf and managed overlay while preserving user vm_base"
else
    fail "remove linuxvm state mismatch"
fi

remove_vm winvm
if [ ! -e "$run_root/conf/winvm.conf" ] && [ ! -e "$run_root/vmstate/winvm" ] &&
   [ -f "$run_root/imgs/direct.raw" ]; then
    pass "remove deletes winvm.conf and managed state while preserving user vm_disk"
else
    fail "remove winvm state mismatch"
fi

# --- Test H: Real QEMU/KVM argv + QMP stop/cont/quit acceptance (headless) ---
if command -v qemu-system-x86_64 >/dev/null 2>&1 && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    export TEST_CLIP_LOG="$run_root/realqemu.clip"
    : >"$TEST_CLIP_LOG"
    cat >"$run_root/conf/realqemu.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_base="$run_root/imgs/base.qcow2"
vm_memory="256"
vm_cpus="1"
vm_uefi="0"
vm_tpm="0"
vm_display="none"
vm_audio="0"
network="0"
EOF
    setup_cg realqemu
    app start realqemu >"$run_root/realqemu.out" 2>&1 &
    rq_pid=$!
    rq_sock="$run_root/vmstate/realqemu/qmp.sock"
    for _ in $(seq 1 150); do
        [ -S "$rq_sock" ] && break
        kill -0 "$rq_pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ -S "$rq_sock" ]; then
        LOCACHY_CLIPBOARD_TOOL="$out/fake-clipboard.sh" app freeze realqemu
        app resume realqemu
        # Send QMP quit so headless QEMU without guest OS exits cleanly.
        python3 -I - "$rq_sock" <<'PY'
import socket, sys
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.settimeout(3)
    s.connect(sys.argv[1])
    s.recv(4096)
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    s.recv(4096)
    s.sendall(b'{"execute":"quit"}\n')
    s.recv(4096)
PY
        wait "$rq_pid" 2>/dev/null || true
        pass "real qemu-system-x86_64 + KVM accepts declarative Windows argv (hv-* + NVMe) and QMP stop/cont/quit"
    else
        kill "$rq_pid" 2>/dev/null || true
        wait "$rq_pid" 2>/dev/null || true
        fail "real qemu-system-x86_64 (Windows) did not open QMP socket: $(cat "$run_root/realqemu.out" 2>/dev/null || true)"
    fi

    cat >"$run_root/conf/reallinux.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="linux"
vm_base="$run_root/imgs/base.qcow2"
vm_memory="256"
vm_cpus="1"
vm_uefi="0"
vm_tpm="0"
vm_display="none"
vm_audio="0"
network="1"
EOF
    setup_cg reallinux
    app start reallinux >"$run_root/reallinux.out" 2>&1 &
    rl_pid=$!
    rl_sock="$run_root/vmstate/reallinux/qmp.sock"
    for _ in $(seq 1 150); do
        [ -S "$rl_sock" ] && break
        kill -0 "$rl_pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ -S "$rl_sock" ]; then
        python3 -I - "$rl_sock" <<'PY'
import socket, sys
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.settimeout(3)
    s.connect(sys.argv[1])
    s.recv(4096)
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    s.recv(4096)
    s.sendall(b'{"execute":"quit"}\n')
    s.recv(4096)
PY
        wait "$rl_pid" 2>/dev/null || true
        pass "real qemu-system-x86_64 + KVM accepts declarative Linux argv (virtio-blk iothread io_uring + virtio-net)"
    else
        kill "$rl_pid" 2>/dev/null || true
        wait "$rl_pid" 2>/dev/null || true
        fail "real qemu-system-x86_64 (Linux) did not open QMP socket: $(cat "$run_root/reallinux.out" 2>/dev/null || true)"
    fi

    # Supplied installer and virtio-win CDs, the drivers seed on usb-storage
    # (xhci without a display), virtio-blk and virtio-net, as a blank Windows VM gets them.
    head -c 1048576 /dev/zero >"$run_root/isos/real-windows.iso"
    head -c 1048576 /dev/zero >"$run_root/isos/real-virtio-win.iso"
    cat >"$run_root/conf/realwinauto.conf" <<EOF
vm="1"
vm_kind="custom"
vm_os="windows"
vm_base="$run_root/imgs/base.qcow2"
vm_memory="256"
vm_cpus="1"
vm_uefi="0"
vm_tpm="0"
vm_display="none"
network="1"
EOF
    setup_cg realwinauto
    TEST_WINISO_WINDOWS="$run_root/isos/real-windows.iso" TEST_WINISO_VIRTIO="$run_root/isos/real-virtio-win.iso" \
        app start realwinauto >"$run_root/realwinauto.out" 2>&1 &
    rw_pid=$!
    rw_sock="$run_root/vmstate/realwinauto/qmp.sock"
    for _ in $(seq 1 150); do
        [ -S "$rw_sock" ] && break
        kill -0 "$rw_pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ -S "$rw_sock" ]; then
        rw_ok=0
        python3 -I - "$rw_sock" <<'PY' || rw_ok=$?
import json, socket, sys
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.settimeout(3)
    s.connect(sys.argv[1])
    f = s.makefile("rw", encoding="utf-8")
    f.readline()
    for cmd in ("qmp_capabilities", "query-block"):
        f.write(json.dumps({"execute": cmd}) + "\n")
        f.flush()
        while True:
            msg = json.loads(f.readline())
            if "return" in msg or "error" in msg:
                break
    devices = {b.get("device") or b.get("qdev", "") for b in msg["return"]}
    assert {"cd0", "cd1", "drv0", "hd0"} <= devices, devices
    try:
        f.write('{"execute":"quit"}\n')
        f.flush()
        f.readline()
    except (OSError, ValueError):
        pass
PY
        if [ "$rw_ok" != 0 ]; then kill "$rw_pid" 2>/dev/null || true; fi
        wait "$rw_pid" 2>/dev/null || true
        if [ "$rw_ok" = 0 ]; then
            pass "real qemu-system-x86_64 + KVM accepts the supplied installer and virtio-win CDs, the usb-storage drivers seed, virtio-blk and virtio-net (query-block: cd0 cd1 drv0 hd0)"
        else
            fail "real qemu-system-x86_64 (Windows, supplied ISOs) lacks a drive in query-block"
        fi
    else
        kill "$rw_pid" 2>/dev/null || true
        wait "$rw_pid" 2>/dev/null || true
        fail "real qemu-system-x86_64 (Windows, supplied ISOs) did not open QMP socket: $(cat "$run_root/realwinauto.out" 2>/dev/null || true)"
    fi
fi

exit "$fails"
NS_EOF
fails=$((fails + ns_rc))

if [ "$fails" -eq 0 ]; then
    echo "ALL PASSED"
else
    echo "FAILED ($fails failures)"
    exit 1
fi
