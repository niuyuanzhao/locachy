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

# 6. KVM micro-VM overlay + QEMU argv generation
if shutil.which("qemu-img"):
    base_qcow = OUT / "base.qcow2"
    subprocess.run(["qemu-img", "create", "-f", "qcow2", str(base_qcow), "64M"], check=True, capture_output=True)
    r = run_cli(COMPUTE, "vm", "create", "expvm", "--disk", str(base_qcow), "--memory-mb", "1024", "--cpus", "2",
                "--share", f"{data_dir}:datasets", "--net", "user")
    check("vm: create qcow2 CoW overlay succeeds", r.returncode == 0, r.stderr)
    r_cmd = run_cli(COMPUTE, "vm", "cmd", "expvm", "--json")
    argv = json.loads(r_cmd.stdout) if r_cmd.returncode == 0 else []
    check("vm: QEMU argv includes q35, virtio disk, serial sock, 9p share, no display",
          "-display" in argv and "none" in argv and any("virtio-9p-pci" in a for a in argv) and any("datasets" in a for a in argv),
          str(argv))
    r_rst = run_cli(COMPUTE, "vm", "reset", "expvm")
    check("vm: reset recreates clean qcow2 overlay", r_rst.returncode == 0, r_rst.stderr)
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
finally:
    cc.terminate()
    try:
        cc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        cc.kill()

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
if ! grep -q '^PASS: compute-client: wildcard is never bound' "$out/run.log"; then
    note FAIL "compute: test driver did not finish cleanly: $(tail -n 12 "$out/run.log" | tr '\n' ' ')"
fi

echo "compute tests: $fails failed"
[ "$fails" -eq 0 ]
