#!/usr/bin/env python3
"""Real rclone FUSE mounts driven by rclone-client's supervisor.

Run as root of a disposable user+mount namespace (test-rclone-fuse.sh does
this). A local remote stands in for a cloud backend; locachy-net is replaced
by this namespace's own network, but mount propagation, resolver binding,
the private RC socket, refresh, status, pending writes and unmount are real.
"""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import types

SOURCE = Path(__file__).resolve().parents[1] / "link/rclone-client"
loader = importlib.machinery.SourceFileLoader("rclone_client", str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
cloud = importlib.util.module_from_spec(spec)
loader.exec_module(cloud)

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f": {detail}" if detail and not ok else ""), flush=True)
    failures += not ok


def wait(condition, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(0.1)
    return False


def main():
    rclone = os.environ["RCLONE"]
    base = Path(tempfile.mkdtemp(prefix="cloud-fuse-", dir=os.environ.get("SCRATCH", "/tmp")))
    home, remote_data = base / "home", base / "remote data"
    (home / ".config/rclone").mkdir(parents=True)
    remote_data.mkdir()
    (remote_data / "existing.txt").write_text("before\n")
    config = home / ".config/rclone/rclone.conf"
    config.write_text(f"[disk space]\ntype = alias\nremote = {remote_data}\n")
    resolver = base / "resolv.conf"
    resolver.write_text("nameserver 192.0.2.53\n")
    netns = base / "netns"
    netns.write_text("")  # stands in for /run/netns/locachy-net: nsenter is stubbed below
    runtime = Path(tempfile.mkdtemp(prefix="cf-", dir="/tmp"))  # Unix socket paths are short

    uid = os.getuid()
    user = types.SimpleNamespace(pw_name="root", pw_dir=str(home), pw_uid=uid, pw_gid=os.getgid())
    cloud.identity = lambda: (user, home / "Drives")
    cloud.RCLONE = rclone
    cloud.RUNTIME, cloud.CONTROL, cloud.STATUS = runtime, runtime / "control.sock", runtime / "status"
    cloud.NETNS, cloud.RESOLVER = str(netns), str(resolver)
    cloud.FUSE_CONF = base / "fuse.conf"  # mounting as namespace root needs no user_allow_other
    # setpriv to uid 1000 is not possible in a single-uid namespace: run as its root.
    cloud.user_command = lambda user, args: ["env", "-i", "PATH=/usr/bin", f"HOME={user.pw_dir}", *args]
    real_network = cloud.network_command

    def network(args):
        command = real_network(args)
        shell = command[command.index("-c") + 1]
        # The real namespace layout, with nsenter --net replaced by plain exec.
        command[command.index("-c") + 1] = shell.replace('exec nsenter --net="$net" --', "exec")
        return command

    cloud.network_command = network
    cloud.FUSERMOUNT = shutil.which("fusermount3") or "/usr/bin/fusermount3"

    service = cloud.Service()
    service.notify = lambda remote: None
    thread = threading.Thread(target=service.serve, daemon=True)
    thread.start()
    point = home / "Drives/disk space"

    def mounted():
        return cloud.mounts().get(str(point)) == ("fuse.rclone", "disk space:")

    def status():
        return (runtime / "status").read_text() if (runtime / "status").exists() else ""

    if not wait(mounted, 30):
        logs = "\n".join(p.read_text(errors="replace")[-2000:] for p in runtime.glob("*/rclone.log"))
        child = ""
        for r in service.remotes.values():
            if r.process:
                try:
                    child = "".join(l for l in open(f"/proc/{r.process.pid}/mountinfo") if "Drives" in l)
                    child += Path(f"/proc/{r.process.pid}/cmdline").read_bytes().replace(b"\0", b" ").decode()
                except OSError as e:
                    child = str(e)
        parent = "".join(l for l in open("/proc/self/mountinfo") if "Drives" in l)
        check("rclone mount appears at Drives/<remote with space>", False,
              status() + logs + "\nCHILD:\n" + child + "\nPARENT:\n" + parent)
        print("FAIL real rclone FUSE supervisor: no mount", flush=True)
        return 1
    check("rclone mount appears at Drives/<remote with space>", True)
    check("the existing remote file is listed", wait(lambda: (point / "existing.txt").exists(), 10))
    check("status reports ready", wait(lambda: "\tready\t" in status(), 10), status())

    # Propagation: a namespace created before the mount (a running container) sees it.
    probe = subprocess.run(["unshare", "--mount", "--propagation", "slave", "--", "sh", "-c",
                            'test -e "$1/existing.txt" && echo seen', "probe", str(point)],
                           capture_output=True, text=True, timeout=10)
    check("a later slave namespace sees the cloud mount", probe.stdout.strip() == "seen", probe.stderr)
    remote = service.remotes["disk space"]
    # Read inside rclone's own mount namespace: /proc/PID/root does not follow
    # an absolute /etc/resolv.conf symlink into that namespace.
    seen = subprocess.run(["nsenter", "--target", str(remote.process.pid), "--mount", "--", "cat", "/etc/resolv.conf"],
                          capture_output=True, text=True, timeout=10).stdout
    check("rclone resolves with the network namespace's resolver", "192.0.2.53" in seen, seen)
    check("the resolver bind did not leak to the host's namespace", "192.0.2.53" not in Path("/etc/resolv.conf").read_text())

    rc = list((runtime).glob("*/rc/rc.sock"))
    check("private RC is a Unix socket in a 0700 directory",
          len(rc) == 1 and (rc[0].parent.stat().st_mode & 0o777) == 0o700)
    log = next(runtime.glob("*/rclone.log"), None)
    check("raw rclone log is root-only", log is not None and (log.stat().st_mode & 0o777) == 0o600)

    # Remote change hidden by the directory cache, then refreshed through the control socket.
    os.listdir(point)
    (remote_data / "added remotely.txt").write_text("new\n")
    before = "added remotely.txt" in os.listdir(point)
    # The real CLI's client, against this test's control socket.
    rc_code = cloud.client("refresh", str(point))
    check("the remote change was cached before refresh", not before)
    check("refresh succeeds", rc_code == 0)
    check("refresh makes the remote change visible", "added remotely.txt" in os.listdir(point))
    check("status records the refresh time", wait(lambda: "\tready\t0\t" not in status(), 5), status())

    sub = point / "sub dir"
    sub.mkdir()
    os.listdir(sub)
    (remote_data / "sub dir" / "inner.txt").write_text("x\n")
    check("subdirectory refresh succeeds", cloud.client("refresh", str(sub)) == 0)
    check("subdirectory refresh shows the remote change", "inner.txt" in os.listdir(sub))
    check("refresh of a missing directory fails", cloud.client("refresh", str(point / "missing")) != 0)
    check("refresh outside managed drives is refused", cloud.client("refresh", str(base)) != 0)
    check("traversal path is refused", cloud.client("refresh", str(point) + "/../x") != 0)

    # A write through the mount reaches the remote; unmount waits for an empty queue.
    (point / "written.txt").write_text("through fuse\n")
    check("a written file reaches the remote", wait(lambda: (remote_data / "written.txt").exists(), 20))
    handle = open(point / "existing.txt")
    busy = cloud.client("unmount", str(point))
    check("unmount with an open file is refused", busy != 0 and mounted())
    handle.close()
    check("unmount succeeds once idle", wait(lambda: cloud.client("unmount", str(point)) == 0, 15))
    check("the mount is gone after unmount", not mounted())
    check("the empty mount directory is removed", not point.exists())
    check("remote data is untouched", (remote_data / "existing.txt").read_text() == "before\n"
          and (remote_data / "written.txt").read_text() == "through fuse\n")

    service.stopping = True
    thread.join(20)
    check("service stops", not thread.is_alive())
    check("no rclone FUSE mount remains", not any(t == "fuse.rclone" for t, _ in cloud.mounts().values()))
    shutil.rmtree(base, ignore_errors=True)
    shutil.rmtree(runtime, ignore_errors=True)
    print(f"{'PASS' if failures == 0 else 'FAIL'} real rclone FUSE supervisor: {failures} failures", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
