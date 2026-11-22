#!/usr/bin/env python3
"""Linux integration test: python3 tools/test-build-jobserver.py SCRATCH_PARENT.

Requires GNU Make >= 4.4 and Ninja >= 1.13 on PATH. The harness owns the FIFO
and reserves one implicit slot per build it launches, as a build dispatcher would.
"""
import argparse
from array import array
from contextlib import ExitStack, suppress
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import termios


TIMEOUT = 30
FIFO = "jobserver.fifo"
AUTH = f"--jobserver-auth=fifo:{FIFO}"
BARRIER = "workers.sock"
SLOTS = 4
TOKENS = b"+" * (SLOTS - 2)
RECIPE = 'exec "$$JOBSERVER_TEST_PYTHON" "$$JOBSERVER_TEST_SCRIPT" --worker'


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def worker(target):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.connect(BARRIER)
        report = [target, sorted(os.sched_getaffinity(0)), os.environ.get("MAKEFLAGS", "")]
        conn.sendall(json.dumps(report).encode() + b"\n")
        if conn.recv(1) != b"+":
            sys.exit(f"{target}: barrier closed before release")
    Path(target).touch()


def tool(name, pattern, minimum, requirement, env):
    executable = shutil.which(name)
    check(executable, f"{requirement} is required on PATH")
    executable = str(Path(executable).absolute())
    output = subprocess.check_output([executable, "--version"], env=env, text=True, timeout=TIMEOUT)
    version = output.partition("\n")[0]
    match = re.match(pattern, version)
    check(match and tuple(map(int, match.groups())) >= minimum,
          f"{requirement} is required; found {version or 'no version output'}")
    return executable


def makefile(path, targets):
    Path(path).write_text(".PHONY: all\nall: " + " ".join(targets) + "\n" +
                          "".join(f"{target}:\n\t@{RECIPE} $@\n" for target in targets))


def stop(process):
    if process.poll() is not None:
        return
    with suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def exercise(make, ninja, env, cpus):
    ninja_targets = [f"ninja-{i}" for i in range(1, SLOTS + 1)]
    make_targets = [f"make-{i}" for i in range(1, SLOTS)]
    makefile("serial.mk", ["serial"])
    makefile("parallel.mk", make_targets)
    Path("build.ninja").write_text(f"rule work\n  command = {RECIPE} $out\n" +
                                  "".join(f"build {name}: work\n" for name in ninja_targets))

    with ExitStack() as cleanup:
        # Keep both ends open without lending the descriptor to any child.
        os.mkfifo(FIFO, 0o600)
        pool = os.open(FIFO, os.O_RDWR | os.O_NONBLOCK)
        cleanup.callback(os.close, pool)
        os.write(pool, TOKENS)
        listener = cleanup.enter_context(socket.socket(socket.AF_UNIX))
        listener.bind(BARRIER)
        listener.listen()
        listener.settimeout(TIMEOUT)

        def spawn(label, args):
            with open(f"{label}.log", "wb") as log:
                process = subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
            cleanup.callback(stop, process)
            check(os.sched_getaffinity(process.pid) == set(cpus),
                  f"{label}: build did not inherit CPU affinity {cpus}")
            return process

        def ready(pending, joins_pool=True):
            try:
                conn, _ = listener.accept()
            except TimeoutError as error:
                raise RuntimeError(f"timed out waiting for recipes: {sorted(pending)}") from error
            cleanup.enter_context(conn)
            conn.settimeout(TIMEOUT)
            with conn.makefile("rb") as stream:
                target, affinity, flags = json.loads(stream.readline())
            check(target in pending, f"unexpected worker {target}; waiting for {sorted(pending)}")
            pending.remove(target)
            check(affinity == cpus, f"{target}: expected CPU affinity {cpus}, got {affinity}")
            if joins_pool:
                check(AUTH in flags.split(), f"{target}: missing FIFO in MAKEFLAGS={flags!r}")
            else:
                check("-j1" in flags.split() and "--jobserver-auth=" not in flags,
                      f"{target}: forced -j1 did not reset MAKEFLAGS={flags!r}")
            return conn

        def expect_tokens(count, stage):
            queued = array("i", [0])
            fcntl.ioctl(pool, termios.FIONREAD, queued)
            check(queued[0] == count, f"{stage}: expected {count} FIFO bytes, got {queued[0]}")

        def release(conn):
            conn.sendall(b"+")
            conn.close()

        def finish(process, label):
            status = process.wait(timeout=TIMEOUT)
            check(status == 0, f"{label}: build exited with status {status}")

        serial = spawn("serial", [make, "-f", "serial.mk", "-j1"])
        serial_gate = ready({"serial"}, joins_pool=False)
        expect_tokens(len(TOKENS), "serial recipe held")

        build = spawn("ninja", [ninja, *ninja_targets])  # No -j: Ninja must use the FIFO.
        pending = set(ninja_targets)
        held = [ready(pending) for _ in range(SLOTS - 1)]
        expect_tokens(0, "serial and three Ninja recipes held")
        # Ninja 1.13 waits for a local completion when out of slots. Keep
        # Make blocked and release exactly one Ninja command to wake it.
        release(held.pop())
        held.append(ready(pending))
        expect_tokens(0, "replacement Ninja recipe held")
        for conn in held:
            release(conn)
        finish(build, "Ninja")
        expect_tokens(len(TOKENS), "after Ninja")

        # Ninja reports started/finished/total before returning the completed
        # edge's slot. Its first completion must still see only three starts,
        # even if an erroneously spawned fourth worker was slow to connect.
        status = re.search(r"^\[slots:(\d+):(\d+):(\d+)\] ",
                           Path("ninja.log").read_text(errors="replace"), re.MULTILINE)
        check(status and tuple(map(int, status.groups())) == (SLOTS - 1, 1, SLOTS),
              "Ninja's first completion did not report three starts out of four")

        # Reuse the same FIFO with an ordinary Make client and no CLI -j override.
        build = spawn("parallel", [make, "-f", "parallel.mk"])
        pending = set(make_targets)
        held = [ready(pending) for _ in make_targets]
        expect_tokens(0, "serial and three Make recipes held")
        for conn in held:
            release(conn)
        finish(build, "parallel Make")
        expect_tokens(len(TOKENS), "after parallel Make")

        release(serial_gate)
        finish(serial, "forced Make")
        expect_tokens(len(TOKENS), "after both implicit slots are done")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scratch", type=Path, help="existing directory that can hold FIFOs")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this developer test requires Linux")
    env = os.environ.copy()
    for name in ("MAKEFLAGS", "GNUMAKEFLAGS", "MFLAGS", "MAKEFILES", "MAKELEVEL", "MAKEOVERRIDES"):
        env.pop(name, None)
    env.update(LC_ALL="C", TERM="dumb")
    try:
        make = tool("make", r"GNU Make (\d+)\.(\d+)", (4, 4), "GNU Make >= 4.4", env)
        ninja = tool("ninja", r"(\d+)\.(\d+)", (1, 13), "Ninja >= 1.13", env)
        env.update(MAKEFLAGS=f"-j {AUTH}", NINJA_STATUS="[slots:%s:%f:%t] ",
                   JOBSERVER_TEST_PYTHON=sys.executable,
                   JOBSERVER_TEST_SCRIPT=str(Path(__file__).resolve()))
        original = os.sched_getaffinity(0)
        cpus = sorted(original)[:SLOTS]
        cwd = os.getcwd()
        with tempfile.TemporaryDirectory(prefix="build-jobserver-", dir=args.scratch.resolve()) as root:
            # Relative FIFO and socket names work even with a long or spaced scratch path.
            os.chdir(root)
            try:
                os.sched_setaffinity(0, cpus)
                exercise(make, ninja, env, cpus)
            except (RuntimeError, OSError, subprocess.SubprocessError):
                for log in sorted(Path().glob("*.log")):
                    print(f"--- {log} ---\n{log.read_text(errors='replace')}", file=sys.stderr)
                raise
            finally:
                os.sched_setaffinity(0, original)
                os.chdir(cwd)
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        sys.exit(f"FAIL: build jobserver: {error}")
    print("PASS: build jobserver: forced make -j1 + Ninja (three held, fourth reused a slot)")
    print(f"PASS: build jobserver: parallel Make reused the FIFO; tokens restored; CPU affinity {cpus}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--worker":
        worker(sys.argv[2])
    else:
        main()
