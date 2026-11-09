#!/usr/bin/env python3
"""Authenticated, explicitly selected rescue Ethernet cable (live medium only).

The UI owns the password prompt. start reads exact password bytes from stdin;
no action prints them. The service owns sshd's cgroup and private shadow mount.
Only this service's network interface and files are cleaned up, never targets.
"""
import contextlib
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time

ROOT = Path("/run/locachy-rescue/link")
STATE = ROOT / "state.json"
NETNS = "locachy-rescue-link"
SERVICE = "locachy-rescue-link.service"
PORT = 2222
ENV = {"PATH": "/usr/bin:/usr/sbin", "LC_ALL": "C", "LANG": "C"}


class LinkError(Exception):
    pass


def command(args, *, data=None, timeout=15, check=True):
    try:
        result = subprocess.run(args, input=data, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=ENV, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise LinkError("Command unavailable or timed out: " + args[0]) from exc
    if check and result.returncode:
        # Tool output may echo sensitive input. Never relay it to UI or journal.
        raise LinkError("Command failed: " + " ".join(str(arg) for arg in args))
    return result


def text(args, **kwargs):
    return command(args, **kwargs).stdout.decode("utf-8", "replace").strip()


def ip(*args, ns=False, check=True):
    return command(["ip", *( ["-n", NETNS] if ns else []), *args], check=check)


def ip_json(*args, ns=False):
    return json.loads(ip("-j", *args, ns=ns).stdout)


def secure_directory(path):
    if path.exists():
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
            raise LinkError("Unsafe rescue runtime directory")
    else:
        path.mkdir(mode=0o700)


def runtime():
    secure_directory(ROOT.parent)
    secure_directory(ROOT)


@contextlib.contextmanager
def locked(name):
    runtime()
    fd = os.open(ROOT / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


def write_bytes(path, data):
    fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)


def save(state):
    write_bytes(ROOT / "state.new", (json.dumps(state) + "\n").encode())
    os.replace(ROOT / "state.new", STATE)


def load():
    if not STATE.exists():
        return None
    info = STATE.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
        raise LinkError("Unsafe rescue state file")
    return json.loads(STATE.read_bytes())


def valid_name(name):
    return bool(re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", name)) and name not in (".", "..")


def interfaces():
    found = []
    for path in sorted(Path("/sys/class/net").iterdir()):
        try:
            if (not valid_name(path.name) or not (path / "device").exists()
                    or (path / "wireless").exists() or (path / "phy80211").exists()
                    or (path / "type").read_text().strip() != "1"
                    or (path / "master").exists()):
                continue
            address = (path / "address").read_text().strip()
            try:
                carrier = (path / "carrier").read_text().strip() == "1"
            except OSError:
                carrier = False
            found.append({"name": path.name, "address": address, "carrier": carrier})
        except OSError:
            continue
    return found


def nm_running():
    return command(["systemctl", "is-active", "--quiet", "NetworkManager.service"],
                   check=False).returncode == 0


def nm_get(field, interface):
    return text(["nmcli", "-g", field, "device", "show", interface])


def snapshot(interface):
    path = Path("/sys/class/net") / interface
    links = ip_json("link", "show", "dev", interface)
    link = links[0]
    # A device with existing unmanaged L3 state cannot be faithfully rebuilt
    # from only a profile UUID. Refuse it instead of silently losing routes.
    nm = nm_running()
    managed = nm and nm_get("GENERAL.NM-MANAGED", interface) == "yes"
    uuid = nm_get("GENERAL.CON-UUID", interface) if managed else ""
    if uuid == "--":
        uuid = ""
    nm_state = nm_get("GENERAL.STATE", interface) if managed else ""
    nm_active = bool(re.match(r"^100\b", nm_state))
    addresses = ip_json("address", "show", "dev", interface)[0].get("addr_info", [])
    if not managed and addresses:
        raise LinkError("The unmanaged port has configured addresses; release it first")
    if managed and nm_active and not uuid:
        raise LinkError("The connected port has no restorable NetworkManager profile")
    if uuid and not re.fullmatch(r"[a-fA-F0-9-]{36}", uuid):
        raise LinkError("The port's NetworkManager connection identity is invalid")
    return {"interface": interface, "device": str((path / "device").resolve()),
            "ifindex": link["ifindex"], "mac": link["address"],
            "up": "UP" in link.get("flags", []), "nm_running": nm,
            "nm_managed": managed, "nm_uuid": uuid, "nm_active": nm_active,
            "nm_autoconnect": nm_get("GENERAL.AUTOCONNECT", interface) if managed else "no",
            "address": "", "fingerprint": "", "state": "preparing",
            "detached": False, "namespace": False, "moved": False}


def validate_password(password):
    if not 12 <= len(password) <= 256:
        raise LinkError("The session password must contain 12 to 256 bytes")
    if any(c in password for c in (b"\x00", b"\n", b"\r")):
        raise LinkError("The session password cannot contain NUL or line breaks")


def private_auth(password):
    # OpenSSL's random salt and stdin interface need neither a plaintext
    # password file nor the global account lock.
    auth = ROOT / "auth"
    secure_directory(auth)
    secure_directory(auth / "etc")
    rows = Path("/etc/shadow").read_bytes().splitlines()
    if sum(row.startswith(b"root:") for row in rows) != 1:
        raise LinkError("The live root account is not uniquely defined")
    hashed = command(["openssl", "passwd", "-6", "-stdin"], data=password + b"\n").stdout.strip()
    if not re.fullmatch(rb"\$6\$[./A-Za-z0-9]{1,16}\$[./A-Za-z0-9]{86}", hashed):
        raise LinkError("The private session password was not hashed")
    # No inherited aging policy may prevent this temporary rescue account.
    replacement = b"root:" + hashed + b":::::::\n"
    write_bytes(auth / "etc/shadow", b"".join(
        replacement if row.startswith(b"root:") else row + b"\n" for row in rows))


def ssh_config(address, interface):
    return f"""Port {PORT}
AddressFamily inet6
ListenAddress [{address}%{interface}]:{PORT}
HostKey {ROOT}/host_ed25519
PidFile {ROOT}/sshd.pid
UsePAM no
PermitRootLogin yes
AllowUsers root
AuthenticationMethods password
PasswordAuthentication yes
PermitEmptyPasswords no
KbdInteractiveAuthentication no
PubkeyAuthentication no
HostbasedAuthentication no
GSSAPIAuthentication no
AuthorizedKeysFile none
DisableForwarding yes
PermitTunnel no
PermitUserRC no
PermitUserEnvironment no
X11Forwarding no
UseDNS no
LoginGraceTime 30
MaxAuthTries 3
MaxStartups 2:30:4
MaxSessions 4
ClientAliveInterval 30
ClientAliveCountMax 3
PrintMotd no
LogLevel ERROR
Subsystem sftp internal-sftp
"""


def sysctl(key, value, *, ns=True):
    if ns:
        command(["ip", "netns", "exec", NETNS, "sysctl", "-q", "-w", key + "=" + str(value)])
    else:
        command(["sysctl", "-q", "-w", key + "=" + str(value)])


def namespace_configure(state):
    interface = state["interface"]
    ip("netns", "add", NETNS)
    state["namespace"] = True
    save(state)
    # Configure safe defaults before the physical device enters this namespace.
    for key, value in (("net.ipv4.ip_forward", 0), ("net.ipv6.conf.all.forwarding", 0),
                       ("net.ipv6.conf.default.accept_ra", 0),
                       ("net.ipv6.conf.default.autoconf", 0),
                       ("net.ipv6.conf.default.disable_ipv6", 0)):
        sysctl(key, value)
    ip("link", "set", "dev", interface, "down")
    ip("link", "set", "dev", interface, "netns", NETNS)
    state["moved"] = True
    save(state)
    ip("address", "flush", "dev", interface, ns=True)
    ip("-6", "address", "flush", "dev", interface, ns=True)
    # The fresh namespace starts without routes; no host route is imported.
    for key, value in (("accept_ra", 0), ("autoconf", 0), ("forwarding", 0),
                       ("disable_ipv6", 0), ("use_tempaddr", 0), ("addr_gen_mode", 0)):
        sysctl("net.ipv6.conf." + interface + "." + key, value)
    # Disable/enable regenerates only the kernel link-local address, with DAD.
    sysctl("net.ipv6.conf." + interface + ".disable_ipv6", 1)
    sysctl("net.ipv6.conf." + interface + ".disable_ipv6", 0)
    ip("link", "set", "dev", "lo", "up", ns=True)
    ip("link", "set", "dev", interface, "up", ns=True)
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        row = ip_json("address", "show", "dev", interface, ns=True)[0]
        for item in row.get("addr_info", []):
            flags = item.get("flags", [])
            if item.get("dadfailed") or "dadfailed" in flags:
                raise LinkError("The rescue port's IPv6 duplicate-address check failed")
            tentative = item.get("tentative") or "tentative" in flags
            if (item.get("family") == "inet6" and item.get("scope") == "link"
                    and not tentative and ipaddress.ip_address(item["local"]).is_link_local):
                state["address"] = item["local"]
                save(state)
                return
        time.sleep(0.1)
    raise LinkError("No usable IPv6 link-local address; check the selected cable")


def listening():
    result = command(["ip", "netns", "exec", NETNS, "ss", "-H", "-lnt", "sport", "=", str(PORT)],
                     check=False)
    return result.returncode == 0 and bool(result.stdout.strip())


def service_active():
    return command(["systemctl", "is-active", "--quiet", SERVICE], check=False).returncode == 0


def start(interface, password):
    validate_password(password)
    with locked("control.lock"):
        if load() is not None or service_active():
            raise LinkError("A rescue cable session already exists; stop it first")
        if interface not in {row["name"] for row in interfaces()}:
            raise LinkError("Select a physical, unenslaved Ethernet port")
        if Path("/run/netns", NETNS).exists():
            raise LinkError("The rescue network namespace is already owned")
        state = snapshot(interface)
        save(state)  # All subsequent side effects have a rollback record.
        try:
            private_auth(password)
            command(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(ROOT / "host_ed25519")])
            fingerprint = text(["ssh-keygen", "-l", "-E", "sha256", "-f", str(ROOT / "host_ed25519.pub")])
            state["fingerprint"] = fingerprint.split()[1]
            save(state)
            if state["nm_managed"]:
                state["detached"] = True
                save(state)
                command(["nmcli", "device", "set", interface, "autoconnect", "no"])
                command(["nmcli", "device", "set", interface, "managed", "no"])
            namespace_configure(state)
            write_bytes(ROOT / "sshd_config", ssh_config(state["address"], interface).encode())
            command(["ip", "netns", "exec", NETNS, "sshd", "-t", "-f", str(ROOT / "sshd_config")])
            state["state"] = "starting"
            save(state)
            command(["systemctl", "start", SERVICE], timeout=25)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if not service_active():
                    break
                if listening():
                    state["state"] = "active"
                    save(state)
                    return "Authenticated rescue cable enabled"
                time.sleep(0.1)
            raise LinkError("The rescue SSH service did not start listening")
        except Exception:
            command(["systemctl", "stop", SERVICE], check=False, timeout=25)
            cleanup()
            raise


def interface_by_identity(state):
    for path in Path("/sys/class/net").iterdir():
        device = path / "device"
        if device.exists() and str(device.resolve()) == state["device"]:
            return path.name
    return None


def cleanup():
    # This runs in the host netns, including ExecStopPost. Never hold the
    # control lock here: a stop request waits for ExecStopPost synchronously.
    with locked("cleanup.lock"):
        state = load()
        if state is None:
            return "No rescue cable session to clean up"
        state["state"] = "stopping"
        save(state)
        if Path("/run/netns", NETNS).exists() and state["namespace"]:
            pids = text(["ip", "netns", "pids", NETNS]).split()
            if pids:
                raise LinkError("Rescue processes remain; stop the service before cleanup")
            # Destroying an empty named netns returns physical devices to their
            # initial namespace. Do not depend on the caller's PID namespace.
            ip("netns", "delete", NETNS)
        # Namespace destruction returns physical devices asynchronously. Wait
        # for udev/sysfs rather than confusing that handoff with unplugging.
        deadline = time.monotonic() + 5
        interface = interface_by_identity(state)
        while interface is None and time.monotonic() < deadline:
            time.sleep(0.1)
            interface = interface_by_identity(state)
        if interface is None:
            raise LinkError("The selected rescue port disappeared; reconnect it and retry cleanup")
        if state["moved"]:
            ip("link", "set", "dev", interface, "down")
            ip("address", "flush", "dev", interface)
            ip("-6", "address", "flush", "dev", interface)
            ip("link", "set", "dev", interface, "address", state["mac"])
            state["moved"] = False
            save(state)
        if state["detached"]:
            if not nm_running():
                raise LinkError("NetworkManager is unavailable; retry cleanup after it returns")
            command(["nmcli", "device", "set", interface, "autoconnect", "no"])
            command(["nmcli", "device", "set", interface, "managed", "yes"])
            if state.get("nm_active") and state["nm_uuid"]:
                command(["nmcli", "--wait", "10", "connection", "up", "uuid", state["nm_uuid"],
                         "ifname", interface], timeout=15)
            command(["nmcli", "device", "set", interface, "autoconnect", state["nm_autoconnect"]])
            state["detached"] = False
            save(state)
        if state["up"] and not state.get("nm_active"):
            ip("link", "set", "dev", interface, "up")
        for path in ROOT.iterdir():
            if path.name in ("control.lock", "cleanup.lock", "state.json"):
                continue
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
        STATE.unlink()
        return "Rescue cable stopped; the selected port was restored"


def stop():
    with locked("control.lock"):
        command(["systemctl", "stop", SERVICE], timeout=25)
        if service_active():
            raise LinkError("The rescue SSH service is still active")
        return cleanup()


def serve():
    state = load()
    if not state or state["state"] not in ("starting", "active"):
        raise LinkError("No prepared rescue cable session")
    # Verify the unit really installed the private bind before accepting logins.
    if not os.path.samefile("/etc/shadow", ROOT / "auth/etc/shadow"):
        raise LinkError("The private authentication mount is missing")
    namespace = os.open("/run/netns/" + NETNS, os.O_RDONLY)
    try:
        if hasattr(os, "setns"):
            os.setns(namespace, 0)
        else:
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.setns(namespace, 0) != 0:
                raise LinkError("Cannot enter the rescue network namespace")
    finally:
        os.close(namespace)
    executable = shutil.which("sshd", path=ENV["PATH"])
    if not executable:
        raise LinkError("OpenSSH server is unavailable")
    os.execve(executable, [executable, "-D", "-e", "-f", str(ROOT / "sshd_config")], ENV)


def status():
    state = load()
    link = {"active": False, "interface": "", "address": "", "port": PORT,
            "fingerprint": "", "state": "off"}
    if state:
        link.update({key: state.get(key, "") for key in ("interface", "address", "fingerprint", "state")})
        link["active"] = state["state"] == "active" and service_active() and listening()
        if state["state"] == "active" and not link["active"]:
            link["state"] = "failed"
    return link


def main():
    output, error = "", ""
    try:
        if sys.platform != "linux" or os.geteuid() != 0:
            raise LinkError("The rescue cable helper requires Linux live root")
        os.umask(0o077)
        action = sys.argv[1] if len(sys.argv) > 1 else ""
        if action == "start" and len(sys.argv) == 3:
            password = sys.stdin.buffer.read(257)
            output = start(sys.argv[2], password)
        elif action == "stop" and len(sys.argv) == 2:
            output = stop()
        elif action == "cleanup" and len(sys.argv) == 2:
            output = cleanup()
        elif action == "serve" and len(sys.argv) == 2:
            serve()
        elif action in ("status", "interfaces") and len(sys.argv) == 2:
            pass
        else:
            raise LinkError("Usage: rescue-link.py status|interfaces|start INTERFACE|stop|serve|cleanup")
    except Exception as exc:
        error = str(exc) if isinstance(exc, LinkError) else "Rescue cable operation failed"
    try:
        link = status() if sys.platform == "linux" and os.geteuid() == 0 else {
            "active": False, "interface": "", "address": "", "port": PORT,
            "fingerprint": "", "state": "off"}
        ports = interfaces() if sys.platform == "linux" else []
    except Exception:
        link = {"active": False, "interface": "", "address": "", "port": PORT,
                "fingerprint": "", "state": "failed"}
        ports = []
        error = error or "Cannot read rescue cable state"
    print(json.dumps({"ok": not bool(error), "link": link, "interfaces": ports,
                      "output": output, "error": error}))
    return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())
