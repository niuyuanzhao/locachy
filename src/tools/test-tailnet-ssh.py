#!/usr/bin/python3 -I
"""Run as PID 1 in a disposable Linux VM with the native OpenSSH tools installed.

Use init=/path/to/test-tailnet-ssh.py locachy.tailnet-ssh-test=1 at boot.
The guest needs a writable throwaway root (not the initial ramfs), cgroup v2,
veth, nftables, OverlayFS, sudo, a static /usr/bin/busybox, and the installed
locachy-compute and static app-init. The test creates a guest-only sudoers file.
No physical or emulated network card or real Tailscale account is needed.
"""
import ctypes
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import subprocess
import sys
import time
import traceback

CLIENT = "/usr/local/lib/locachy/tailnet-ssh-client"
GROUP = Path("/sys/fs/cgroup/locachy-tailnet-ssh")
NET = Path("/run/netns/locachy-net")
SERVER4 = "100.64.17.1"
SERVER6 = "fd7a:115c:a1e0::17"
LAN4 = "192.0.2.1"
NEXT4 = "100.64.17.3"
HOST = Path("/etc/locachy/tailnet-ssh/ssh_host_ed25519_key")
ACL = HOST.parent / "authorized_keys"
RESOLVER = Path("/etc/netns/locachy-net/resolv.conf")
WORK = Path("/tmp/ssh-test")
COMPUTE = "/usr/local/bin/locachy-compute"
CGROUP_ROOT = Path("/sys/fs/cgroup")
COMPUTE_ROOT = CGROUP_ROOT / "locachy-compute"
GATES = Path("/home/sshfixture/compute-gates")
COMPUTE_JOBS = (("ssh-ok", "succeeded", 0), ("ssh-fail", "failed", 37), ("ssh-stop", "stopped", 43))
SSH_OPTS = ["-F", "/dev/null", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=2",
            "-o", "ConnectionAttempts=1", "-o", f"UserKnownHostsFile={WORK}/known_hosts",
            "-o", "GlobalKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
            "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes"]


def run(*args, timeout=10, **kwargs):
    return subprocess.run(args, check=True, timeout=timeout, **kwargs)


def ip(*args):
    return run("ip", *args)


def in_net(*args):
    return run("nsenter", f"--net={NET}", "--", *args, capture_output=True, text=True)


def in_ip(*args):
    return in_net("ip", *args)


def check(condition, description):
    if not condition:
        raise AssertionError(description)
    print(f"PASS: {description}", flush=True)


def eventually(condition, description):
    deadline = time.monotonic() + 8
    while True:
        if condition():
            check(True, description)
            return
        if time.monotonic() >= deadline:
            raise AssertionError(description)
        time.sleep(0.05)


def write(path, text, mode=0o644):
    path = Path(path)
    path.write_text(text)
    path.chmod(mode)


def key(path):
    run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(path))
    return Path(str(path) + ".pub").read_text()


def ssh_args(addr=SERVER4, user="sshfixture", identity="allowed", *opts):
    return ("ssh", *SSH_OPTS, "-i", str(WORK / identity), *opts, "-l", user, addr)


def refused(args, reason):
    result = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
    return result.returncode != 0 and re.search(reason, result.stderr, re.I) is not None


def listeners():
    return {line.split()[3] for line in in_net("ss", "-Hltn", "sport", "=", ":22").stdout.splitlines()}


def ss_ready(*addresses):
    return listeners() == {f"{x}:22" if ":" not in x else f"[{x}]:22" for x in addresses}


def files():
    WORK.mkdir(mode=0o700)
    ACL.mkdir(parents=True)
    RESOLVER.parent.mkdir(parents=True)
    Path("/run/locachy").mkdir()
    Path("/run/sshd").mkdir(mode=0o755, exist_ok=True)
    for user, uid in (("sshfixture", 1000), ("sshnokey", 1001), ("sshsystem", 999)):
        home = Path(f"/home/{user}")
        home.mkdir(parents=True)
        with Path("/etc/passwd").open("a") as out:
            out.write(f"{user}:x:{uid}:{uid}::{home}:/bin/sh\n")
        with Path("/etc/group").open("a") as out:
            out.write(f"{user}:x:{uid}:\n")
        with Path("/etc/shadow").open("a") as out:
            # Non-locked accounts with impossible password hashes.
            out.write(f"{user}:x:20000:0:99999:7:::\n")
        p = home / ".ssh"
        p.mkdir(mode=0o700)
        os.chown(home, uid, uid)
        os.chown(p, uid, uid)

    write(RESOLVER, "# tailnet-only test resolver\nnameserver 100.100.100.100\n")
    write("/etc/resolv.conf", "# host-only test resolver\n")
    allowed = key(WORK / "allowed")
    key(WORK / "other")
    for user in ("sshfixture", "sshsystem", "root"):
        write(ACL / user, allowed)
    home_file = Path("/home/sshnokey/.ssh/authorized_keys")
    write(home_file, allowed, 0o600)
    os.chown(home_file, 1001, 1001)


def network():
    Path("/run/netns").mkdir(parents=True, exist_ok=True)
    ip("link", "set", "lo", "up")
    ip("netns", "add", "locachy-net")
    in_ip("link", "set", "lo", "up")
    for iface, peer, local, remote in (("tailscale0", "tail-test", "100.64.17.2/24", None),
                                       ("lan0", "lan-test", "192.0.2.2/24", f"{LAN4}/24")):
        ip("link", "add", peer, "type", "veth", "peer", "name", iface, "netns", "locachy-net")
        ip("address", "add", local, "dev", peer)
        ip("link", "set", peer, "up")
        in_ip("link", "set", iface, "up")
        if remote:
            in_ip("address", "add", remote, "dev", iface)
    ip("-6", "address", "add", "fd7a:115c:a1e0::18/64", "dev", "tail-test", "nodad")
    # Distinct source addresses keep OpenSSH's per-source failed-auth penalty
    # from hiding the authorization checks that follow.
    for address in ("100.64.17.10/24", "100.64.17.11/24", "100.64.17.12/24",
                    "100.64.17.13/24", "100.64.17.14/24", "100.64.17.15/24", "100.64.17.16/24"):
        ip("address", "add", address, "dev", "tail-test")
    # Lay down the tailnet path's route, but add no Tailscale address on the
    # server yet. Non-tailnet global addresses on this interface cannot bind.
    in_ip("address", "add", "203.0.113.1/24", "dev", "tailscale0")


def ssh_detached(addr):
    pid = int(run(*ssh_args(addr), "setsid sleep 90 </dev/null >/dev/null 2>&1 & echo $!",
                  capture_output=True, text=True).stdout)
    check(Path(f"/proc/{pid}/cgroup").read_text() == "0::/locachy-tailnet-ssh\n",
          "ordinary detached SSH process stays in the service cgroup")
    return pid


def process_exited(pid):
    try:
        return "\nState:\tZ" in Path(f"/proc/{pid}/status").read_text()
    except FileNotFoundError:
        return True


def compute_ssh(addr, *args):
    try:
        result = run(*ssh_args(addr), shlex.join(("sudo", "-n", COMPUTE, *args)),
                     capture_output=True, text=True, timeout=18)
    except subprocess.CalledProcessError as exc:
        raise AssertionError(f"remote compute {args!r} exited {exc.returncode}: {exc.stderr}") from exc
    return result.stdout


def compute_setup():
    # This fixture is only reachable under the disposable guest's PID 1 guard.
    write("/etc/sudoers", f"sshfixture ALL=(root) NOPASSWD: {COMPUTE}\n", 0o440)
    rootfs = WORK / "compute-rootfs"
    (rootfs / "bin").mkdir(parents=True)
    shutil.copy2("/usr/bin/busybox", rootfs / "bin/busybox")
    for tool in ("sh", "sleep"):
        (rootfs / "bin" / tool).symlink_to("busybox")
    GATES.mkdir()
    os.chown(GATES, 1000, 1000)
    compute_ssh(SERVER6, "image", "import", "ssh-fixture", str(rootfs))
    # Concurrent jobs must not share an OverlayFS upper/work pair.
    for job, _, _ in COMPUTE_JOBS:
        compute_ssh(SERVER6, "env", "create", job, "--image", "ssh-fixture",
                    "--net", "none", "--mount", f"{GATES}:/gates:ro")


def compute_status(addr, job):
    return json.loads(compute_ssh(addr, "job", "status", job, "--json"))


def check_job_running(record):
    group = COMPUTE_ROOT / f"job-{record['job_id']}"
    launcher = Path(f"/proc/{record['pid']}/cgroup")
    supervisor = Path(f"/proc/{record['supervisor']['pid']}/cgroup")
    check(record["state"] == "running" and record["cgroup"] == str(group)
          and "populated 1" in (group / "cgroup.events").read_text().splitlines()
          and launcher.read_text() == f"0::/{group.relative_to(CGROUP_ROOT)}\n"
          and supervisor.read_text() == "0::/locachy-compute/supervisors\n",
          f"{record['job_id']}: real payload and supervisor cgroups are outside the SSH service cgroup")


def submit_compute(addr, job, command):
    return json.loads(compute_ssh(addr, "run", job, "--detach", "--job-id", job,
                                  "--", "/bin/sh", "-c", command))


def check_job_done(rec, job, state, code, log):
    description = f"{job}: SSH reports {state}, exact exit {code}, stdout and stderr"
    if (rec.get("state") != state or rec.get("exit_code") != code or not rec.get("ended_at")
            or log.splitlines() != [f"{job}-stdout", f"{job}-stderr"]):
        raise AssertionError(f"{description}: status {rec!r}, log {log!r}")
    check(True, description)
    group = COMPUTE_ROOT / f"job-{job}"
    check(rec.get("cgroup") == str(group) and not group.exists() and "cleanup_error" not in rec,
          f"{job}: payload cgroup removed before terminal status")


def exercise():
    files()
    service = subprocess.Popen((CLIENT,), start_new_session=True)
    try:
        time.sleep(0.2)
        check(service.poll() is None and not GROUP.exists(), "waits for network namespace before running sshd")
        network()
        eventually(lambda: GROUP.exists(), "enters the late network namespace")
        check(ss_ready(), "no listeners with only a LAN and non-Tailscale addresses")
        in_ip("address", "add", f"{SERVER4}/24", "dev", "tailscale0")
        in_ip("-6", "address", "add", f"{SERVER6}/64", "dev", "tailscale0", "nodad")
        eventually(lambda: ss_ready(SERVER4, SERVER6), "listens only on exact late tailnet IPv4 and IPv6 addresses")
        check(HOST.stat().st_mode & 0o777 == 0o600, "opt-in creates a private host key")
        check(refused((*ssh_args(), "true"), "host key verification failed|no .* host key is known"),
              "unknown host key is not trusted")
        pub = run("ssh-keygen", "-y", "-f", str(HOST), capture_output=True, text=True).stdout
        write(WORK / "known_hosts", f"{SERVER4},{SERVER6},{NEXT4} {pub}")
        for addr in (SERVER4, SERVER6):
            out = run(*ssh_args(addr), "id -u; cat /etc/resolv.conf; cat /proc/self/cgroup",
                      capture_output=True, text=True).stdout
            check(out.startswith("1000\n# tailnet-only test resolver\n")
                  and out.endswith("0::/locachy-tailnet-ssh\n"), f"pubkey exec as regular user in cgroup via {addr}")
        check(Path("/etc/resolv.conf").read_text() == "# host-only test resolver\n", "resolver bind is private")
        for source, user, identity in (("100.64.17.10", "root", "allowed"),
                                       ("100.64.17.11", "sshsystem", "allowed"),
                                       ("100.64.17.12", "sshnokey", "allowed"),
                                       ("100.64.17.13", "sshfixture", "other")):
            check(refused((*ssh_args(SERVER4, user, identity, "-b", source), "true"), "permission denied"),
                  f"rejects {user} with {identity} key")
        check(refused((*ssh_args(SERVER4, "sshfixture", "allowed", "-o", "PubkeyAuthentication=no",
                                "-o", "PreferredAuthentications=password,keyboard-interactive", "-b", "100.64.17.14"), "true"),
                      r"permission denied \(publickey\)"), "server advertises only public-key authentication")
        acl_file = ACL / "sshfixture"
        acl_file.chmod(0o666)
        check(refused((*ssh_args(), "true"), "permission denied"), "ignores a writable authorization file")
        acl_file.chmod(0o644)
        acl_file.rename(ACL / "saved")
        acl_file.symlink_to(ACL / "saved")
        check(refused((*ssh_args(), "true"), "permission denied"), "ignores a symlink authorization file")
        acl_file.unlink()
        (ACL / "saved").rename(acl_file)

        write(WORK / "sample", "tailnet file transfer\n")
        for args in ((), ("-O",)):
            run("scp", *args, *SSH_OPTS, "-i", str(WORK / "allowed"), str(WORK / "sample"),
                f"sshfixture@{SERVER4}:sample")
            check(Path("/home/sshfixture/sample").read_text() == "tailnet file transfer\n",
                  "legacy SCP transfer" if args else "modern SCP transfer")
        run("sftp", *SSH_OPTS, "-i", str(WORK / "allowed"), "-b", "-", f"sshfixture@{SERVER4}",
            input=f"get sample {WORK}/download\n", text=True)
        check((WORK / "download").read_text() == "tailnet file transfer\n", "SFTP session")

        compute_setup()
        # Each workload waits for a release or stop from a later SSH connection,
        # so none can finish before the submitting session closes and sshd rebinds.
        for job, state, code in COMPUTE_JOBS:
            if state == "stopped":
                command = (f"trap 'echo {job}-stderr >&2; exit {code}' TERM; "
                           f"echo {job}-stdout; while :; do sleep 0.05; done")
            else:
                command = (f"while [ ! -e /gates/{job} ]; do sleep 0.05; done; "
                           f"echo {job}-stdout; echo {job}-stderr >&2; exit {code}")
            check_job_running(submit_compute(SERVER6, job, command))

        ip("route", "replace", f"{SERVER4}/32", "via", LAN4, "dev", "lan-test")
        check(refused((*ssh_args(), "true"), "timed out|refused"), "blocks same tailnet address via LAN ingress")
        ip("route", "del", f"{SERVER4}/32")
        check(refused((*ssh_args(LAN4), "true"), "refused"), "does not listen on LAN address")
        check(refused(("nsenter", f"--net={NET}", "--", *ssh_args(), "true"), "timed out|refused"),
              "blocks same tailnet address via local loopback ingress")
        check(refused(("nsenter", f"--net={NET}", "--", *ssh_args("127.0.0.1"), "true"), "refused"),
              "does not listen on loopback address")
        check(refused((*ssh_args(SERVER4, "sshfixture", "allowed", "-o", "ExitOnForwardFailure=yes", "-R",
                                 "12345:127.0.0.1:12345"), "true"), "forwarding failed"), "remote forwarding is disabled")

        with subprocess.Popen((*ssh_args(SERVER6, "sshfixture", "allowed", "-tt"), "echo $$; exec sleep 60"),
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as tty:
            try:
                check(select.select([tty.stdout], [], [], 5)[0], "PTY reports its process ID")
                pid = int(tty.stdout.readline().strip())
                check(Path(f"/proc/{pid}/cgroup").read_text() == "0::/locachy-tailnet-ssh\n",
                      "PTY stays in service cgroup")
                in_ip("address", "del", f"{SERVER4}/24", "dev", "tailscale0")
                in_ip("address", "add", f"{NEXT4}/24", "dev", "tailscale0")
                eventually(lambda: ss_ready(NEXT4, SERVER6), "rebinds after address replacement")
                check(tty.wait(timeout=5) != 0, "old PTY session ends on rebind")
            finally:
                if tty.poll() is None:
                    tty.kill()
        check(run(*ssh_args(NEXT4), "id -u", capture_output=True, text=True).stdout == "1000\n",
              "new address accepts the same pinned host key")

        for job, state, code in COMPUTE_JOBS:
            check(compute_status(NEXT4, job).get("state") == "running",
                  f"{job}: detached job survives SSH disconnect and sshd rebind")
            if state == "stopped":
                # The launch acknowledgement only proves exec succeeded. Wait
                # for the first log line, printed after the TERM trap is armed.
                eventually(lambda: compute_ssh(NEXT4, "job", "log", job) == f"{job}-stdout\n",
                           f"{job}: workload is ready to handle TERM")
                record = json.loads(compute_ssh(NEXT4, "job", "stop", job))
                check(compute_status(NEXT4, job) == record, f"{job}: stop result is persisted")
            else:
                run(*ssh_args(NEXT4), f"touch {GATES / job}")
                eventually(lambda: compute_status(NEXT4, job).get("state") not in ("starting", "running"),
                           f"{job}: reaches terminal state after gate is released over SSH")
                record = compute_status(NEXT4, job)
            check_job_done(record, job, state, code, compute_ssh(NEXT4, "job", "log", job))
        eventually(lambda: "populated 0" in (COMPUTE_ROOT / "supervisors/cgroup.events").read_text().splitlines(),
                   "all detached compute supervisors exit")

        detached = ssh_detached(NEXT4)
        # Stop the SSH service after the compute jobs are complete. The service
        # cgroup kill must remove the ordinary detached process too.
        os.killpg(service.pid, 15)
        service.terminate()
        check(service.wait(timeout=10) == 0, "service shuts down cleanly")
        check(ss_ready() and not GROUP.exists() and "locachy-tailnet-ssh" not in in_net("nft", "list", "tables").stdout,
              "no remaining SSH listener, cgroup, or firewall table")
        check(process_exited(detached), "service stop kills ordinary detached SSH process")
    finally:
        if service.poll() is None:
            service.terminate()
            service.wait(timeout=10)


def poweroff():
    # glibc reboot() supplies the two Linux syscall magic arguments.
    ctypes.CDLL(None).reboot(0x4321FEDC)


def main():
    if (os.getpid() != 1 or not sys.platform.startswith("linux") or os.geteuid() != 0
            or "locachy.tailnet-ssh-test=1" not in Path("/proc/cmdline").read_text().split()):
        raise SystemExit("boot a disposable Linux guest into this test; never run on a host")
    try:
        exercise()
    except Exception:
        traceback.print_exc()
        print("FAIL: tailnet SSH guest test", flush=True)
    else:
        print("PASS: tailnet SSH guest test complete", flush=True)
    finally:
        os.sync()
        poweroff()
        # If reboot is unavailable, leave PID 1 alive for the VM timeout.
        while True:
            time.sleep(60)


if __name__ == "__main__":
    main()
