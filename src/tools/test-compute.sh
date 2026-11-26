#!/bin/sh
# End-to-end tests for LOCachy headless compute node (locachy-compute + compute-client).
# Covers:
#   1. Image import (tarball + directory + OCI layer tar) & env create
#   2. Native daemonless container execution (unshare + OverlayFS + pivot_root + app-init)
#      with zero Wayland/compositor env leakage
#   3. Host data bind-mount (arbitrary path outside /home) + path-policy rejection
#   4. Instant upperdir wipe reset preserving read-only base image & mounted data
#   5. GPU device/lib probe + wildcard bind rejection
#   6. KVM micro-VM config, qcow2 CoW overlay creation, reset, and QEMU argv verification
#   7. mTLS HTTPS control plane: paired peer succeeds, unpaired peer rejected,
#      extdisplay-only peer rejected, revoked peer immediately rejected, remote job run+log+reset
#   8. compute-client service in locachy-net, listening only on the tailscale0 address
#   9. Storage volume binding (--volume) and compute_storage routing
#  10. Native detached job lifecycle after the submitting session is gone
# Runs inside unprivileged user/net/mount/pid namespaces; nothing installed.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: test-compute.sh requires Linux"
    exit 77
fi
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-compute-test.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi

fails=0
note() {
    echo "$1: $2"
    [ "$1" != FAIL ] || fails=$((fails + 1))
}

# Build app-init twice: static (what a foreign distro rootfs can run, exec'd as
# PID 1 by the container) and dynamic (host-libc build: the in-process reaper
# fallback must take over instead of exec'ing it inside a foreign rootfs).
cc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -static -o "$out/app-init" "$src/progs/app-init.c"
note PASS "build: static app-init compiles cleanly"
cc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -o "$out/app-init-dynamic" "$src/progs/app-init.c"
note PASS "build: dynamic app-init compiles cleanly"

if ! unshare --user --map-auto --map-root-user --net --mount true >/dev/null 2>&1; then
    echo "SKIP: test-compute.sh needs unprivileged user/net/mount namespaces"
    exit 77
fi

cat >"$out/driver.py" <<'PYEOF'
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tarfile
import time

OUT = Path(sys.argv[1])
SRC = Path(sys.argv[2])
PAIR = str(SRC / "link" / "locachy-pair")
COMPUTE = str(SRC / "link" / "locachy-compute")
APP_INIT = str(OUT / "app-init")

fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS" if ok else "FAIL") + ": " + name + ("" if ok or not detail else " -- " + detail), flush=True)
    if not ok:
        fails += 1


def make_minimal_rootfs(dest: Path):
    """Build a tiny self-contained Linux rootfs using host /bin/sh + coreutils + their shared libs."""
    for d in ("bin", "usr/bin", "lib", "lib64", "usr/lib", "etc", "tmp", "root", "proc", "sys", "dev", "run"):
        (dest / d).mkdir(parents=True, exist_ok=True)
    bins = []
    for cand in ("sh", "cat", "ls", "touch", "mkdir", "rm", "env", "id", "echo", "sleep", "uname", "readlink", "stat"):
        p = shutil.which(cand)
        if p:
            bins.append((cand, Path(p).resolve()))
    copied_libs = set()
    for name, real_bin in bins:
        dst = dest / "usr/bin" / name
        shutil.copy2(real_bin, dst)
        link = dest / "bin" / name
        if not link.exists():
            os.symlink(f"/usr/bin/{name}", link)
        ldd = subprocess.run(["ldd", str(real_bin)], capture_output=True, text=True)
        for line in ldd.stdout.splitlines():
            parts = line.strip().split()
            paths = []
            if "=>" in parts:
                idx = parts.index("=>")
                if parts[0].startswith("/"):
                    paths.append(parts[0])
                if idx + 1 < len(parts) and parts[idx + 1].startswith("/"):
                    paths.append(parts[idx + 1])
            elif parts and parts[0].startswith("/"):
                paths.append(parts[0])
            for lib_path in paths:
                if lib_path not in copied_libs and os.path.exists(lib_path):
                    copied_libs.add(lib_path)
                    rel = lib_path.lstrip("/")
                    target = dest / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(os.path.realpath(lib_path), target)
    (dest / "etc/base-marker").write_text("immutable-base-v1\n", encoding="utf-8")


env = dict(os.environ)
env["LOCACHY_PEERS_DIR"] = str(OUT / "peers")
env["LOCACHY_COMPUTE_BASE"] = str(OUT / "compute-base")
env["LOCACHY_COMPUTE_RUN"] = str(OUT / "compute-run")
env["LOCACHY_COMPUTE_APP_INIT"] = APP_INIT
env["LOCACHY_COMPUTE_ALLOW_NO_CGROUP"] = "1"
# Deliberately inject desktop env vars on caller to prove compute containers strip them.
env["WAYLAND_DISPLAY"] = "locachy-0"
env["DISPLAY"] = ":0"
env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/tmp/fake-bus"


def run_cli(tool, *args, check_rc=False):
    r = subprocess.run(["python3", tool, *args], env=env, capture_output=True, text=True, timeout=60)
    if check_rc and r.returncode != 0:
        raise RuntimeError(f"{tool} {args} failed ({r.returncode}): {r.stdout} {r.stderr}")
    return r


# 1. Build tarball rootfs and import image
rootfs_src = OUT / "rootfs-src"
make_minimal_rootfs(rootfs_src)
tar_path = OUT / "mini-rootfs.tar.gz"
with tarfile.open(tar_path, "w:gz") as tf:
    for item in rootfs_src.iterdir():
        tf.add(item, arcname=item.name)

r = run_cli(COMPUTE, "image", "import", "mini-ubuntu", str(tar_path))
check("image: tarball import succeeds", r.returncode == 0, r.stderr)
r = run_cli(COMPUTE, "image", "list", "--json")
check("image: list reports imported image", r.returncode == 0 and "mini-ubuntu" in r.stdout, r.stdout)

# 2. Data mount outside /home + security rejection of sensitive host paths
data_dir = OUT / "nvme-dataset"
data_dir.mkdir(parents=True)
(data_dir / "dataset.txt").write_text("train-batch-42\n", encoding="utf-8")

r = run_cli(COMPUTE, "env", "create", "bad-root", "--image", "mini-ubuntu", "--mount", "/:/hostroot:ro")
check("policy: mounting host '/' is refused", r.returncode != 0, r.stdout + r.stderr)

Path(env["LOCACHY_PEERS_DIR"]).mkdir(parents=True, exist_ok=True)
os.chmod(env["LOCACHY_PEERS_DIR"], 0o700)
r_peers = run_cli(COMPUTE, "env", "create", "bad-peers", "--image", "mini-ubuntu",
                  "--mount", f"{env['LOCACHY_PEERS_DIR']}:/stolen:ro")
check("policy: mounting peers directory is refused", r_peers.returncode != 0, r_peers.stdout + r_peers.stderr)
r_proc = run_cli(COMPUTE, "env", "create", "bad-proc", "--image", "mini-ubuntu", "--mount", "/proc:/hostproc:ro")
check("policy: mounting host /proc is refused", r_proc.returncode != 0, r_proc.stdout + r_proc.stderr)

r = run_cli(COMPUTE, "env", "create", "train0", "--image", "mini-ubuntu", "--net", "none",
            "--mount", f"{data_dir}:/data:rw", "--env", "EXP_NAME=run42")
check("env: create with external data mount succeeds", r.returncode == 0, r.stderr)

# 3. Native container execution: verify base file, data mount, env isolation, and OverlayFS write
r = run_cli(
    COMPUTE, "run", "train0", "--", "/bin/sh", "-ec",
    "cat /etc/base-marker; cat /data/dataset.txt; echo dirty-state > /tmp-dirty.txt; "
    "echo from-job > /data/result.txt; echo EXP=$EXP_NAME; echo WL=${WAYLAND_DISPLAY:-UNSET}; echo DISP=${DISPLAY:-UNSET}; echo DBUS=${DBUS_SESSION_BUS_ADDRESS:-UNSET}"
)
check("native-run: container executes via unshare+OverlayFS+pivot_root+app-init",
      r.returncode == 0 and "immutable-base-v1" in r.stdout and "train-batch-42" in r.stdout and "EXP=run42" in r.stdout,
      f"rc={r.returncode} out={r.stdout!r} err={r.stderr!r}")
check("native-run: desktop Wayland/X11/DBus env stripped",
      "WL=UNSET" in r.stdout and "DISP=UNSET" in r.stdout and "DBUS=UNSET" in r.stdout, r.stdout)

# The static app-init is PID 1 of the container (exec'd, reaping children).
r_pid1 = run_cli(COMPUTE, "run", "train0", "--", "/bin/sh", "-ec", "cat /proc/1/comm")
check("native-run: static app-init is PID 1 inside the container",
      r_pid1.returncode == 0 and r_pid1.stdout.strip().startswith("locachy-app-ini"), r_pid1.stdout + r_pid1.stderr)

# Foreign rootfs + dynamic (host-libc) app-init: never exec'd there; the in-process reaper runs as PID 1.
dyn_env = dict(env, LOCACHY_COMPUTE_APP_INIT=str(OUT / "app-init-dynamic"))
r_dyn = subprocess.run(["python3", COMPUTE, "run", "train0", "--", "/bin/sh", "-ec", "cat /proc/1/comm; echo fallback-ok"],
                       env=dyn_env, capture_output=True, text=True, timeout=60)
check("native-run: dynamic host-libc app-init is not exec'd in a foreign rootfs (fallback reaper)",
      r_dyn.returncode == 0 and "fallback-ok" in r_dyn.stdout and "locachy-app-ini" not in r_dyn.stdout,
      f"rc={r_dyn.returncode} out={r_dyn.stdout!r} err={r_dyn.stderr!r}")

# Verify upperdir persisted /tmp-dirty.txt across runs BEFORE reset
r2 = run_cli(COMPUTE, "run", "train0", "--", "/bin/sh", "-ec", "cat /tmp-dirty.txt")
check("overlay: writes persist in upperdir before reset", r2.returncode == 0 and "dirty-state" in r2.stdout, r2.stdout + r2.stderr)

# Verify base image was NOT mutated
check("overlay: base rootfs remains clean/immutable",
      not (Path(env["LOCACHY_COMPUTE_BASE"]) / "images/mini-ubuntu/rootfs/tmp-dirty.txt").exists())

# 4. Instant upperdir wipe reset
r = run_cli(COMPUTE, "env", "reset", "train0")
check("reset: env reset succeeds", r.returncode == 0, r.stderr)
r3 = run_cli(COMPUTE, "run", "train0", "--", "/bin/sh", "-ec",
             "test ! -e /tmp-dirty.txt && cat /etc/base-marker && cat /data/result.txt")
check("reset: upperdir wiped while base image and mounted dataset stay intact",
      r3.returncode == 0 and "immutable-base-v1" in r3.stdout and "from-job" in r3.stdout,
      f"rc={r3.returncode} out={r3.stdout!r} err={r3.stderr!r}")

# Detached job commands are released only after the submitting shell has exited.
# The mounted gate avoids timing assumptions about SSH/client startup or polling.
def wait_job(jid):
    deadline = time.monotonic() + 8
    rec = {}
    while time.monotonic() < deadline:
        result = run_cli(COMPUTE, "job", "status", jid, "--json")
        rec = json.loads(result.stdout) if result.returncode == 0 else {}
        if rec.get("state") in ("succeeded", "failed", "stopped", "exited"):
            break
        time.sleep(0.05)
    return rec


def session_submit(jid, command):
    return subprocess.run(
        ["/bin/sh", "-c", '"$@"', "submit", "python3", COMPUTE, "run", "train0",
         "--detach", "--job-id", jid, "--", "/bin/sh", "-c", command],
        env=env, start_new_session=True, capture_output=True, text=True, timeout=15,
    )


def check_job_cleanup(rec):
    jid = rec["job_id"]
    cg = rec.get("cgroup")
    if cg:
        check(f"detached: {jid} removed its cgroup before recording terminal status",
              not Path(cg).exists() and "cleanup_error" not in rec, repr(rec))
    else:
        print(f"SKIP: detached: {jid} cgroup cleanup needs writable cgroup v2 delegation", flush=True)


for jid, code in (("detached-ok", 0), ("detached-fail", 37)):
    gate = data_dir / jid
    r = session_submit(jid, f"while [ ! -e /data/{jid} ]; do sleep 0.05; done; "
                            f"echo {jid}-stdout; echo {jid}-stderr >&2; exit {code}")
    issued = json.loads(r.stdout) if r.returncode == 0 else {}
    gate.touch()
    rec = wait_job(jid) if issued else {}
    rlog = run_cli(COMPUTE, "job", "log", jid)
    expected_state = "succeeded" if code == 0 else "failed"
    check(f"detached: closed submitter preserves {expected_state}, exact exit {code}, stdout and stderr",
          issued.get("state") == "running" and rec.get("state") == expected_state
          and rec.get("exit_code") == code and bool(rec.get("ended_at"))
          and rlog.stdout.splitlines() == [f"{jid}-stdout", f"{jid}-stderr"],
          f"submit={r.stdout + r.stderr!r} status={rec!r} log={rlog.stdout!r}")
    if rec.get("state") == expected_state:
        check_job_cleanup(rec)

r = session_submit("detached-stop", "trap 'echo stopped-by-term >&2; exit 43' TERM; "
                                    "touch /data/stop-ready; while :; do sleep 0.05; done")
for _ in range(100):
    if (data_dir / "stop-ready").exists() or r.returncode != 0:
        break
    time.sleep(0.05)
stopped = run_cli(COMPUTE, "job", "stop", "detached-stop")
stop_rec = json.loads(stopped.stdout) if stopped.returncode == 0 else {}
rlog = run_cli(COMPUTE, "job", "log", "detached-stop")
check("detached: stop after submitter exit delivers TERM and records the trapped exit 43",
      r.returncode == 0 and stop_rec.get("state") == "stopped" and stop_rec.get("exit_code") == 43
      and "stopped-by-term" in rlog.stdout,
      f"submit={r.stdout + r.stderr!r} stop={stopped.stdout + stopped.stderr!r} log={rlog.stdout!r}")
if stop_rec.get("state") == "stopped":
    check_job_cleanup(stop_rec)

r = run_cli(COMPUTE, "run", "train0", "--detach", "--job-id", "missing-exec", "--", "/no/such/compute-command")
missing = wait_job("missing-exec")
check("detached: missing executable fails submission, with exact launcher exit 127 and its diagnostic",
      r.returncode != 0 and not r.stdout and "/no/such/compute-command" in r.stderr
      and missing.get("state") == "failed" and missing.get("exit_code") == 127,
      f"submit={r.stdout + r.stderr!r} status={missing!r}")
if missing.get("state") == "failed":
    check_job_cleanup(missing)
r = run_cli(COMPUTE, "run", "train0", "--job-id", "foreground-exit", "--", "/bin/sh", "-c",
            "echo foreground-out; echo foreground-err >&2; exit 29")
check("foreground: exact exit 29 and merged output are returned by the CLI",
      r.returncode == 29 and r.stdout.splitlines() == ["foreground-out", "foreground-err"],
      f"rc={r.returncode} stdout={r.stdout!r} stderr={r.stderr!r}")

with ThreadPoolExecutor(max_workers=2) as pool:
    calls = [pool.submit(session_submit, "one-id", f"echo winner-{i}; exit {10 + i}") for i in range(2)]
    launchers = [call.result() for call in calls]
winners = [i for i, launcher in enumerate(launchers) if launcher.returncode == 0]
losers = [launcher for launcher in launchers if launcher.returncode != 0]
rec = wait_job("one-id")
rlog = run_cli(COMPUTE, "job", "log", "one-id")
check("detached: concurrent same-ID submits reserve exactly one record and log",
      len(winners) == len(losers) == 1 and "job already exists" in losers[0].stderr
      and rec.get("exit_code") == 10 + winners[0] and rlog.stdout == f"winner-{winners[0]}\n",
      f"submit={launchers!r} status={rec!r} log={rlog.stdout!r}")

# A stale record points at a live unrelated child, but uses the old identity.
# Check both the launcher PID and supervisor PID paths without recycling host PIDs.
with subprocess.Popen(["sleep", "15"]) as sentinel:
    try:
        stale = dict(rec, state="running", pid=sentinel.pid)
        stale["supervisor"] = dict(stale["supervisor"], pid=sentinel.pid)
        stale["job_id"] = "stale"
        (Path(env["LOCACHY_COMPUTE_BASE"]) / "jobs/stale.json").write_text(json.dumps(stale))
        r = run_cli(COMPUTE, "job", "stop", "stale")
        check("detached: stale supervisor identity refuses stop without signaling the unrelated PID",
              r.returncode != 0 and "no verifiable supervisor" in r.stderr and sentinel.poll() is None,
              f"stop={r.stdout + r.stderr!r} sentinel={sentinel.poll()}")
    finally:
        sentinel.terminate()

# 5. GPU probe + wildcard bind rejection
r = run_cli(COMPUTE, "gpu-probe", "--json")
gpu_info = json.loads(r.stdout) if r.returncode == 0 else {}
check("gpu-probe: returns structured JSON", r.returncode == 0 and "devices" in gpu_info, r.stdout + r.stderr)
if gpu_info.get("devices"):
    rg_env = run_cli(COMPUTE, "env", "create", "gpu0", "--image", "mini-ubuntu", "--gpu", "--net", "none")
    check("gpu: env with --gpu is created", rg_env.returncode == 0, rg_env.stderr)
    probe_dev = gpu_info["devices"][0]
    probe_cmd = f"test -c {probe_dev} && echo gpu-dev-ok"
    if gpu_info.get("nvidia_libs"):
        probe_cmd += " && test -d /usr/local/lib/locachy-gpu && test -f /etc/ld.so.conf.d/locachy-gpu.conf && echo gpu-libs-ok"
    rg = run_cli(COMPUTE, "run", "gpu0", "--", "/bin/sh", "-ec", probe_cmd)
    want_libs = "gpu-libs-ok" in rg.stdout if gpu_info.get("nvidia_libs") else True
    check("gpu: host GPU devices (and NVIDIA user-space libs when present) are bound into the container",
          rg.returncode == 0 and "gpu-dev-ok" in rg.stdout and want_libs,
          f"dev={probe_dev} nlibs={len(gpu_info.get('nvidia_libs', []))} rc={rg.returncode} out={rg.stdout!r} err={rg.stderr!r}")
    run_cli(COMPUTE, "env", "remove", "gpu0", check_rc=True)
else:
    print("SKIP: gpu passthrough: no host GPU device nodes on this machine", flush=True)

r_wc = run_cli(COMPUTE, "serve", "--bind", "0.0.0.0", "--port", "27189")
check("serve: wildcard bind 0.0.0.0 is refused", r_wc.returncode != 0 and "wildcard" in r_wc.stderr, r_wc.stderr)

# 6. KVM micro-VM overlay + QEMU argv generation (virtiofs is the only share transport)
if shutil.which("qemu-img"):
    base_qcow = OUT / "base.qcow2"
    subprocess.run(["qemu-img", "create", "-f", "qcow2", str(base_qcow), "64M"], check=True, capture_output=True)
    r = run_cli(COMPUTE, "vm", "create", "expvm", "--disk", str(base_qcow), "--memory-mb", "1024", "--cpus", "2",
                "--share", f"{data_dir}:datasets", "--net", "user")
    check("vm: create qcow2 CoW overlay succeeds", r.returncode == 0, r.stderr)
    r_cmd = run_cli(COMPUTE, "vm", "cmd", "expvm", "--json")
    argv = json.loads(r_cmd.stdout) if r_cmd.returncode == 0 else []
    argv_s = " ".join(argv)
    check("vm: default share backend uses virtiofs (memfd share=on + vhost-user-fs-pci) with q35, virtio disk, no display",
          "-display" in argv and "none" in argv
          and "memory-backend-memfd,id=mem,size=1024M,share=on" in argv_s
          and "memory-backend=mem" in argv_s
          and "vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=datasets" in argv_s,
          str(argv))
    r_9p = run_cli(COMPUTE, "vm", "create", "expvm9p", "--disk", str(base_qcow), "--memory-mb", "512", "--cpus", "1",
                   "--share", f"{data_dir}:datasets:ro", "--share-backend", "9p", "--net", "none")
    check("vm: one share mechanism only: --share-backend (9p) is not accepted",
          r_9p.returncode != 0 and "--share-backend" in r_9p.stderr and not (OUT / "compute-base" / "vms" / "expvm9p").exists(),
          r_9p.stdout + r_9p.stderr)
    r_rst = run_cli(COMPUTE, "vm", "reset", "expvm")
    check("vm: reset recreates clean qcow2 overlay", r_rst.returncode == 0, r_rst.stderr)
    # vm start creates the VM's run directory 0700 whatever the umask; a stub QEMU that
    # exits at once leaves the directory behind to inspect. The cgroup root is the test's.
    r_sv = run_cli(COMPUTE, "vm", "create", "startvm", "--disk", str(base_qcow), "--memory-mb", "256", "--cpus", "1",
                   "--net", "none")
    stub_dir = OUT / "qemu-exit-stub"
    stub_dir.mkdir(exist_ok=True)
    (stub_dir / "qemu-system-x86_64").write_text("#!/bin/sh\nexit 3\n", encoding="utf-8")
    os.chmod(stub_dir / "qemu-system-x86_64", 0o755)
    old_umask = os.umask(0)
    try:
        r_st = subprocess.run(["python3", COMPUTE, "vm", "start", "startvm"], capture_output=True, text=True,
                              timeout=60, env=dict(env, PATH=f"{stub_dir}:{env.get('PATH', '')}",
                                                   LOCACHY_COMPUTE_CGROUP=str(OUT / "vm-cgroup")))
    finally:
        os.umask(old_umask)
    start_rdir = OUT / "compute-run" / "vm-startvm"
    start_mode = start_rdir.stat().st_mode & 0o777 if start_rdir.is_dir() else None
    check("vm: vm start creates the VM's run directory 0700",
          r_sv.returncode == 0 and r_st.returncode != 0 and "exited immediately" in r_st.stderr
          and start_mode == 0o700,
          f"create={r_sv.returncode} start={r_st.returncode} mode={start_mode} err={r_sv.stderr + r_st.stderr}")
else:
    print("SKIP: qemu-img not installed; skipping VM overlay test", flush=True)

# 7. mTLS HTTPS Control Plane: fail-closed -> paired OK -> wrong scope rejected -> unpaired rejected -> revoked rejected
# Start server BEFORE any peer is paired (should fail closed!)
ready_file = OUT / "serve-ready.json"
port = 27400 + (os.getpid() % 1000)
srv = subprocess.Popen(
    ["python3", COMPUTE, "serve", "--port", str(port), "--bind", "127.0.0.1", "--allow-loopback-test", "--ready-file", str(ready_file)],
    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
)
try:
    for _ in range(50):
        if ready_file.exists():
            break
        time.sleep(0.1)
    check("mtls: server binds and writes ready marker", ready_file.exists())

    # Create rogue client cert (NOT paired)
    rogue_dir = OUT / "rogue-creds"
    rogue_dir.mkdir()
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
        "-nodes", "-days", "1", "-subj", "/CN=rogue",
        "-keyout", str(rogue_dir / "client-key.pem"), "-out", str(rogue_dir / "client-cert.pem"),
    ], check=True, capture_output=True)
    shutil.copy2(Path(env["LOCACHY_PEERS_DIR"]) / "host-cert.pem", rogue_dir / "host-cert.pem")
    host_meta = json.loads((Path(env["LOCACHY_PEERS_DIR"]) / "host.json").read_text())
    (rogue_dir / "peer.json").write_text(json.dumps({"host_cert_sha256": host_meta["host_cert_sha256"]}))

    # With 0 paired peers, rogue MUST fail closed
    r = run_cli(COMPUTE, "remote", "--creds-dir", str(rogue_dir), "--host", "127.0.0.1", "--port", str(port), "health")
    check("mtls: fails closed when 0 peers paired", r.returncode != 0, r.stdout + r.stderr)

    # Pair an extdisplay-only peer (has cert, wrong scope) -> MUST be rejected for compute
    ext_only_dir = OUT / "ext-only-creds"
    r = run_cli(PAIR, "add", "--yes", "--name", "DisplayOnly", "--scope", "extdisplay", "--cert-file", str(rogue_dir / "client-cert.pem"), "--out-dir", str(ext_only_dir))
    shutil.copy2(rogue_dir / "client-key.pem", ext_only_dir / "client-key.pem")
    r_ext = run_cli(COMPUTE, "remote", "--creds-dir", str(ext_only_dir), "--host", "127.0.0.1", "--port", str(port), "health")
    check("mtls: extdisplay-only peer is refused by compute API", r_ext.returncode != 0, r_ext.stdout + r_ext.stderr)

    # Pair a real compute peer
    mac_dir = OUT / "mac-creds"
    r = run_cli(PAIR, "add", "--yes", "--name", "WorkMac", "--scope", "compute", "--out-dir", str(mac_dir), check_rc=True)

    # Paired peer succeeds!
    r_h = run_cli(COMPUTE, "remote", "--creds-dir", str(mac_dir), "--host", "127.0.0.1", "--port", str(port), "health")
    hdata = json.loads(r_h.stdout) if r_h.returncode == 0 else {}
    check("mtls: paired compute peer authenticates and reads /v1/health",
          r_h.returncode == 0 and hdata.get("peer_name") == "WorkMac" and hdata.get("envs") == 1,
          r_h.stdout + r_h.stderr)

    # Unpaired rogue STILL rejected even now that a valid peer exists
    r_rogue = run_cli(COMPUTE, "remote", "--creds-dir", str(rogue_dir), "--host", "127.0.0.1", "--port", str(port), "health")
    check("mtls: unpaired client cert is rejected", r_rogue.returncode != 0, r_rogue.stdout + r_rogue.stderr)

    # Remote job launch, poll completion, read log, remote reset
    r_job = run_cli(COMPUTE, "remote", "--creds-dir", str(mac_dir), "--host", "127.0.0.1", "--port", str(port),
                    "run", "train0", "--", "/bin/sh", "-ec", "echo remote-compute-ok && echo m > /remote-dirty")
    jdata = json.loads(r_job.stdout) if r_job.returncode == 0 else {}
    jid = jdata.get("job_id", "")
    check("mtls: remote POST /v1/jobs launches container job", r_job.returncode == 0 and bool(jid), r_job.stdout + r_job.stderr)

    if jid:
        state = ""
        for _ in range(50):
            rs = run_cli(COMPUTE, "remote", "--creds-dir", str(mac_dir), "--host", "127.0.0.1", "--port", str(port), "job-status", jid)
            if rs.returncode == 0:
                state = json.loads(rs.stdout).get("state", "")
                if state in ("succeeded", "failed", "stopped"):
                    break
            time.sleep(0.1)
        rl = run_cli(COMPUTE, "remote", "--creds-dir", str(mac_dir), "--host", "127.0.0.1", "--port", str(port), "job-log", jid)
        check("mtls: remote job completes and log is readable",
              state == "succeeded" and "remote-compute-ok" in rl.stdout,
              f"state={state} log={rl.stdout!r}")
        rr = run_cli(COMPUTE, "remote", "--creds-dir", str(mac_dir), "--host", "127.0.0.1", "--port", str(port), "reset", "train0")
        check("mtls: remote POST /v1/envs/<name>/reset succeeds", rr.returncode == 0, rr.stdout + rr.stderr)

    # Revoke peer -> subsequent mTLS request MUST fail immediately (without restarting server!)
    run_cli(PAIR, "revoke", "WorkMac", check_rc=True)
    r_after = run_cli(COMPUTE, "remote", "--creds-dir", str(mac_dir), "--host", "127.0.0.1", "--port", str(port), "health")
    check("mtls: revoked peer is rejected immediately without daemon restart", r_after.returncode != 0, r_after.stdout + r_after.stderr)
finally:
    srv.terminate()
    try:
        srv.wait(timeout=5)
    except subprocess.TimeoutExpired:
        srv.kill()

# 8. compute-client (the service launcher): nsenter --net enters locachy-net and keeps
# the mount namespace (so the host /sys/fs/cgroup is unchanged); it serves on tailscale0.
mnt_here = os.readlink("/proc/self/ns/mnt")
nsenter_mnt = subprocess.run(["nsenter", "--net=/run/netns/locachy-net", "readlink", "/proc/self/ns/mnt"],
                             capture_output=True, text=True, timeout=20)
check("compute-client: nsenter --net keeps the mount namespace (host /sys/fs/cgroup preserved)",
      nsenter_mnt.returncode == 0 and nsenter_mnt.stdout.strip() == mnt_here, nsenter_mnt.stdout + nsenter_mnt.stderr)
nsenter_net = subprocess.run(["nsenter", "--net=/run/netns/locachy-net", "readlink", "/proc/self/ns/net"],
                             capture_output=True, text=True, timeout=20)
check("compute-client: /run/netns/locachy-net is the namespace the service enters",
      nsenter_net.returncode == 0 and nsenter_net.stdout.strip() == os.readlink("/proc/self/ns/net"), nsenter_net.stdout)

r_cc = run_cli(PAIR, "add", "--yes", "--name", "ClusterMac", "--scope", "compute", "--out-dir", str(OUT / "cluster-creds"), check_rc=True)
cc_env = dict(env)
cc_env.update({
    "LOCACHY_COMPUTE_NETNS": "locachy-net",
    "LOCACHY_COMPUTE_BIN": COMPUTE,
    "LOCACHY_COMPUTE_BASE": str(OUT / "cc-base"),
    "LOCACHY_COMPUTE_RUN": str(OUT / "cc-run"),
    "LOCACHY_COMPUTE_RC": str(OUT / "no-rc.conf"),
})
cc = subprocess.Popen(["bash", str(SRC / "link" / "compute-client")], env=cc_env,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    health = None
    for _ in range(100):
        if cc.poll() is not None:
            break
        rh = run_cli(COMPUTE, "remote", "--creds-dir", str(OUT / "cluster-creds"), "--host", "100.64.0.1",
                     "--port", "27184", "health")
        if rh.returncode == 0:
            health = json.loads(rh.stdout)
            break
        time.sleep(0.1)
    check("compute-client: serves mTLS health on tailscale0 inside locachy-net",
          health is not None and health.get("peer_name") == "ClusterMac",
          f"exit={cc.poll()} err={cc.stderr.read() if cc.poll() is not None else ''}")
    check("compute-client: wildcard is never bound (only the tailscale0 address answers)",
          run_cli(COMPUTE, "remote", "--creds-dir", str(OUT / "cluster-creds"), "--host", "127.0.0.1",
                  "--port", "27184", "health").returncode != 0)
    cc_run_mode = (OUT / "cc-run").stat().st_mode & 0o777 if (OUT / "cc-run").is_dir() else None
    check("compute-client: the run directory is 0711 (a VM app's uid reaches its own vm-NAME)",
          cc_run_mode == 0o711, f"mode={oct(cc_run_mode) if cc_run_mode is not None else 'missing'}")
finally:
    cc.terminate()
    try:
        cc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        cc.kill()

# 9. Storage volume binding (--volume) and compute_storage routing (fail-closed)
storage_root = OUT / "srv-storage"
volumes_file = OUT / "volumes.json"
rc_storage = OUT / "storage-rc.conf"
for vname in ("vol-shared", "vol-compute", "vol-data"):
    mp = storage_root / vname
    mp.mkdir(parents=True, exist_ok=True)
    subprocess.run(["mount", "-t", "tmpfs", "tmpfs", str(mp)], check=True)
    (mp / "marker.txt").write_text(f"from-{vname}\n", encoding="utf-8")

volumes_file.write_text(
    json.dumps(
        {
            "schema": 1,
            "volumes": [
                {
                    "name": "vol-shared",
                    "topology": "single",
                    "fs": "ext4",
                    "raid": "",
                    "role": "shared",
                    "encrypt": "none",
                    "auto_mount": True,
                    "fs_uuid": "11111111-1111-1111-1111-111111111111",
                    "members": [{"part_uuid": "aaaa1111-1111-1111-1111-111111111111", "luks_uuid": "", "mapper": "", "keyfile": ""}],
                    "mount_options": "",
                    "created_at": "2026-11-24T00:00:00Z",
                },
                {
                    "name": "vol-compute",
                    "topology": "single",
                    "fs": "btrfs",
                    "raid": "",
                    "role": "compute",
                    "encrypt": "none",
                    "auto_mount": True,
                    "fs_uuid": "22222222-2222-2222-2222-222222222222",
                    "members": [{"part_uuid": "aaaa2222-2222-2222-2222-222222222222", "luks_uuid": "", "mapper": "", "keyfile": ""}],
                    "mount_options": "",
                    "created_at": "2026-11-24T00:00:00Z",
                },
                {
                    "name": "vol-data",
                    "topology": "single",
                    "fs": "ext4",
                    "raid": "",
                    "role": "data",
                    "encrypt": "none",
                    "auto_mount": True,
                    "fs_uuid": "33333333-3333-3333-3333-333333333333",
                    "members": [{"part_uuid": "aaaa3333-3333-3333-3333-333333333333", "luks_uuid": "", "mapper": "", "keyfile": ""}],
                    "mount_options": "",
                    "created_at": "2026-11-24T00:00:00Z",
                },
            ],
        },
        indent=2,
    ),
    encoding="utf-8",
)

vol_env = dict(env)
vol_env["LOCACHY_VOLUMES_FILE"] = str(volumes_file)
vol_env["LOCACHY_STORAGE_ROOT"] = str(storage_root)

def run_vol_cli(*args, extra_env=None):
    e = dict(vol_env)
    if extra_env:
        e.update(extra_env)
    return subprocess.run(["python3", COMPUTE, *args], env=e, capture_output=True, text=True, timeout=60)

# --volume rejects data-only volume at env create AND at run time AND via raw --mount
r_data_env = run_vol_cli("env", "create", "bad-data-vol", "--image", "mini-ubuntu", "--net", "none", "--volume", "vol-data")
check("storage: --volume refuses data-only volume at env create",
      r_data_env.returncode != 0 and "data" in r_data_env.stderr, r_data_env.stdout + r_data_env.stderr)

r_data_run = run_vol_cli("run", "train0", "--volume", "vol-data:/v:ro", "--", "/bin/sh", "-c", "true")
check("storage: --volume refuses data-only volume at run time",
      r_data_run.returncode != 0 and "data" in r_data_run.stderr, r_data_run.stdout + r_data_run.stderr)

r_data_mnt = run_vol_cli("env", "create", "bad-data-mnt", "--image", "mini-ubuntu", "--net", "none",
                         "--mount", f"{storage_root / 'vol-data'}:/stolen:ro")
check("storage: raw --mount into data-only /srv/storage volume is refused",
      r_data_mnt.returncode != 0 and "data-only" in r_data_mnt.stderr, r_data_mnt.stdout + r_data_mnt.stderr)

# --volume succeeds for shared (default target /volumes/<name>) and compute (custom target + ro)
r_sv = run_vol_cli("env", "create", "volenv", "--image", "mini-ubuntu", "--net", "none",
                   "--volume", "vol-shared", "--volume", "vol-compute:/scratch:ro")
check("storage: env create with shared + compute --volume succeeds", r_sv.returncode == 0, r_sv.stderr)

r_sv_run = run_vol_cli(
    "run", "volenv", "--", "/bin/sh", "-ec",
    "cat /volumes/vol-shared/marker.txt; cat /scratch/marker.txt; echo wrote-shared > /volumes/vol-shared/out.txt; "
    "if echo nope > /scratch/ro-test 2>/dev/null; then echo RO_BROKEN; else echo RO_ENFORCED; fi"
)
check("storage: shared & compute volumes bind cleanly and ro mode is enforced",
      r_sv_run.returncode == 0
      and "from-vol-shared" in r_sv_run.stdout
      and "from-vol-compute" in r_sv_run.stdout
      and "RO_ENFORCED" in r_sv_run.stdout
      and (storage_root / "vol-shared" / "out.txt").read_text(encoding="utf-8").strip() == "wrote-shared",
      f"rc={r_sv_run.returncode} out={r_sv_run.stdout!r} err={r_sv_run.stderr!r}")

# compute_storage routes compute_base() onto the mounted volume and FAILS CLOSED when unmounted
rc_storage.write_text('compute_storage="vol-compute"\n', encoding="utf-8")
cs_env = dict(vol_env)
cs_env.pop("LOCACHY_COMPUTE_BASE", None)
cs_env["LOCACHY_COMPUTE_RC"] = str(rc_storage)

r_cs_info = subprocess.run(["python3", COMPUTE, "storage", "--json"], env=cs_env, capture_output=True, text=True, timeout=30)
cs_data = json.loads(r_cs_info.stdout) if r_cs_info.returncode == 0 else {}
expected_cs_base = str(storage_root / "vol-compute" / "locachy-compute")
check("storage: compute_storage routes compute_base() to mounted compute volume",
      r_cs_info.returncode == 0
      and cs_data.get("storage_volume") == "vol-compute"
      and cs_data.get("compute_base") == expected_cs_base
      and (storage_root / "vol-compute" / "locachy-compute" / "images").is_dir(),
      r_cs_info.stdout + r_cs_info.stderr)

r_cs_img = subprocess.run(["python3", COMPUTE, "image", "import", "vol-img", str(tar_path)],
                          env=cs_env, capture_output=True, text=True, timeout=60)
check("storage: image import writes into compute_storage volume base",
      r_cs_img.returncode == 0 and (storage_root / "vol-compute" / "locachy-compute" / "images" / "vol-img" / "rootfs").is_dir(),
      r_cs_img.stdout + r_cs_img.stderr)

# Unmount vol-compute -> compute_base() MUST FAIL CLOSED (never fall back to root!)
subprocess.run(["umount", str(storage_root / "vol-compute")], check=True)
r_cs_unmounted = subprocess.run(["python3", COMPUTE, "image", "list", "--json"],
                                env=cs_env, capture_output=True, text=True, timeout=30)
check("storage: compute_storage fails closed when configured volume is unmounted",
      r_cs_unmounted.returncode != 0 and "not mounted" in r_cs_unmounted.stderr,
      r_cs_unmounted.stdout + r_cs_unmounted.stderr)

# Also verify health endpoint exposes storage_volume & compute_base
r_sv_clean = run_vol_cli("env", "remove", "volenv")
check("storage: env with volume bindings removes cleanly", r_sv_clean.returncode == 0, r_sv_clean.stderr)

# 10. VM display/QMP argv, virtiofs --volume shares, export-app, foreground vm run
import importlib.machinery as _ilm
import importlib.util as _ilu
_mloader = _ilm.SourceFileLoader("locachy_compute_mod", COMPUTE)
_mspec = _ilu.spec_from_file_location("locachy_compute_mod", COMPUTE, loader=_mloader)
lc = _ilu.module_from_spec(_mspec)
_mspec.loader.exec_module(lc)


def after_flag(argv, flag):
    return argv[argv.index(flag) + 1]


gfx_meta = {"name": "gfx", "memory_mb": 512, "cpus": 1, "overlay_disk": str(OUT / "gfx.qcow2"),
            "shares": [], "volumes": [], "cloud_init": "", "net": "none"}
gfx = lc.vm_build_argv(gfx_meta, kvm=False, display="gtk", qmp_sock="/tmp/locachy-qmp.sock")
check("vm-argv: display gtk adds virtio-vga, qemu-xhci, usb-tablet and the gtk backend with window-close=off",
      after_flag(gfx, "-display") == "gtk,window-close=off" and "virtio-vga" in gfx and "qemu-xhci" in gfx
      and "usb-tablet" in gfx,
      str(gfx))
sdl = lc.vm_build_argv(gfx_meta, kvm=False, display="sdl")
check("vm-argv: display sdl is the sdl backend with window-close=off",
      after_flag(sdl, "-display") == "sdl,window-close=off" and "virtio-vga" in sdl, str(sdl))
check("vm-argv: -name is the window tag locachy-compute[NAME]",
      after_flag(gfx, "-name") == "locachy-compute[gfx]" and after_flag(sdl, "-name") == "locachy-compute[gfx]",
      str(gfx))
check("vm-argv: --qmp-sock becomes unix:PATH,server=on,wait=off",
      after_flag(gfx, "-qmp") == "unix:/tmp/locachy-qmp.sock,server=on,wait=off", str(gfx))
headless = lc.vm_build_argv(gfx_meta, kvm=False)
check("vm-argv: headless keeps -display none, no GPU device, no memfd when no shares, default qmp.sock",
      after_flag(headless, "-display") == "none" and "virtio-vga" not in headless
      and "memory-backend-memfd" not in " ".join(headless)
      and after_flag(headless, "-qmp") == "unix:" + str(lc.compute_run() / "vm-gfx" / "qmp.sock") + ",server=on,wait=off",
      str(headless))
for bad in ("vnc", "gtk,gl=on"):
    try:
        lc.vm_build_argv(gfx_meta, kvm=False, display=bad)
        check(f"vm-argv: refuses display {bad!r}", False, "accepted")
    except lc.ComputeError:
        check(f"vm-argv: refuses display {bad!r}", True)
disk_argv = lc.vm_build_argv(gfx_meta, kvm=False, display="none")
check("vm-disk: base overlay on virtio-blk-pci with an iothread, aio=io_uring, cache=none, discard=unmap; no NVMe",
      "-object" in disk_argv and "iothread,id=iothread0" in disk_argv
      and f"file={gfx_meta['overlay_disk']},format=qcow2,if=none,id=hd0,cache=none,aio=io_uring,discard=unmap" in disk_argv
      and "virtio-blk-pci,drive=hd0,iothread=iothread0" in disk_argv
      and not any("nvme" in a.lower() for a in disk_argv),
      str(disk_argv))
try:
    lc.vm_build_argv(gfx_meta, kvm=False, qmp_sock="/tmp/a,b.sock")
    check("vm-argv: refuses a QMP socket path containing a comma", False, "accepted")
except lc.ComputeError:
    check("vm-argv: refuses a QMP socket path containing a comma", True)

# virtiofsd argv: rw vs ro (--readonly), --sandbox=namespace, --cache=auto
vfs_rw = lc.virtiofsd_build_argv({"host": str(data_dir), "tag": "ds", "mode": "rw"}, Path("/tmp/v0.sock"), binary="/usr/lib/virtiofsd")
vfs_ro = lc.virtiofsd_build_argv({"host": str(data_dir), "tag": "ds", "mode": "ro"}, Path("/tmp/v1.sock"), binary="/usr/lib/virtiofsd")
check("vm-virtiofs: virtiofsd_build_argv sets socket-path, shared-dir, sandbox=namespace, cache=auto, readonly only on ro",
      "--socket-path=/tmp/v0.sock" in vfs_rw
      and f"--shared-dir={data_dir}" in vfs_rw
      and "--sandbox=namespace" in vfs_rw
      and "--cache=auto" in vfs_rw
      and "--readonly" not in vfs_rw
      and "--readonly" in vfs_ro,
      f"rw={vfs_rw} ro={vfs_ro}")

# Storage volumes: remount vol-compute (section 9 unmounted it), then vm create with --volume
subprocess.run(["mount", "-t", "tmpfs", "tmpfs", str(storage_root / "vol-compute")], check=True)
(storage_root / "vol-compute" / "marker.txt").write_text("from-vol-compute\n", encoding="utf-8")

if shutil.which("qemu-img"):
    vmbase = OUT / "vmbase.qcow2"
    subprocess.run(["qemu-img", "create", "-f", "qcow2", str(vmbase), "64M"], check=True, capture_output=True)
    r_vv_data = run_vol_cli("vm", "create", "volvm-bad", "--disk", str(vmbase), "--net", "none", "--volume", "vol-data")
    check("vm-volume: vm create refuses a data-only volume",
          r_vv_data.returncode != 0 and "data" in r_vv_data.stderr, r_vv_data.stdout + r_vv_data.stderr)
    r_vv = run_vol_cli("vm", "create", "volvm", "--disk", str(vmbase), "--memory-mb", "512", "--cpus", "1",
                       "--net", "none", "--volume", "vol-shared", "--volume", "vol-compute:cstore:ro")
    check("vm-volume: vm create accepts shared and compute volumes as virtiofs shares",
          r_vv.returncode == 0, r_vv.stdout + r_vv.stderr)
    r_vc = run_vol_cli("vm", "cmd", "volvm", "--display", "gtk", "--qmp-sock", str(OUT / "volvm-qmp.sock"), "--json")
    vargv = json.loads(r_vc.stdout) if r_vc.returncode == 0 else []
    vargv_s = " ".join(vargv)
    check("vm-volume: vm cmd binds vol-shared and vol-compute (tag cstore) over virtiofs with memfd share=on",
          "memory-backend-memfd,id=mem,size=512M,share=on" in vargv_s
          and "memory-backend=mem" in vargv_s
          and "vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=vol-shared" in vargv_s
          and "vhost-user-fs-pci,queue-size=1024,chardev=vfs1,tag=cstore" in vargv_s
          and "virtio-vga" in vargv
          and after_flag(vargv, "-display") == "gtk,window-close=off",
          r_vc.stdout + r_vc.stderr)

    # Missing virtiofsd fails clearly before launching QEMU
    r_novfs = run_vol_cli("vm", "run", "volvm", "--display", "none",
                          extra_env={"LOCACHY_VIRTIOFSD": "/nonexistent/virtiofsd", "LOCACHY_COMPUTE_KVM": "0"})
    check("vm-virtiofs: missing virtiofsd fails cleanly before launching QEMU",
          r_novfs.returncode != 0 and "LOCACHY_VIRTIOFSD" in r_novfs.stderr, r_novfs.stdout + r_novfs.stderr)

    r_ex = run_vol_cli("vm", "export-app", "volvm", "--app-name", "volvm-app", "--display", "gtk",
                       "--freeze", "qmp", "--network", "1",
                       extra_env={"LOCACHY_CONTAINERS_DIR": str(OUT / "containers")})
    conf_path = OUT / "containers" / "volvm-app.conf"
    conf_text = conf_path.read_text(encoding="utf-8") if conf_path.is_file() else ""
    check("vm-app: export-app writes APP.conf (0644) with vm, vm_kind, vm_name, vm_display, vm_freeze, network",
          r_ex.returncode == 0
          and all(s in conf_text for s in ['vm="1"', 'vm_kind="compute"', 'vm_name="volvm"',
                                           'vm_display="gtk"', 'vm_freeze="qmp"', 'network="1"'])
          and (conf_path.stat().st_mode & 0o777) == 0o644,
          r_ex.stdout + r_ex.stderr + conf_text)
    r_ex_none = run_vol_cli("vm", "export-app", "volvm", "--display", "none",
                            extra_env={"LOCACHY_CONTAINERS_DIR": str(OUT / "containers")})
    check("vm-app: export-app refuses a windowless display (an app needs a window)",
          r_ex_none.returncode != 0, r_ex_none.stdout + r_ex_none.stderr)
    r_ex_bad = run_vol_cli("vm", "export-app", "no-such-vm", "--app-name", "ghost",
                           extra_env={"LOCACHY_CONTAINERS_DIR": str(OUT / "containers")})
    check("vm-app: export-app refuses an unknown VM", r_ex_bad.returncode != 0, r_ex_bad.stdout + r_ex_bad.stderr)

    # Real QEMU run through vm run (a VM without shares needs no virtiofsd): QMP handshake + query-status + quit,
    # then SIGTERM forwarding
    r_qvm = run_vol_cli("vm", "create", "qmpvm", "--disk", str(vmbase), "--memory-mb", "256", "--cpus", "1", "--net", "none")
    check("vm-run: create VM for live QEMU QMP test succeeds", r_qvm.returncode == 0, r_qvm.stderr)
    if shutil.which("qemu-system-x86_64"):
        qmp_sock = OUT / "real-qmp.sock"
        p = subprocess.Popen(
            ["python3", COMPUTE, "vm", "run", "qmpvm", "--display", "none", "--qmp-sock", str(qmp_sock)],
            env=dict(vol_env, LOCACHY_COMPUTE_KVM="0"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            for _ in range(100):
                if qmp_sock.exists() or p.poll() is not None:
                    break
                time.sleep(0.05)
            qmp_ok = False
            if qmp_sock.exists():
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as qs:
                    qs.settimeout(5.0)
                    qs.connect(str(qmp_sock))
                    f = qs.makefile("rwb", buffering=0)
                    greet = json.loads(f.readline().decode("utf-8"))
                    f.write(b'{"execute":"qmp_capabilities"}\n')
                    cap = json.loads(f.readline().decode("utf-8"))
                    f.write(b'{"execute":"query-status"}\n')
                    st = json.loads(f.readline().decode("utf-8"))
                    f.write(b'{"execute":"quit"}\n')
                    qmp_ok = "QMP" in greet and "return" in cap and "return" in st
            rc = p.wait(timeout=10)
            check("vm-run: foreground QEMU answers QMP and exits cleanly on quit",
                  qmp_ok and rc == 0, f"qmp_ok={qmp_ok} rc={rc} err={p.stderr.read()}")
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
        # SIGTERM to vm run forwards to QEMU
        qmp_sock2 = OUT / "real-qmp2.sock"
        p2 = subprocess.Popen(
            ["python3", COMPUTE, "vm", "run", "qmpvm", "--display", "none", "--qmp-sock", str(qmp_sock2)],
            env=dict(vol_env, LOCACHY_COMPUTE_KVM="0"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            # Wait until QEMU's main loop answers QMP (not just socket bind), so its
            # SIGTERM handler is installed before we signal vm run.
            for _ in range(200):
                if p2.poll() is not None:
                    break
                if qmp_sock2.exists():
                    try:
                        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        s.settimeout(2.0)
                        s.connect(str(qmp_sock2))
                        f = s.makefile("rwb", buffering=0)
                        f.readline()
                        f.write(b'{"execute":"qmp_capabilities"}\r\n')
                        f.readline()
                        f.write(b'{"execute":"query-status"}\r\n')
                        f.readline()
                        s.close()
                        break
                    except OSError:
                        pass
                time.sleep(0.05)
            p2.terminate()
            rc2 = p2.wait(timeout=10)
            check("vm-run: SIGTERM forwards to QEMU and exits cleanly", rc2 == 0, f"rc={rc2} err={p2.stderr.read()}")
        finally:
            if p2.poll() is None:
                p2.kill()
                p2.wait()
    else:
        print("SKIP: qemu-system-x86_64 not installed; skipping real QEMU vm run test", flush=True)

    # 10b. virtiofs lifecycle: stub $LOCACHY_VIRTIOFSD + QEMU shim. The sockets must exist before QEMU
    # starts, --readonly only for ro shares, and SIGTERM to vm run stops QEMU and then virtiofsd.
    vfs_dir = OUT / "vfs-life"
    shim_dir = vfs_dir / "shim"
    shim_dir.mkdir(parents=True, exist_ok=True)
    stub_vfsd = vfs_dir / "stub-virtiofsd.py"
    stub_vfsd.write_text(
        "#!/usr/bin/env python3\n"
        "import os, signal, socket, sys, time\n"
        "from pathlib import Path\n"
        "args = sys.argv[1:]\n"
        "here = Path(os.path.dirname(os.path.abspath(__file__)))\n"
        "sock_path = next(a.split('=', 1)[1] for a in args if a.startswith('--socket-path='))\n"
        "with open(here / 'vfsd-args.log', 'a', encoding='utf-8') as fh:\n"
        "    fh.write(' '.join(args) + '\\n')\n"
        "with open(here / f'vfsd-{os.getpid()}.pid', 'w', encoding='utf-8') as fh:\n"
        "    fh.write(str(os.getpid()) + '\\n')\n"
        "srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
        "srv.bind(sock_path)\n"
        "srv.listen(4)\n"
        "def _term(signum, frame):\n"
        "    with open(here / 'vfsd-term.txt', 'a', encoding='utf-8') as fh:\n"
        "        fh.write('term\\n')\n"
        "    try:\n"
        "        os.unlink(sock_path)\n"
        "    except OSError:\n"
        "        pass\n"
        "    os._exit(0)\n"
        "signal.signal(signal.SIGTERM, _term)\n"
        "while True:\n"
        "    time.sleep(0.05)\n",
        encoding="utf-8",
    )
    os.chmod(stub_vfsd, 0o755)
    qemu_shim = shim_dir / "qemu-system-x86_64"
    qemu_shim.write_text(
        "#!/bin/sh\n"
        "d=$(dirname \"$0\")\n"
        "printf '%s\\n' \"$@\" >\"$d/argv.txt\"\n"
        "present=yes\n"
        "for p in $(sed -n 's/^socket,id=[^,]*,path=//p' \"$d/argv.txt\"); do\n"
        "    [ -S \"$p\" ] || present=no\n"
        "done\n"
        "printf '%s' \"$present\" >\"$d/vfs-present.txt\"\n"
        "trap 'printf term >\"$d/term.txt\"; exit 0' TERM INT\n"
        ": >\"$d/ready\"\n"
        "while :; do sleep 0.05; done\n",
        encoding="utf-8",
    )
    os.chmod(qemu_shim, 0o755)
    r_vfs_vm = run_vol_cli("vm", "create", "vfsvm", "--disk", str(vmbase), "--memory-mb", "256", "--cpus", "1",
                           "--net", "none", "--share", f"{data_dir}:datasets:ro", "--volume", "vol-shared")
    check("vm-virtiofs: create VM with a ro directory share and a rw volume share (virtiofs default)",
          r_vfs_vm.returncode == 0, r_vfs_vm.stdout + r_vfs_vm.stderr)
    life_env = dict(vol_env, LOCACHY_COMPUTE_KVM="0", LOCACHY_VIRTIOFSD=str(stub_vfsd),
                    PATH=f"{shim_dir}:{os.environ.get('PATH', '')}")
    for stale in shim_dir.glob("*"):
        if stale.is_file() and stale.name != "qemu-system-x86_64":
            stale.unlink()
    p3 = subprocess.Popen(["python3", COMPUTE, "vm", "run", "vfsvm", "--display", "none"],
                          env=life_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        for _ in range(200):
            if (shim_dir / "ready").exists() or p3.poll() is not None:
                break
            time.sleep(0.05)
        ready3 = (shim_dir / "ready").exists()
        p3.terminate()
        rc3 = p3.wait(timeout=20)
    finally:
        if p3.poll() is None:
            p3.kill()
            p3.wait()
    err3 = p3.stderr.read()
    vfsd_log = (vfs_dir / "vfsd-args.log").read_text(encoding="utf-8") if (vfs_dir / "vfsd-args.log").is_file() else ""
    vfs_present = (shim_dir / "vfs-present.txt").read_text(encoding="utf-8") if (shim_dir / "vfs-present.txt").is_file() else ""
    term_count = (vfs_dir / "vfsd-term.txt").read_text(encoding="utf-8").count("term") if (vfs_dir / "vfsd-term.txt").is_file() else 0
    stub_pids = [int(p.read_text().strip()) for p in vfs_dir.glob("vfsd-*.pid")]
    dead = True
    for pid in stub_pids:
        try:
            os.kill(pid, 0)
            dead = False
        except OSError:
            pass
    left_socks = [s.name for s in (OUT / "compute-run" / "vm-vfsvm").glob("vfs-*.sock")]
    check("vm-virtiofs: one virtiofsd per share starts before QEMU, sockets exist when QEMU starts",
          ready3 and vfs_present == "yes" and len(stub_pids) == 2,
          f"ready={ready3} present={vfs_present!r} pids={stub_pids} err={err3}")
    check("vm-virtiofs: virtiofsd argv has --socket-path, --shared-dir, --sandbox=namespace, --cache=auto, and --readonly only for ro",
          vfsd_log.count("--sandbox=namespace") == 2 and vfsd_log.count("--cache=auto") == 2
          and any(f"--shared-dir={data_dir}" in ln and "--readonly" in ln for ln in vfsd_log.splitlines())
          and any(f"--shared-dir={os.path.realpath(str(storage_root / 'vol-shared'))}" in ln and "--readonly" not in ln
                  for ln in vfsd_log.splitlines()),
          vfsd_log)
    check("vm-virtiofs: SIGTERM to vm run stops QEMU, then every virtiofsd; no sockets or pids left behind",
          rc3 == 0 and term_count == 2 and dead and left_socks == [],
          f"rc={rc3} terms={term_count} dead={dead} socks={left_socks} err={err3}")

    # Real QEMU parse check: memfd + vhost-user-fs-pci with -S -display none, stub listeners at the chardev paths
    if shutil.which("qemu-system-x86_64"):
        import threading
        (OUT / "compute-run" / "vm-vfsvm").mkdir(parents=True, exist_ok=True)
        cmd_res = run_vol_cli("vm", "cmd", "vfsvm", "--json", extra_env={"LOCACHY_COMPUTE_KVM": "0"})
        real_argv = json.loads(cmd_res.stdout) if cmd_res.returncode == 0 else []
        paths = [a.split(",path=", 1)[1] for a in real_argv if a.startswith("socket,id=vfs")]
        listeners = []
        held = []
        for sp_path in paths:
            Path(sp_path).unlink(missing_ok=True)
            ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            ls.bind(sp_path)
            ls.listen(4)
            listeners.append(ls)

            def _accept_forever(sock=ls):
                while True:
                    try:
                        conn, _ = sock.accept()
                        held.append(conn)
                    except OSError:
                        return

            threading.Thread(target=_accept_forever, daemon=True).start()
        qparse = subprocess.Popen([real_argv[0], "-S", *real_argv[1:]],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(2.0)
        alive = qparse.poll() is None
        if alive:
            qparse.kill()
        qout, qerr = qparse.communicate(timeout=10)
        for conn in held:
            try:
                conn.close()
            except OSError:
                pass
        for ls in listeners:
            ls.close()
        for sp_path in paths:
            Path(sp_path).unlink(missing_ok=True)
        parse_bad = any(w in qerr.lower() for w in ("invalid option", "unknown option", "is unexpected",
                                                      "unknown property", "invalid parameter", "requires a value",
                                                      "io_uring", "iothread", "aio"))
        check("vm-virtiofs: real qemu parses -S -display none, the memfd object and vhost-user-fs-pci with stub sockets",
              not parse_bad and (alive or "vhost" in qerr.lower() or "socket" in qerr.lower() or qerr == ""),
              f"alive_at_2s={alive} stderr={qerr.strip()[:600]!r}")
else:
    print("SKIP: qemu-img not installed; skipping vm --volume and export-app tests", flush=True)

# 11. Interactive shell, VM loopback SSH forwarding, and OpenSSH ProxyJump config generator
shell_env = dict(env, TERM="screen-256color")
r_sh_cmd = subprocess.run(
    ["python3", COMPUTE, "shell", "train0", "--", "/bin/sh", "-c",
     "echo hello-shell; env; exit 7"],
    env=shell_env, capture_output=True, text=True, timeout=60,
)
check("shell: container command passes stdout, preserves TERM, strips desktop vars, and returns exit 7",
      r_sh_cmd.returncode == 7
      and "hello-shell" in r_sh_cmd.stdout
      and "TERM=screen-256color" in r_sh_cmd.stdout
      and "EXP_NAME=run42" in r_sh_cmd.stdout
      and "WAYLAND_DISPLAY=" not in r_sh_cmd.stdout
      and "DISPLAY=" not in r_sh_cmd.stdout
      and "DBUS_SESSION_BUS_ADDRESS=" not in r_sh_cmd.stdout,
      f"rc={r_sh_cmd.returncode} out={r_sh_cmd.stdout!r} err={r_sh_cmd.stderr!r}")

r_sh_default = subprocess.run(
    ["python3", COMPUTE, "shell", "train0"],
    env=shell_env, input="echo stdin-shell-ok; cat /etc/base-marker\nexit 0\n",
    capture_output=True, text=True, timeout=60,
)
check("shell: container default shell without explicit command reads stdin and exits cleanly",
      r_sh_default.returncode == 0
      and "stdin-shell-ok" in r_sh_default.stdout
      and "immutable-base-v1" in r_sh_default.stdout,
      f"rc={r_sh_default.returncode} out={r_sh_default.stdout!r} err={r_sh_default.stderr!r}")

r_sh_missing = run_cli(COMPUTE, "shell", "no-such-target")
check("shell: unknown target fails clearly", r_sh_missing.returncode != 0, r_sh_missing.stdout + r_sh_missing.stderr)

if shutil.which("qemu-img"):
    # Note: expvm (created earlier with --net user) already took auto port 2222.
    expvm_meta = json.loads((Path(env["LOCACHY_COMPUTE_BASE"]) / "vms" / "expvm" / "vm.json").read_text(encoding="utf-8"))
    check("vm-ssh: first --net user VM auto-allocates loopback SSH port 2222",
          expvm_meta.get("ssh_port") == 2222, repr(expvm_meta))

    r_vm2 = run_cli(COMPUTE, "vm", "create", "sshvm2", "--disk", str(base_qcow), "--memory-mb", "512", "--cpus", "1",
                    "--net", "user", "--ssh-user", "ubuntu")
    vm2_meta = json.loads(r_vm2.stdout) if r_vm2.returncode == 0 else {}
    check("vm-ssh: second --net user VM auto-allocates 2223 and persists --ssh-user",
          r_vm2.returncode == 0 and vm2_meta.get("ssh_port") == 2223 and vm2_meta.get("ssh_user") == "ubuntu",
          r_vm2.stdout + r_vm2.stderr)

    r_vm3 = run_cli(COMPUTE, "vm", "create", "sshvm3", "--disk", str(base_qcow), "--memory-mb", "512", "--cpus", "1",
                    "--net", "user", "--ssh-port", "2350", "--ssh-user", "admin")
    vm3_meta = json.loads(r_vm3.stdout) if r_vm3.returncode == 0 else {}
    check("vm-ssh: explicit --ssh-port 2350 succeeds",
          r_vm3.returncode == 0 and vm3_meta.get("ssh_port") == 2350 and vm3_meta.get("ssh_user") == "admin",
          r_vm3.stdout + r_vm3.stderr)

    r_vm_dup = run_cli(COMPUTE, "vm", "create", "sshvm-dup", "--disk", str(base_qcow), "--net", "user", "--ssh-port", "2350")
    check("vm-ssh: duplicate --ssh-port is rejected",
          r_vm_dup.returncode != 0 and "already assigned" in r_vm_dup.stderr, r_vm_dup.stdout + r_vm_dup.stderr)

    r_vm_none_port = run_cli(COMPUTE, "vm", "create", "sshvm-nonebad", "--disk", str(base_qcow), "--net", "none", "--ssh-port", "2250")
    check("vm-ssh: --net none with explicit --ssh-port is rejected",
          r_vm_none_port.returncode != 0 and "--net user" in r_vm_none_port.stderr, r_vm_none_port.stdout + r_vm_none_port.stderr)

    r_vm_zero = run_cli(COMPUTE, "vm", "create", "sshvm-off", "--disk", str(base_qcow), "--net", "user", "--ssh-port", "0")
    r_vm_zero_cmd = run_cli(COMPUTE, "vm", "cmd", "sshvm-off", "--json")
    zero_argv = json.loads(r_vm_zero_cmd.stdout) if r_vm_zero_cmd.returncode == 0 else []
    check("vm-ssh: --ssh-port 0 disables hostfwd while keeping user net",
          r_vm_zero.returncode == 0 and "user,model=virtio-net-pci" in zero_argv
          and not any("hostfwd=" in a for a in zero_argv),
          f"create={r_vm_zero.stderr} argv={zero_argv}")

    r_vm2_cmd = run_cli(COMPUTE, "vm", "cmd", "sshvm2", "--json")
    vm2_argv = json.loads(r_vm2_cmd.stdout) if r_vm2_cmd.returncode == 0 else []
    nic_val = after_flag(vm2_argv, "-nic") if "-nic" in vm2_argv else ""
    check("vm-ssh: vm cmd emits loopback-only hostfwd=tcp:127.0.0.1:2223-:22 (never 0.0.0.0 or bare :port)",
          nic_val == "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2223-:22"
          and "0.0.0.0" not in " ".join(vm2_argv)
          and "hostfwd=tcp::" not in " ".join(vm2_argv),
          f"nic={nic_val!r} argv={vm2_argv}")

    r_vmlist = run_cli(COMPUTE, "vm", "list")
    check("vm-ssh: vm list prints ssh=127.0.0.1:<port> for VMs with SSH forwarding",
          r_vmlist.returncode == 0
          and "sshvm2" in r_vmlist.stdout and "\tssh=127.0.0.1:2223" in r_vmlist.stdout
          and "\tssh=127.0.0.1:2350" in r_vmlist.stdout,
          r_vmlist.stdout + r_vmlist.stderr)

    # ssh-config: rc.conf ssh_port integration, plain text + JSON structure
    rc_ssh = OUT / "ssh-rc.conf"
    rc_ssh.write_text('ssh_port="22022"\n', encoding="utf-8")
    ssh_cfg_env = dict(env, LOCACHY_COMPUTE_RC=str(rc_ssh))

    r_cfg_txt = subprocess.run(
        ["python3", COMPUTE, "ssh-config", "--prefix", "gpu01", "--user", "alice"],
        env=ssh_cfg_env, capture_output=True, text=True, timeout=30,
    )
    cfg_txt = r_cfg_txt.stdout
    check("ssh-config: plain text emits jump host (auto tailscale0 + rc.conf ssh_port), container RemoteCommand, and VM ProxyJump",
          r_cfg_txt.returncode == 0
          and "Host gpu01\n    HostName 100.64.0.1\n    User alice\n    Port 22022" in cfg_txt
          and "Host gpu01-train0\n    HostName 100.64.0.1\n    User alice\n    Port 22022\n    RequestTTY yes\n    RemoteCommand sudo locachy-compute shell train0" in cfg_txt
          and "Host gpu01-expvm\n    HostName 127.0.0.1\n    Port 2222\n    ProxyJump gpu01" in cfg_txt
          and "Host gpu01-sshvm2\n    HostName 127.0.0.1\n    Port 2223\n    User ubuntu\n    ProxyJump gpu01" in cfg_txt
          and "Host gpu01-sshvm3\n    HostName 127.0.0.1\n    Port 2350\n    User admin\n    ProxyJump gpu01" in cfg_txt
          and "Host gpu01-sshvm-off" not in cfg_txt,
          cfg_txt + r_cfg_txt.stderr)

    # shell into a VM: stopped VM rejected, running VM invokes ssh to 127.0.0.1:<port>, --console proxies and detaches on Ctrl+]
    r_vmsh_stopped = run_cli(COMPUTE, "shell", "sshvm2")
    check("shell: stopped VM fails clearly",
          r_vmsh_stopped.returncode != 0 and "not running" in r_vmsh_stopped.stderr,
          r_vmsh_stopped.stdout + r_vmsh_stopped.stderr)

    vmsh_rdir = Path(env["LOCACHY_COMPUTE_RUN"]) / "vm-sshvm2"
    vmsh_rdir.mkdir(parents=True, exist_ok=True)
    ssh_stub_dir = OUT / "ssh-stub"
    ssh_stub_dir.mkdir(exist_ok=True)
    (ssh_stub_dir / "ssh").write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\nexit 5\n", encoding="utf-8")
    os.chmod(ssh_stub_dir / "ssh", 0o755)
    vm_sentinel = subprocess.Popen(["sleep", "30"])
    try:
        (vmsh_rdir / "qemu.pid").write_text(f"{vm_sentinel.pid}\n", encoding="utf-8")
        r_vmsh_ssh = subprocess.run(
            ["python3", COMPUTE, "shell", "sshvm2", "-i", "/tmp/test-key", "--", "uname", "-a"],
            env=dict(env, PATH=f"{ssh_stub_dir}:{env.get('PATH', '')}"),
            capture_output=True, text=True, timeout=30,
        )
        check("shell: VM SSH mode invokes ssh -p 2223 -l ubuntu -i /tmp/test-key 127.0.0.1 <cmd> and returns exit code",
              r_vmsh_ssh.returncode == 5
              and r_vmsh_ssh.stdout.splitlines() == ["-p", "2223", "-l", "ubuntu", "-i", "/tmp/test-key", "127.0.0.1", "uname", "-a"],
              f"rc={r_vmsh_ssh.returncode} out={r_vmsh_ssh.stdout!r} err={r_vmsh_ssh.stderr!r}")

        # --console over Unix serial socket: echoes payload and detaches cleanly on Ctrl+] (0x1d)
        import threading
        ser_sock = vmsh_rdir / "serial.sock"
        ser_sock.unlink(missing_ok=True)
        ser_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser_srv.bind(str(ser_sock))
        ser_srv.listen(1)
        received_box = []
        stop_ser = threading.Event()

        def _serial_echo():
            try:
                conn, _ = ser_srv.accept()
                with conn:
                    conn.settimeout(5.0)
                    conn.sendall(b"welcome-serial\n")
                    data = conn.recv(1024)
                    received_box.append(data)
                    conn.sendall(b"echo:" + data + b"\n")
                    # Keep socket open until the client detaches on Ctrl+].
                    stop_ser.wait(5.0)
            except OSError:
                pass

        ser_th = threading.Thread(target=_serial_echo, daemon=True)
        ser_th.start()
        p_con = subprocess.Popen(
            ["python3", COMPUTE, "shell", "sshvm2", "--console"],
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            assert p_con.stdin is not None and p_con.stdout is not None and p_con.stderr is not None
            line1 = p_con.stdout.readline()
            p_con.stdin.write(b"hi-vm")
            p_con.stdin.flush()
            line2 = p_con.stdout.readline()
            p_con.stdin.write(b"\x1dignored-after-detach")
            p_con.stdin.flush()
            rc_con = p_con.wait(timeout=10)
            con_err = p_con.stderr.read()
        finally:
            stop_ser.set()
            if p_con.poll() is None:
                p_con.kill()
                p_con.wait(timeout=5)
            ser_srv.close()
            ser_th.join(timeout=2)
            ser_sock.unlink(missing_ok=True)
        check("shell: VM --console connects to serial.sock, proxies bidirectionally, and detaches cleanly on Ctrl+]",
              rc_con == 0
              and line1 == b"welcome-serial\n"
              and line2 == b"echo:hi-vm\n"
              and b"Ctrl+]" in con_err
              and received_box == [b"hi-vm"],
              f"rc={rc_con} line1={line1!r} line2={line2!r} err={con_err!r} recv={received_box!r}")
    finally:
        (vmsh_rdir / "qemu.pid").unlink(missing_ok=True)
        vm_sentinel.terminate()
        vm_sentinel.wait(timeout=5)

    r_cfg_json = subprocess.run(
        ["python3", COMPUTE, "ssh-config", "--host", "100.64.9.9", "--user", "bob", "--port", "2222",
         "--prefix", "node9", "--no-sudo", "--json"],
        env=ssh_cfg_env, capture_output=True, text=True, timeout=30,
    )
    cfg_data = json.loads(r_cfg_json.stdout) if r_cfg_json.returncode == 0 else {}
    vm_entries = {v["name"]: v for v in cfg_data.get("vms", [])}
    env_entries = {e["name"]: e for e in cfg_data.get("envs", [])}
    check("ssh-config: --json returns jump, envs, vms, config text and respects --port/--no-sudo",
          r_cfg_json.returncode == 0
          and cfg_data.get("jump") == {"host_alias": "node9", "hostname": "100.64.9.9", "user": "bob", "port": 2222}
          and env_entries.get("train0", {}).get("remote_command") == "locachy-compute shell train0"
          and vm_entries.get("sshvm2", {}).get("hostname") == "127.0.0.1"
          and vm_entries.get("sshvm2", {}).get("port") == 2223
          and vm_entries.get("sshvm2", {}).get("user") == "ubuntu"
          and vm_entries.get("sshvm2", {}).get("proxy_jump") == "node9"
          and "RemoteCommand locachy-compute shell train0" in cfg_data.get("config", ""),
          r_cfg_json.stdout + r_cfg_json.stderr)
else:
    # Still validate ssh-config when qemu-img is absent
    rc_ssh = OUT / "ssh-rc.conf"
    rc_ssh.write_text('ssh_port="22022"\n', encoding="utf-8")
    ssh_cfg_env = dict(env, LOCACHY_COMPUTE_RC=str(rc_ssh))
    r_cfg_json = subprocess.run(
        ["python3", COMPUTE, "ssh-config", "--prefix", "gpu01", "--user", "alice", "--json"],
        env=ssh_cfg_env, capture_output=True, text=True, timeout=30,
    )
    cfg_data = json.loads(r_cfg_json.stdout) if r_cfg_json.returncode == 0 else {}
    check("ssh-config: generates jump and container entries with rc.conf ssh_port",
          r_cfg_json.returncode == 0
          and cfg_data.get("jump", {}).get("port") == 22022
          and any(e.get("name") == "train0" for e in cfg_data.get("envs", [])),
          r_cfg_json.stdout + r_cfg_json.stderr)

print("DRIVER-DONE", flush=True)
sys.exit(0 if fails == 0 else 1)
PYEOF

unshare --user --map-auto --map-root-user --net --mount --propagation private \
    bash -c '
        ip link set lo up
        # A namespace file for locachy-net and a tailscale0 address inside it,
        # as service_network makes them (a veth stands in for Tailscale).
        mount -t tmpfs tmpfs /run
        mkdir -p /run/netns
        : >/run/netns/locachy-net
        mount --bind /proc/self/ns/net /run/netns/locachy-net
        ip link add tailscale0 type veth peer name tailscale-peer
        ip addr add 100.64.0.1/32 dev tailscale0
        ip link set tailscale0 up
        ip link set tailscale-peer up
        exec python3 "$1" "$2" "$3"
    ' bash "$out/driver.py" "$out" "$src" >"$out/run.log" 2>&1 || :
grep -E '^(PASS|FAIL|SKIP): ' "$out/run.log" || cat "$out/run.log"
sub_fails=$(grep -c '^FAIL: ' "$out/run.log" || :)
fails=$((fails + sub_fails))
if ! grep -q '^DRIVER-DONE' "$out/run.log"; then
    note FAIL "compute: test driver did not finish cleanly: $(tail -n 12 "$out/run.log" | tr '\n' ' ')"
fi

# Foreground vm run as a non-root uid (the uid locachy-app runs a compute VM as):
# no root on its run path. Only this shell's own uid is used, never a privileged one.
uid=$(id -u)
echo "vm-run uid: $uid"
if [ "$uid" != 0 ]; then
    nr="$out/vmnr"
    mkdir -p "$nr/compute-base/vms/nrvm" "$nr/compute-run" "$nr/shim" "$nr/locked"
    cat >"$nr/shim/qemu-system-x86_64" <<'SHIM'
#!/bin/sh
d=$(dirname "$0")
printf '%s\n' "$@" >"$d/argv.txt"
trap 'printf term >"$d/term.txt"; exit 0' TERM INT
: >"$d/ready"
while :; do sleep 0.05; done
SHIM
    chmod 755 "$nr/shim/qemu-system-x86_64"
    python3 - "$nr" <<'PYEOF'
import json, os, sys
nr = sys.argv[1]
vdir = os.path.join(nr, "compute-base", "vms", "nrvm")
vm = {"version": 1, "name": "nrvm", "base_disk": os.path.join(nr, "base.qcow2"),
      "overlay_disk": os.path.join(vdir, "overlay.qcow2"), "memory_mb": 512, "cpus": 1,
      "shares": [], "volumes": [], "cloud_init": "", "net": "none",
      "created_at": "2026-11-24T00:00:00Z", "reset_at": "2026-11-24T00:00:00Z"}
with open(os.path.join(vdir, "vm.json"), "w", encoding="utf-8") as fh:
    json.dump(vm, fh)
os.chmod(os.path.join(vdir, "vm.json"), 0o600)
open(os.path.join(vdir, "overlay.qcow2"), "wb").close()
PYEOF
    # Refused with a clear error when this uid cannot create the run directory
    chmod 0500 "$nr/locked"
    rc_locked=0
    env LOCACHY_COMPUTE_BASE="$nr/compute-base" LOCACHY_COMPUTE_RUN="$nr/locked" LOCACHY_COMPUTE_KVM=0 \
        PATH="$nr/shim:$PATH" python3 "$src/link/locachy-compute" vm run nrvm >"$nr/locked.out" 2>&1 || rc_locked=$?
    chmod 0700 "$nr/locked"
    if [ "$rc_locked" -ne 0 ] && grep -q "cannot be created by uid $uid" "$nr/locked.out"; then
        note PASS "vm-run: uid $uid refuses an unwritable run directory with a clear error"
    else
        note FAIL "vm-run: unwritable run dir: rc=$rc_locked $(cat "$nr/locked.out")"
    fi
    # Foreground run: QEMU (the shim) gets -display gtk and -qmp, SIGTERM is forwarded, the status comes back
    env LOCACHY_COMPUTE_BASE="$nr/compute-base" LOCACHY_COMPUTE_RUN="$nr/compute-run" LOCACHY_COMPUTE_KVM=0 \
        PATH="$nr/shim:$PATH" python3 "$src/link/locachy-compute" vm run nrvm --display gtk \
        --qmp-sock "$nr/nr-qmp.sock" >"$nr/run.out" 2>&1 &
    run_pid=$!
    i=0
    while [ ! -e "$nr/shim/ready" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    kill -TERM "$run_pid" 2>/dev/null || true
    rc_run=0
    wait "$run_pid" || rc_run=$?
    if [ "$rc_run" -eq 0 ] && [ -f "$nr/shim/term.txt" ] \
        && grep -qx -- '-display' "$nr/shim/argv.txt" && grep -qx -- 'gtk,window-close=off' "$nr/shim/argv.txt" \
        && grep -qxF -- 'locachy-compute[nrvm]' "$nr/shim/argv.txt" \
        && grep -q -- '^unix:.*nr-qmp.sock' "$nr/shim/argv.txt" \
        && [ ! -e "$nr/compute-run/vm-nrvm/qemu.pid" ]; then
        note PASS "vm-run: uid $uid runs the VM in the foreground, forwards SIGTERM, returns its status, removes its pid file"
    else
        note FAIL "vm-run: foreground run: rc=$rc_run term=$(cat "$nr/shim/term.txt" 2>/dev/null) out=$(cat "$nr/run.out" 2>/dev/null)"
    fi
    # virtiofs as this uid: the stub virtiofsd logs the uid it runs as, the sockets exist before QEMU starts,
    # and SIGTERM to vm run stops QEMU and then virtiofsd, leaving no sockets or pids
    mkdir -p "$nr/compute-base/vms/nrvfs" "$nr/data"
    cat >"$nr/stub-virtiofsd.py" <<'VFSD'
#!/usr/bin/env python3
import os, signal, socket, sys, time
from pathlib import Path
args = sys.argv[1:]
here = Path(os.path.dirname(os.path.abspath(__file__)))
sock_path = next(a.split("=", 1)[1] for a in args if a.startswith("--socket-path="))
with open(here / "vfsd-args.log", "a", encoding="utf-8") as fh:
    fh.write(f"uid={os.getuid()} " + " ".join(args) + "\n")
with open(here / f"vfsd-{os.getpid()}.pid", "w", encoding="utf-8") as fh:
    fh.write(str(os.getpid()) + "\n")
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
srv.listen(4)
def _term(signum, frame):
    with open(here / "vfsd-term.txt", "a", encoding="utf-8") as fh:
        fh.write("term\n")
    try:
        os.unlink(sock_path)
    except OSError:
        pass
    os._exit(0)
signal.signal(signal.SIGTERM, _term)
while True:
    time.sleep(0.05)
VFSD
    chmod 755 "$nr/stub-virtiofsd.py"
    python3 - "$nr" "$nr/data" <<'PYEOF'
import json, os, sys
nr, data = sys.argv[1], sys.argv[2]
vdir = os.path.join(nr, "compute-base", "vms", "nrvfs")
vm = {"version": 1, "name": "nrvfs", "base_disk": os.path.join(nr, "base.qcow2"),
      "overlay_disk": os.path.join(vdir, "overlay.qcow2"), "memory_mb": 256, "cpus": 1,
      "shares": [{"host": data, "tag": "ds", "mode": "ro"}], "volumes": [],
      "cloud_init": "", "net": "none", "created_at": "2026-11-24T00:00:00Z", "reset_at": "2026-11-24T00:00:00Z"}
with open(os.path.join(vdir, "vm.json"), "w", encoding="utf-8") as fh:
    json.dump(vm, fh)
os.chmod(os.path.join(vdir, "vm.json"), 0o600)
open(os.path.join(vdir, "overlay.qcow2"), "wb").close()
PYEOF
    rm -f "$nr/shim/ready" "$nr/shim/term.txt" "$nr/shim/vfs-present.txt"
    cat >"$nr/shim/qemu-system-x86_64" <<'SHIM'
#!/bin/sh
d=$(dirname "$0")
printf '%s\n' "$@" >"$d/argv.txt"
present=yes
for p in $(sed -n 's/^socket,id=[^,]*,path=//p' "$d/argv.txt"); do
    [ -S "$p" ] || present=no
done
printf '%s' "$present" >"$d/vfs-present.txt"
trap 'printf term >"$d/term.txt"; exit 0' TERM INT
: >"$d/ready"
while :; do sleep 0.05; done
SHIM
    chmod 755 "$nr/shim/qemu-system-x86_64"
    env LOCACHY_COMPUTE_BASE="$nr/compute-base" LOCACHY_COMPUTE_RUN="$nr/compute-run" LOCACHY_COMPUTE_KVM=0 \
        LOCACHY_VIRTIOFSD="$nr/stub-virtiofsd.py" PATH="$nr/shim:$PATH" \
        python3 "$src/link/locachy-compute" vm run nrvfs --display none >"$nr/vfs-run.out" 2>&1 &
    vfs_pid=$!
    i=0
    while [ ! -e "$nr/shim/ready" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    vfs_ready=no
    [ -e "$nr/shim/ready" ] && vfs_ready=yes
    kill -TERM "$vfs_pid" 2>/dev/null || true
    rc_vfs=0
    wait "$vfs_pid" || rc_vfs=$?
    present=$(cat "$nr/shim/vfs-present.txt" 2>/dev/null || true)
    if [ "$vfs_ready" = yes ] && [ "$rc_vfs" -eq 0 ] && [ "$present" = yes ] \
        && grep -q "^uid=$uid " "$nr/vfsd-args.log" && grep -q -- '--readonly' "$nr/vfsd-args.log" \
        && [ "$(cat "$nr/vfsd-term.txt" 2>/dev/null | wc -l)" -eq 1 ] \
        && [ -z "$(ls "$nr/compute-run/vm-nrvfs"/vfs-*.sock 2>/dev/null || true)" ]; then
        note PASS "vm-virtiofs: uid $uid runs virtiofsd with the sockets ready before QEMU; SIGTERM stops QEMU then virtiofsd; no sockets left"
    else
        note FAIL "vm-virtiofs: non-root lifecycle: ready=$vfs_ready rc=$rc_vfs present=$present log=$(cat "$nr/vfsd-args.log" 2>/dev/null) out=$(cat "$nr/vfs-run.out" 2>/dev/null)"
    fi
    chmod -R u+rwX "$nr" 2>/dev/null || true
else
    note SKIP "vm-run: non-root check skipped (this run is uid 0)"
fi

echo "compute tests: $fails failed"
[ "$fails" -eq 0 ]
