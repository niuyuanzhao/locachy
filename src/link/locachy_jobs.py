"""Fixed system-job client, shared by the installed TPM and boot tools.

No shell jobs or environment bypass: inline work is allowed only in an actual
job cgroup or offline (a chroot, or a host not running LOCachy's process 1).
"""
import array
import os
from pathlib import Path
import re
import socket
import sys
import time

sys.dont_write_bytecode = True
CONTROL = "/run/locachy/jobs/control"
CGROUP = "/sys/fs/cgroup/locachy-jobs"
INIT = "/usr/local/lib/locachy/init"
INPUT_MAX = 4096


def job_members(job):
    """The members of a job's cgroup. Under `ip netns exec` (pacman runs in
    locachy-net, so the hooks it runs do too: the guard, the rebuild hook) /sys
    is a fresh sysfs of that network namespace, and /sys/fs/cgroup in it is
    empty. The same file is then read through process 1's root, where the
    hierarchy is. Membership is still checked by pid, as before."""
    path = Path(CGROUP, job, "cgroup.procs")
    try:
        return path.read_text().splitlines()
    except FileNotFoundError:
        if os.readlink("/proc/self/ns/mnt") == os.readlink("/proc/1/ns/mnt"):
            raise
    return Path("/proc/1/root" + str(path)).read_text().splitlines()


def execution_mode():
    root = os.stat("/")
    host = os.stat("/proc/1/root")
    if (root.st_dev, root.st_ino) != (host.st_dev, host.st_ino):
        return "offline"
    lines = Path("/proc/self/cgroup").read_text().splitlines()
    for line in lines:
        match = re.fullmatch(r"0::/locachy-jobs/(job-[1-9][0-9]*)", line)
        if match:
            procs = job_members(match[1])
            if str(os.getpid()) in procs:
                return "job"
            raise RuntimeError("job cgroup membership could not be verified")
    pid1 = os.readlink("/proc/1/exe").removesuffix(" (deleted)")
    return "queue" if pid1 in (INIT, "/init") else "offline"


def ask(fields, input_fd=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC) as s:
        s.settimeout(5)
        s.connect(CONTROL)
        msg = b"\0".join(str(f).encode() for f in fields)
        rights = [] if input_fd is None else [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [input_fd]))]
        if s.sendmsg([msg], rights) != len(msg):
            raise RuntimeError("job service did not take the complete request")
        reply = s.recv(65536).decode().split("\0")
    if not reply or reply[0] != "ok":
        raise RuntimeError("job service: " + (reply[1] if len(reply) > 1 else "no answer"))
    return reply[1:]


def submit(kind, args=(), secret=None, wait=True, startup_wait=0):
    """Register once, optionally follow output/result. Input is an EOF pipe only.

    Retry only connect failures during startup, never a possibly accepted submit.
    Closing a UI/client does not cancel the registered job.
    """
    fd = None
    if secret is not None:
        data = secret.encode()
        if not 0 < len(data) <= INPUT_MAX:
            raise RuntimeError("passphrase must be 1..4096 UTF-8 bytes")
        fd, writer = os.pipe()
        os.set_inheritable(fd, False)
        os.set_inheritable(writer, False)
        try:
            view = memoryview(data)
            while view:
                try:
                    n = os.write(writer, view)
                except InterruptedError:
                    continue
                if n <= 0:
                    raise RuntimeError("incomplete passphrase input")
                view = view[n:]
        finally:
            os.close(writer)
    try:
        until = time.monotonic() + startup_wait
        while True:
            try:
                number, position = ask(["submit", kind, *args], fd)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= until:
                    raise
                time.sleep(0.1)
    finally:
        if fd is not None:
            os.close(fd)
    print(f"locachy-jobs: job {number} ({kind}), {position} before it", flush=True)
    if not wait:
        return 0
    offset = 0
    while True:
        try:
            [row] = ask(["status", number])
        except (OSError, RuntimeError):
            # A job outlives its service. Never resubmit after an accepted ID.
            print(f"locachy-jobs: job {number} still registered; service unavailable; check home jobs", file=sys.stderr)
            return 1
        data = b""
        try:
            with open(f"/run/locachy/jobs/{number}.log", "rb") as log:
                log.seek(offset)
                data = log.read()
                offset += len(data)
        except FileNotFoundError:
            pass # A queued job has no log yet.
        if data:
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()
        fields = row.split("\t")
        if fields[2] not in ("queued", "running", "stopping"):
            if fields[2] != "done":
                print(f"locachy-jobs: job {number} {fields[2]}: {fields[-1]}", file=sys.stderr)
            return 0 if fields[2] == "done" else 1
        time.sleep(0.2)


def pacman_ancestor():
    """Hooks run below pacman through one or more shell processes."""
    pid = os.getppid()
    for _ in range(32):
        try:
            comm = Path(f"/proc/{pid}/comm").read_text().strip()
            if comm == "pacman":
                return True
            text = Path(f"/proc/{pid}/stat").read_text()
            tail = text.rsplit(")", 1)[-1].split()
            pid = int(tail[1])
        except (OSError, ValueError, IndexError):
            return False
        if pid <= 1:
            return False
    return False


def boot_route(args):
    if args not in ([], ["--luks"], ["--register-only"]):
        raise RuntimeError("boot accepts only --luks or --register-only")
    mode = execution_mode()
    if mode != "queue":
        if mode == "offline":
            print("locachy-boot: offline root; building inline", file=sys.stderr)
        return 100
    if pacman_ancestor():
        # A PostTransaction hook must not wait while pacman holds its DB lock:
        # submit-only leaves the queue event for after pacman returns.
        ask(["submit", "boot", *args])
        print("locachy-boot: queued from pacman hook; it will run after the transaction", file=sys.stderr)
        return 0
    return submit("boot", args)


if __name__ == "__main__":
    try:
        if sys.argv[1:2] != ["boot"]:
            raise RuntimeError("only the fixed boot route is available")
        sys.exit(boot_route(sys.argv[2:]))
    except (OSError, ValueError, RuntimeError) as e:
        print(f"locachy-jobs: {e}; not run outside the queue", file=sys.stderr)
        sys.exit(1)
