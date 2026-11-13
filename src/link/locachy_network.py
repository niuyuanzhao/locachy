#!/usr/bin/python3
"""locachy_network.py: fail-closed transitions of the full-system network
layers of locachy-net (layer 1 Tor, layer 2 Clash; network-layers in
tor-client and clash-client). A short synchronous request, not a service:
it never waits for a daemon.

  set [--tor off|mihomo|bridges] [--clash off|tun|proxy]
        the target policy (an omitted layer stays as configured); a layer
        that is on needs its program (mihomo; tor; lyrebird for bridges),
        else the request is refused (exit 2)
  reconcile   enforce what rc.conf says (network-client, the daemons)
  status [--text]   desired / applied / phase / error, and the daemons
  lan         refresh Tor's local networks (tor-client)
  daemon claim tor|clash               print a generation token
  daemon release tor|clash TOKEN [--failed MESSAGE]

Every transition, under the stable STATE/policy.lock:
  1. a transition table blocks all output and forwarding but loopback and
     DHCP (locachy-transition, priority -250, before every other table);
  2. both layers' tables are replaced in one nft batch (one transaction);
  3. rc.conf is committed once, all keys together (locachy_config);
  4. only then the transition table goes.
A failure at any step leaves the transition table: nothing leaves until a
later set or reconcile succeeds. A failed request that was not committed is
not silently replaced by the older configuration: reconcile keeps the block
until set is run again (retry, or another choice).

Lock order: policy.lock, then rc.conf.lock. Daemon lifetimes have their own
STATE/LAYER.lifecycle.lock, taken by tor-client and clash-client for their
whole run (inherited by tor and mihomo, which die with them: pdeathsig);
policy.lock is never held while a daemon starts or stops.

Exit status: 0 applied (or queued: no locachy-net yet); 1 failed, the network
blocked; 2 usage or a refused request, nothing changed; 3 a stale daemon
generation, nothing touched; 4 failed and the block could not be loaded.
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import uuid

HERE = Path(__file__).resolve().parent
RC = Path(os.environ.get("LOCACHY_NETWORK_RC", "/etc/locachy/rc.conf"))
STATE = Path(os.environ.get("LOCACHY_NETWORK_STATE", "/var/lib/locachy/network"))
NS = os.environ.get("LOCACHY_NETWORK_NETNS", "locachy-net")
TOR_ENFORCE = os.environ.get("LOCACHY_TOR_ENFORCE", str(HERE / "tor-enforce"))
CLASH_CLIENT = os.environ.get("LOCACHY_CLASH_CLIENT", str(HERE / "clash-client"))
TOR_RUN = Path(os.environ.get("LOCACHY_TOR_RUN", "/run/locachy/tor"))
TOR_SERVICE = "/usr/local/lib/locachy/tor-client"
CLASH_SERVICE = "/usr/local/lib/locachy/clash-client"
FRONTS = ("mihomo", "bridges")
MODES = ("tun", "proxy")
TRANSITION = "locachy-transition"
# clash-client's: mihomo's routing table and rule priorities, its interface.
CLASH_TABLE, CLASH_RULES, OVER_TOR_RULE, CLASH_DEVICE = "2022", range(9000, 9100), 8999, "mihomo"


class Failed(Exception):
    """The transition failed with the block in place."""


class Unprotected(Exception):
    """Not even the block could be loaded."""


def run(argv, text=None):
    return subprocess.run([str(a) for a in argv], input=text, text=True, capture_output=True,
                          timeout=30, check=True)


def in_ns(*argv, text=None):
    return run(["ip", "netns", "exec", NS, *argv], text)


def read_rc():
    try:
        text = RC.read_text()
    except FileNotFoundError:
        text = ""
    values = dict(re.findall(r'^[ \t]*([A-Za-z0-9_]+)="([^"\n]*)"', text, re.MULTILINE))
    present = set(re.findall(r"^[ \t]*([A-Za-z0-9_]+)=", text, re.MULTILINE))
    return values, present


def configured():
    """The policy rc.conf has, as the daemons read it (tor-client, clash-client)."""
    values, present = read_rc()
    tor = "off"
    if "service_tor" in present:
        tor = values.get("tor_front") if values.get("tor_front") in FRONTS else "none"
    clash = "off"
    if "service_clash" in present:
        clash = "proxy" if values.get("clash_mode") == "proxy" else "tun"
    return {"tor": tor, "clash": clash}


def installed(variable, default):
    """Is the program there that settings checks before it offers its layer:
    the same path, and the override the daemons take (clash-client's
    LOCACHY_MIHOMO, tor-client's LOCACHY_TOR and LOCACHY_LYREBIRD)."""
    return os.access(os.environ.get(variable, default), os.X_OK)


def refused(target):
    """Why the target is refused; None when it is allowed."""
    if target["tor"] == "none":
        return "Tor needs a way out: mihomo or bridges"
    if target["tor"] == "mihomo" and target["clash"] == "off":
        return "Clash is Tor's front: put Tor through bridges or off first, or both off together"
    # A layer that is on needs its program: settings offers a layer only then
    # (settings.c: clash_installed, tor_installed, and lyrebird_installed for
    # the bridges front). Off needs nothing.
    if target["clash"] != "off" and not installed("LOCACHY_MIHOMO", "/usr/bin/mihomo"):
        return "Clash is on, but mihomo is not installed: install it in settings first"
    if target["tor"] != "off" and not installed("LOCACHY_TOR", "/usr/bin/tor"):
        return "Tor is on, but tor is not installed: install it in settings first"
    if target["tor"] == "bridges" and not installed("LOCACHY_LYREBIRD", "/usr/bin/lyrebird"):
        return "the bridges need lyrebird, which is not installed: install it in settings first"
    return None


def rc_changes(target):
    changes = {}
    if target["tor"] == "off":
        changes["service_tor"] = None
    else:
        changes.update(service_tor=TOR_SERVICE, tor_front=target["tor"])
    if target["clash"] == "off":
        changes["service_clash"] = None
    else:
        changes.update(service_clash=CLASH_SERVICE, clash_mode=target["clash"])
    return changes


@contextlib.contextmanager
def locked():
    STATE.mkdir(mode=0o755, parents=True, exist_ok=True)
    # Opened for append, never renamed or unlinked: one inode for every writer
    # (tor-enforce lan takes it with flock too).
    with open(STATE / "policy.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def boot_id():
    try:
        return Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except OSError:
        return ""


def load_state():
    try:
        state = json.loads((STATE / "status.json").read_text())
    except (FileNotFoundError, ValueError):
        state = {}
    for key, value in (("desired", None), ("applied", None), ("applied_boot", ""), ("phase", "unknown"),
                       ("error", ""), ("generation", ""), ("guard", False), ("daemons", {})):
        state.setdefault(key, value)
    return state


def save_state(state):
    fd, name = tempfile.mkstemp(prefix=".status.json.", dir=STATE)
    try:
        with os.fdopen(fd, "w") as out:
            os.fchmod(out.fileno(), 0o644)
            json.dump(state, out, sort_keys=True)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        os.replace(name, STATE / "status.json")
        name = ""
        directory = os.open(STATE, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if name:
            os.unlink(name)


def have_ns():
    return Path("/run/netns", NS).exists()


def drop_table(name):
    # Declaring it first makes the delete valid whether or not it is there,
    # inside the same batch.
    return f"table inet {name}\ndelete table inet {name}\n"


GUARD = drop_table(TRANSITION) + f"""table inet {TRANSITION} {{
    chain output {{
        type filter hook output priority -250; policy drop;
        oif lo accept
        udp sport 68 udp dport 67 accept
        udp sport 546 udp dport 547 accept
    }}
    chain forward {{
        type filter hook forward priority -250; policy drop;
    }}
}}
"""


def target_rules(target):
    """One batch: both layers' tables as the target has them."""
    if target["tor"] != "off":
        env = dict(os.environ, LOCACHY_TOR_NETNS=NS)
        tor = subprocess.run(["bash", TOR_ENFORCE, "rules", target["tor"]], env=env, text=True,
                             capture_output=True, timeout=30, check=True).stdout
        return tor + drop_table("locachy-clash")
    if target["clash"] == "tun":
        return drop_table("locachy-tor") + run(["bash", CLASH_CLIENT, "--rules"]).stdout
    return drop_table("locachy-tor") + drop_table("locachy-clash")


def detail(error):
    return (getattr(error, "stderr", "") or str(error)).strip().replace("\n", " ")[:400]


def transition(target, commit):
    """Enforce target (and commit it to rc.conf). Holds policy.lock."""
    state = load_state()
    # The phase as the last transition left it, before this one's.
    if commit or state["desired"] is None or state["phase"] != "failed":
        state["desired"] = target
    state.update(generation=uuid.uuid4().hex, phase="applying", error="")
    pending = state["desired"]
    save_state(state)
    step = "the transition block"
    try:
        if have_ns():
            in_ns("nft", "-f", "-", text=GUARD)
            state["guard"] = True
        if pending != target:
            # A failed set that never reached rc.conf: keep blocking rather
            # than fall back to the older choice silently.
            raise Failed(f"the request {json.dumps(pending, sort_keys=True)} failed and is not applied: "
                         "set it again, or choose another")
        if have_ns():
            step = "the layers' rules"
            in_ns("nft", "-f", "-", text=target_rules(target))
        if commit:
            step = str(RC)
            from locachy_config import update
            update(RC, rc_changes(target))
        if have_ns():
            step = "removing the transition block"
            in_ns("nft", "-f", "-", text=drop_table(TRANSITION))
            state.update(guard=False, applied=target, applied_boot=boot_id(), phase="applied")
        else:
            state.update(applied=None, applied_boot="", phase="queued")
        save_state(state)
        return state
    except Failed as error:
        state.update(phase="failed", error=str(error))
        save_state(state)
        raise
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        state.update(phase="failed", error=f"{step}: {detail(error)}")
        save_state(state)
        if not state["guard"] and have_ns():
            raise Unprotected(state["error"]) from error
        raise Failed(state["error"]) from error


def clean_routes():
    """What mihomo's auto-route leaves when it is killed; only the current
    generation, with its lifecycle lock held (no other mihomo exists)."""
    if not have_ns():
        return
    for family in ("-4", "-6"):
        for line in in_ns("ip", family, "rule", "show").stdout.splitlines():
            priority = int(line.split(":", 1)[0])
            if priority in CLASH_RULES or priority == OVER_TOR_RULE:
                in_ns("ip", family, "rule", "del", "priority", str(priority))
        with contextlib.suppress(subprocess.CalledProcessError):
            in_ns("ip", family, "route", "flush", "table", CLASH_TABLE)
    with contextlib.suppress(subprocess.CalledProcessError):
        in_ns("ip", "link", "del", CLASH_DEVICE)


def daemon(args):
    with locked():
        if args.operation == "claim":
            try:
                transition(configured(), commit=False)
            except Failed:
                pass  # blocked: the daemon may start, nothing leaves
            state = load_state()
            token = uuid.uuid4().hex
            state["daemons"][args.layer] = {"token": token, "state": "running", "error": ""}
            save_state(state)
            if args.layer == "clash":
                clean_routes()  # a killed predecessor's
            print(token)
            return 0
        state = load_state()
        if state["daemons"].get(args.layer, {}).get("token") != args.token:
            return 3
        # The configured policy, whatever this generation started with: an
        # old daemon's exit never removes a newer choice's rules.
        with contextlib.suppress(Failed):
            transition(configured(), commit=False)
        if args.layer == "clash":
            clean_routes()
        else:
            (TOR_RUN / "tor.pid").unlink(missing_ok=True)
        state = load_state()
        state["daemons"][args.layer] = {"token": "", "state": "failed" if args.failed else "stopped",
                                        "error": args.failed}
        save_state(state)
        return 0


def refresh_lan():
    """Tor's set of local networks, after they changed; never adds or
    removes a table, so a stale caller cannot undo a newer policy."""
    if not have_ns() or configured()["tor"] == "off":
        return 0
    env = dict(os.environ, LOCACHY_TOR_NETNS=NS)
    subprocess.run(["bash", TOR_ENFORCE, "lan-locked"], env=env, timeout=30, check=True)
    return 0


def status(text):
    state = load_state()
    state["configured"] = configured()
    if state["applied"] is not None and state["applied_boot"] != boot_id():
        state["applied"] = None  # tables of an earlier boot
        if state["phase"] == "applied":
            state["phase"] = "queued"
    del state["applied_boot"]
    for owner in state["daemons"].values():
        owner.pop("token", None)
    if not text:
        print(json.dumps(state, sort_keys=True))
        return 0
    def name(policy):
        return "none" if policy is None else f"Tor {policy['tor']}, Clash {policy['clash']}"
    line = f"{state['phase']}: desired {name(state['desired'])}; applied {name(state['applied'])}"
    if state["guard"]:
        line += "; the network is blocked"
    if state["error"]:
        line += f"; {state['error']}"
    print(line)
    return 0


def main():
    parser = argparse.ArgumentParser(prog="locachy_network.py",
                                     description="Fail-closed network policy transitions.")
    commands = parser.add_subparsers(dest="command", required=True)
    request = commands.add_parser("set")
    request.add_argument("--tor", choices=("off",) + FRONTS)
    request.add_argument("--clash", choices=("off",) + MODES)
    commands.add_parser("reconcile")
    commands.add_parser("lan")
    query = commands.add_parser("status")
    query.add_argument("--text", action="store_true")
    lifecycle = commands.add_parser("daemon")
    lifecycle.add_argument("operation", choices=("claim", "release"))
    lifecycle.add_argument("layer", choices=("tor", "clash"))
    lifecycle.add_argument("token", nargs="?", default="")
    lifecycle.add_argument("--failed", default="")
    args = parser.parse_args()
    try:
        if args.command == "daemon":
            return daemon(args)
        with locked():
            if args.command == "status":
                return status(args.text)
            if args.command == "lan":
                return refresh_lan()
            target = configured()
            if args.command == "set":
                for layer in ("tor", "clash"):
                    if getattr(args, layer) is not None:
                        target[layer] = getattr(args, layer)
                why = refused(target)
                if why:
                    print(f"network policy: refused: {why}", file=sys.stderr)
                    return 2
            transition(target, commit=args.command == "set")
            return 0
    except Failed as error:
        print(f"network policy: failed, the network is blocked: {error}", file=sys.stderr)
        return 1
    except Unprotected as error:
        print(f"network policy: failed, and the block could not be loaded: {error}", file=sys.stderr)
        return 4
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"network policy: {detail(error)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
