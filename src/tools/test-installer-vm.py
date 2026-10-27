#!/usr/bin/env python3
"""Boot the real installer ISO; optionally install and boot a private target.

smoke ISO NEW_TEST_DIRECTORY [--firmware bios|uefi]
install ISO NEW_TEST_DIRECTORY [--level x86-64-v2] [--timeout 14400]
tui ISO NEW_TEST_DIRECTORY        the full-screen installer, by its keys
tui-stops ISO NEW_TEST_DIRECTORY  the same with the Tor module, the network
                                  cut while the packages download (it stops,
                                  then goes on); both trust the source keys
                                  it stops for as a person does (y copies the
                                  command, t a terminal, pasted, y each)

No host block devices, privileged mounts, SSH service, guest test hook or
prebuilt target root are used. Only the test VM partitions its new disk.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("root_boot", SOURCE / "test-root-boot.py")
root_boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(root_boot)
PASSWORD = root_boot.PASSWORD  # Nonsecret fixture, entered only at guest passwd prompts.
PROMPT = "LOCACHY-VM# "


class Guest(root_boot.Guest):
    def key(self, *names):
        # Actual installed systems start services while the test types. Keep
        # each press/release separate even under cold-cache startup load.
        self.command("send-key", {"keys": [{"type": "qcode", "data": name} for name in names],
                                  "hold-time": 80})
        self.pump(0.16)

    def read_serial(self):
        super().read_serial()
        self.pending = self.pending[-2 * 1024 * 1024:]

    def send(self, text):
        self.serial.sendall(text.encode())
        self.pump(0.05)

    def any(self, pattern):
        expression = re.compile(pattern, re.I)
        while True:
            match = expression.search(self.pending)
            if match:
                self.pending = self.pending[match.end():]
                return match
            if not self.alive:
                raise RuntimeError("Guest ended before " + repr(pattern))
            self.pump(min(0.2, self.remaining()))

    def shell(self, command):
        marker = "LOCACHY-CMD-" + str(time.monotonic_ns())
        self.send(command + "; _rc=$?; printf '\\n" + marker + "-%s\\n' \"$_rc\"\n")
        result = self.any(rb"(?:\r?\n)" + marker.encode() + rb"-([0-9]+)\r?\n")
        if result.group(1) != b"0":
            raise RuntimeError("Guest command failed (" + result.group(1).decode() + "): " + command)


def tools():
    for tool in ("qemu-system-x86_64", "xorriso", "qemu-img"):
        if not shutil.which(tool):
            raise RuntimeError("Missing test tool: " + tool)


def iso_commands(iso, out):
    """Use exactly the built ISO's expanded boot entry (systemd-boot's, which
    archiso also puts in the ISO 9660 tree), plus serial logging: the kernel
    with its options, and its initrds (comma-separated, as Syslinux takes)."""
    config = out / "iso-loader-entry.conf"
    with (out / "iso-extract.log").open("w") as log:
        subprocess.run(["xorriso", "-osirrox", "on", "-indev", str(iso), "-extract",
                        "/loader/entries/01-archiso-linux.conf", str(config)], check=True, stdout=log, stderr=log)
    text = config.read_text()
    kernel = re.search(r"(?m)^\s*linux\s+(\S+)\s*$", text)
    initrds = re.findall(r"(?m)^\s*initrd\s+(\S+)\s*$", text)
    options = re.search(r"(?m)^\s*options\s+([^\n]+)", text)
    if not kernel or not initrds or not options or "%" in text:
        raise RuntimeError("The real ISO has no expanded Linux/initrd boot entry")
    if not re.fullmatch(r"/[A-Za-z0-9/_.-]+", kernel.group(1)):
        raise RuntimeError("Unexpected ISO kernel path")
    return kernel.group(1) + " " + options.group(1).strip(), ",".join(initrds)


def firmware_files(out):
    code = Path(os.environ.get("OVMF_CODE", "/usr/share/edk2/x64/OVMF_CODE.4m.fd"))
    template = Path(os.environ.get("OVMF_VARS", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"))
    if not code.is_file() or not template.is_file():
        raise RuntimeError("OVMF firmware is unavailable")
    variables = out / "ovmf-vars.fd"
    shutil.copyfile(template, variables)
    return ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + str(code),
            "-drive", "if=pflash,format=raw,unit=1,file=" + str(variables)]


def launch(out, seconds, *, iso=None, disk=None, firmware="uefi", smoke=False, network=False):
    out.mkdir()
    if smoke:
        acceleration = ["-accel", "tcg", "-cpu", "qemu64"]
    else:
        if not os.access("/dev/kvm", os.R_OK | os.W_OK):
            raise RuntimeError("KVM access is required for the target installation test")
        acceleration = ["-accel", "kvm", "-cpu", "host"]
    command = ["qemu-system-x86_64", "-machine", "q35"] + acceleration
    command += ["-m", "3072" if smoke else "8192", "-smp", "2" if smoke else "4",
                "-vga", "std", "-display", "none", "-monitor", "none", "-no-reboot"]
    if firmware == "uefi":
        command += firmware_files(out)
    if disk is not None:
        command += ["-drive", "if=none,id=target,format=raw,file=" + str(disk) +
                    (",snapshot=on" if iso is None else ""), "-device", "ide-hd,drive=target,bus=ide.0"]
    if iso is not None:
        command += ["-drive", "if=none,id=iso,media=cdrom,readonly=on,format=raw,file=" + str(iso),
                    "-device", "ide-cd,drive=iso,bus=ide.1", "-boot", "order=d,menu=on"]
    # The network card as nic0: QMP set_link takes it away and back.
    command += ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0,id=nic0"] if network else ["-nic", "none"]
    command += ["-chardev", "socket,id=com1,path=" + str(out / "com1.sock") + ",server=on,wait=on",
                "-serial", "chardev:com1", "-qmp", "unix:" + str(out / "qmp.sock") + ",server=on,wait=off"]
    (out / "qemu-command.json").write_text(json.dumps(command, indent=2) + "\n")
    error = (out / "qemu.err").open("wb")
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
    try:
        guest = Guest(out, seconds)
    except BaseException:
        process.terminate()
        process.wait(timeout=10)
        error.close()
        raise
    return process, guest, error


def end_vm(process, guest, error, *, completed=False):
    try:
        if completed:
            process.wait(timeout=30)
            if process.returncode:
                raise RuntimeError("QEMU exit status " + str(process.returncode))
        elif process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
    finally:
        guest.close()
        error.close()


def boot_live(guest, firmware, kernel, initrd):
    # The ISO's own boot menus (systemd-boot for UEFI, Syslinux for BIOS), not
    # QEMU -kernel or a rebuilt test initramfs: serial logging added to them.
    if firmware == "uefi":
        # The menu writes to serial too, but its input focus can remain on
        # the firmware console. Use the emulated keyboard, not COM1 input:
        # e edits the entry's options (systemd-boot), Enter boots them.
        guest.any(rb"Arch Linux|LOCachy")
        guest.pump(0.3)
        guest.key("e")
        guest.pump(0.3)
        guest.key("end")
        guest.type(" console=ttyS0,115200")
        guest.pending = b""
        guest.key("ret")
    else:
        guest.any(rb"Arch Linux|LOCachy")
        guest.key("esc")
        guest.any(rb"boot:")
        guest.pending = b""
        guest.type(kernel + " initrd=" + initrd + " console=ttyS0,115200")
        guest.key("ret")
    match = guest.any(rb"(?:login: ?|root@[^\r\n]*[#] ?)")
    if b"login:" in match.group(0).lower():
        guest.send("root\n")
        guest.any(rb"root@[^\r\n]*[#] ?")
    guest.shell("export HISTFILE=/dev/null; export PS1='" + PROMPT + "'; stty -echo")


def smoke_test(args, iso, out, kernel, initrd):
    process, guest, error = launch(out / "v1-smoke", args.timeout, iso=iso,
                                   firmware=args.firmware, smoke=True)
    completed = False
    try:
        boot_live(guest, args.firmware, kernel, initrd)
        guest.shell("pacman -Q linux && uname -r && getconf GNU_LIBC_VERSION && pacman -V && python3 -I -c 'import json,ssl,subprocess; print(\"v1-python-ok\")'")
        guest.shell("! /usr/lib/ld-linux-x86-64.so.2 --help | grep -q 'x86-64-v2 (supported' && ! grep -Eq '^\\[(cachyos|.*x86-64-v)' /etc/pacman.conf")
        guest.shell("printf '#include <stdio.h>\\nint main(void){puts(\"v1-gcc-ok\");return 0;}\\n' >/tmp/v1.c && gcc -march=x86-64 -O2 /tmp/v1.c -o /tmp/v1-test && /tmp/v1-test")
        guest.shell("locachy-install --help >/dev/null && echo v1-installer-entry-ok")
        guest.shell("printf 'LOCACHY-V1-SMOKE-PASSED\\n'")
        guest.send("poweroff\n")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
        completed = True
    finally:
        end_vm(process, guest, error, completed=completed)
    (out / "result.json").write_text(json.dumps({"passed": True, "mode": "smoke", "cpu": "qemu64",
                                                 "firmware": args.firmware, "iso": str(iso)}, indent=2) + "\n")
    print("PASS v1: real ISO, standard kernel, glibc/pacman/Python/GCC and installer entry on qemu64")


def install_guest(args, iso, out, kernel, initrd):
    disk = out / "target.img"
    subprocess.run(["qemu-img", "create", "-f", "raw", str(disk), "32G"], check=True)
    process, guest, error = launch(out / "install", args.timeout, iso=iso, disk=disk, network=True)
    completed = False
    try:
        boot_live(guest, "uefi", kernel, initrd)
        guest.shell("test -b /dev/sda && test \"$(lsblk -dn -o TYPE /dev/sda)\" = disk && ! findmnt -rn -S /dev/sda && ! findmnt -rn -S /dev/sda1 && ! findmnt -rn -S /dev/sda2")
        guest.shell("printf 'label: gpt\\nsize=512M,type=uefi\\ntype=linux\\n' | sfdisk /dev/sda && udevadm settle --timeout=20")
        guest.shell("mkfs.fat -F 32 /dev/sda1 && mkfs.ext4 -F -L locachy-root /dev/sda2 && mount /dev/sda2 /mnt && mkdir /mnt/efi && mount /dev/sda1 /mnt/efi")
        invocation = "locachy-install --root /mnt --esp /mnt/efi --level " + args.level + " --no-ime --user boot-test --yes"
        guest.send(invocation + "; _rc=$?; printf '\\nLOCACHY-INSTALL-RESULT-%s\\n' \"$_rc\"\n")
        prompt = guest.any(rb"(?:New password:|LOCACHY-INSTALL-RESULT-([0-9]+))")
        if prompt.group(1) is not None:
            raise RuntimeError("Guest installer exited before password setup: " + prompt.group(1).decode())
        guest.send(PASSWORD + "\n")
        guest.any(rb"Retype new password:")
        guest.send(PASSWORD + "\n")
        result = guest.any(rb"(?:\r?\n)LOCACHY-INSTALL-RESULT-([0-9]+)\r?\n")
        if result.group(1) != b"0":
            raise RuntimeError("Guest installer exit status " + result.group(1).decode())
        guest.shell("test -s /mnt/var/lib/locachy/install.json && test -s /mnt/efi/EFI/BOOT/BOOTX64.EFI && arch-chroot /mnt /usr/local/bin/locachy-pkg status && ! grep -q '^service_link=' /mnt/etc/locachy/rc.conf && ! grep -q '^login_skip=' /mnt/etc/locachy/rc.conf")
        # Own builds for this CPU's model (gcc -march=native here: -cpu host).
        guest.shell("cpu=$(python3 -c 'import json; print(json.load(open(\"/mnt/var/lib/locachy/install.json\"))[\"cpu\"])') && "
                    "test \"$cpu\" != " + args.level + " && grep -qx \"$cpu\" /mnt/usr/local/lib/locachy/cpu && "
                    "grep -q -- \"-march=$cpu'\" /mnt/etc/makepkg.conf.d/99-locachy-cpu.conf && cat /mnt/usr/local/lib/locachy/build.json && cat /mnt/var/lib/locachy/install.json && test -s /mnt/var/log/locachy/install-build.log && echo target-local-build-evidence-ok")
        # This private test disk keeps the production PAM/lock/home, only serial
        # verbosity changes to make the second, ISO-free boot observable: the
        # Boot rows of settings (rc.conf), made into the image by locachy-boot.
        guest.shell("printf 'boot_verbose=\"1\"\\nboot_cmdline=\"console=ttyS0\"\\n' >>/mnt/etc/locachy/rc.conf && arch-chroot /mnt /usr/local/bin/locachy-boot | grep -q 'console=ttyS0' && cmp /mnt/efi/EFI/BOOT/BOOTX64.EFI /mnt/efi/EFI/locachy/locachy.efi; sync; umount /mnt/efi && umount /mnt")
        guest.shell("printf 'LOCACHY-GUEST-INSTALL-PASSED\\n'")
        guest.send("poweroff\n")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
        completed = True
    finally:
        end_vm(process, guest, error, completed=completed)
    return disk


def wait_kmsg(guest, pattern, seconds, fail=None):
    """Until the guest's kernel log has pattern (grep -E), read over the
    serial shell; fail: a pattern that ends the wait as a failure."""
    stop = "" if fail is None else "dmesg | grep -qE '" + fail + "' && exit 3; "
    guest.shell("timeout " + str(seconds) + " sh -c \"until dmesg | grep -qE '" + pattern + "'; do " + stop +
                "sleep 2; done\"")


def guest_state(guest, command):
    """What command (shell, sets s) found, through the serial shell."""
    guest.send(command + "; printf '\\nLOCACHY-STATE-%s\\n' \"$s\"\n")
    return guest.any(rb"(?:\r?\n)LOCACHY-STATE-([a-z0-9]+)\r?\n").group(1).decode()


def kmsg_count(pattern):
    return "$(dmesg | grep -cE '" + pattern + "')"


def trust_keys(guest, screenshots, round_):
    """As a person: y copies the command the page shows, t opens the
    terminal, the command pasted (Ctrl+Shift+V) and run, y to each key it
    shows; then the terminal closed."""
    target = "/run/locachy-install-target"
    keys = int(guest_state(guest, "s=$(awk '{print $2}' " + target + "/var/lib/locachy/builder/untrusted-keys | sort -u | wc -l)"))
    guest.shell("cat " + target + "/var/lib/locachy/builder/untrusted-keys")
    terms = int(guest_state(guest, "s=" + kmsg_count("installer: running /usr/local/bin/term")))
    guest.key("y")
    guest.key("t")
    guest.shell("timeout 30 sh -c \"until [ " + kmsg_count("installer: running /usr/local/bin/term") + " -gt " +
                str(terms) + " ]; do sleep 1; done\"")
    guest.pump(3)
    guest.key("ctrl", "shift", "v")
    guest.pump(1)
    screenshots[f"trust-{round_}"] = guest.shot(f"tui-trust-{round_}")
    guest.key("ret")
    for _ in range(keys):
        guest.pump(2)
        guest.type("y")
        guest.key("ret")
    guest.shell("timeout 600 sh -c 'while [ -s " + target + "/var/lib/locachy/builder/untrusted-keys ]; do sleep 2; done'")
    guest.pump(2)
    screenshots[f"trusted-{round_}"] = guest.shot(f"tui-trusted-{round_}")
    guest.type("exit")
    guest.key("ret")
    guest.shell("timeout 30 sh -c \"until [ " + kmsg_count("installer: /usr/local/bin/term ended") + " -gt " +
                str(terms) + " ]; do sleep 1; done\"")
    guest.pump(2)


def install_tui(args, iso, out, kernel, initrd, stops=False):
    """The full-screen installer on tty1, driven by its keys alone: the
    terminal (and back), then the whole disk, a passphrase and a user.
    Where it stops it is continued (c): for source signing keys, trusted as
    a person does (trust_keys); stops: with the Tor module (its own key),
    and the network cut while the packages download."""
    disk = out / "target.img"
    subprocess.run(["qemu-img", "create", "-f", "raw", str(disk), "32G"], check=True)
    process, guest, error = launch(out / "install", args.timeout, iso=iso, disk=disk, network=True)
    completed = False
    screenshots = {}
    try:
        boot_live(guest, "uefi", kernel, initrd)
        guest.shell("locale -a | grep -qi '^zh_CN.utf8$' && test -s /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc")
        wait_kmsg(guest, "installer: started", 120)
        guest.pump(15)  # its description of this computer (locachy-install --describe-json)
        screenshots["start"] = guest.shot("tui-start")
        guest.key("down")
        guest.key("ret")
        wait_kmsg(guest, "installer: running /usr/local/bin/term", 30)
        guest.pump(3)
        screenshots["terminal"] = guest.shot("tui-terminal")
        guest.type("echo live-rescue-ok >/dev/kmsg; exit")
        guest.key("ret")
        wait_kmsg(guest, "installer: /usr/local/bin/term ended 0", 30)
        guest.shell("dmesg | grep -q live-rescue-ok")
        guest.pump(10)  # the description again
        guest.key("up")
        guest.key("ret")                                  # Install LOCachy
        guest.key("end")
        guest.key("ret")                                  # Network: next
        guest.key("down")
        guest.key("ret")                                  # Disk: the first one (sda)
        screenshots["disk"] = guest.shot("tui-disk")
        guest.key("end")
        guest.key("ret")
        guest.key("down")
        guest.key("ret")                                  # Encryption: the passphrase, twice
        guest.type(PASSWORD)
        guest.key("ret")
        guest.type(PASSWORD)
        guest.key("ret")
        guest.key("end")
        guest.key("ret")
        guest.key("ret")                                  # User: its name
        guest.key("ctrl", "u")
        guest.type("boot-test")
        guest.key("ret")
        guest.key("down")
        guest.key("down")
        guest.key("ret")                                  # its password, twice
        guest.type(PASSWORD)
        guest.key("ret")
        guest.type(PASSWORD)
        guest.key("ret")
        for _ in range(2):                                # Region: as it is
            guest.key("end")
            guest.key("ret")
        if stops:                                         # Software: the Tor module too
            modules = list(json.loads((SOURCE.parent / "link/modules.json").read_text()))
            guest.key("home")
            for _ in range(modules.index("tor")):
                guest.key("down")
            guest.key("ret")
            screenshots["software"] = guest.shot("tui-software")
        for _ in range(2):                                # Software, System: next
            guest.key("end")
            guest.key("ret")
        guest.pump(1)
        screenshots["summary"] = guest.shot("tui-summary")
        guest.key("end")
        guest.key("ret")                                  # Install: --check, then the question
        guest.pump(15)
        screenshots["confirm"] = guest.shot("tui-confirm")
        guest.key("y")
        wait_kmsg(guest, "installer: installing", 60)
        guest.pump(60)
        screenshots["progress"] = guest.shot("tui-progress")
        target = "/run/locachy-install-target"
        stopped, retried, rounds = 0, 0, []
        if stops:
            # The network gone while the packages download: it stops; back,
            # and it goes on from there.
            guest.shell("timeout 3600 sh -c \"until grep -q ') downloading ' /run/locachy-installer/install.log; do sleep 1; done\"")
            guest.command("set_link", {"name": "nic0", "up": False})
            wait_kmsg(guest, "installer: installation failed", 3600)
            stopped += 1
            guest.pump(2)
            screenshots["stopped-network"] = guest.shot("tui-stopped-network")
            guest.command("set_link", {"name": "nic0", "up": True})
            guest.shell("nm-online -q -t 180")
            guest.key("c")
            guest.shell("timeout 30 sh -c \"until dmesg | grep -q 'installer: continuing'; do sleep 1; done\"")
            rounds.append("network")
        while True:
            state = guest_state(guest, "until dmesg | grep -q 'installer: installation completed' || [ " +
                                kmsg_count("installer: installation failed") + " -gt " + str(stopped) + " ]; do sleep 5; done; "
                                "if dmesg | grep -q 'installer: installation completed'; then s=done; "
                                "elif [ -s " + target + "/var/lib/locachy/builder/untrusted-keys ]; then s=keys; else s=failed; fi")
            if state == "done":
                break
            stopped += 1
            guest.pump(2)
            screenshots[f"stopped-{stopped}"] = guest.shot(f"tui-stopped-{stopped}")
            # Why, from its log (the guest's /run goes with it), into serial.log.
            guest.shell("tail -n 40 /run/locachy-installer/install.log; true")
            if state == "keys":
                trust_keys(guest, screenshots, stopped)
                rounds.append("keys")
            else:
                retried += 1
                if retried > 2:
                    raise RuntimeError("The installation failed, and again when continued")
                rounds.append("failed")
            continuing = int(guest_state(guest, "s=" + kmsg_count("installer: continuing")))
            guest.key("c")
            guest.shell("timeout 30 sh -c \"until [ " + kmsg_count("installer: continuing") + " -gt " +
                        str(continuing) + " ]; do sleep 1; done\"")
        print("stops: " + (", ".join(rounds) or "none"), flush=True)
        if stops and "keys" not in rounds:
            raise RuntimeError("The installation never stopped for a source signing key")
        guest.pump(2)
        screenshots["done"] = guest.shot("tui-done")
        # The live entry fetched the newest source, and that one is installed.
        guest.shell("grep -m1 '^Source' /run/locachy-installer/install.log && "
                    "grep -q '^Source: .*, the newest, fetched now$' /run/locachy-installer/install.log && "
                    "test \"$(git -C /run/locachy-source rev-parse HEAD)\" = \"$(cat " + target +
                    "/usr/local/lib/locachy/source-commit)\"")
        guest.shell("test -s " + target + "/var/lib/locachy/install.json && grep -q '\"encrypted\": true' " + target +
                    "/var/lib/locachy/install.json && test -s " + target + "/efi/EFI/BOOT/BOOTX64.EFI && "
                    "test ! -e /run/locachy-installer/passphrase && test ! -e /run/locachy-installer/password && "
                    "cryptsetup isLuks /dev/sda3 && df -h " + target + " && du -sh " + target + "/var/cache/pacman/pkg")
        guest.shell("printf 'boot_verbose=\"1\"\\nboot_cmdline=\"console=ttyS0\"\\n' >>" + target + "/etc/locachy/rc.conf && "
                    "arch-chroot " + target + " /usr/local/bin/locachy-boot | grep -q 'console=ttyS0' && cmp " + target +
                    "/efi/EFI/BOOT/BOOTX64.EFI " + target + "/efi/EFI/locachy/locachy.efi; sync; umount " + target +
                    "/efi && umount " + target)
        guest.shell("printf 'LOCACHY-GUEST-INSTALL-PASSED\\n'")
        guest.send("poweroff\n")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
        completed = True
    finally:
        end_vm(process, guest, error, completed=completed)
    return disk, screenshots


def target_boot(args, disk, out, passphrase=None, devices=rb"root: /dev/sda2, esp: /dev/sda1"):
    process, guest, error = launch(out / "installed-boot", 300, disk=disk)
    completed = False
    screenshots = {}
    try:
        if passphrase:
            # The initramfs' prompt: white on black, too few colours for shot().
            guest.wait_for(rb"init: waiting for LUKS2 passphrase")
            guest.pump(3)
            guest.type(passphrase)
            guest.key("ret")
            guest.wait_for(rb"init: unlocked LUKS2")
        else:
            guest.wait_for(rb"init: locking")
            guest.wait_for(rb"init: started lock \(pid [0-9]+\)")
            guest.pump(5)
            screenshots["locked"] = guest.shot("locked")
            guest.type(PASSWORD)
            guest.key("ret")
            guest.wait_for(rb"init: unlocked")
        home = guest.any(rb"home: ([0-9]+) containers, [0-9]+ host programs")
        napps = int(home.group(1))
        guest.wait_for(rb"home> ")
        if passphrase:
            # The passphrase typed at power-on was the boot's unlock (init:
            # no lock after it); the lock and its PAM password: Super+L.
            guest.pump(3)
            guest.key("meta_l", "l")
            guest.wait_for(rb"init: locking")
            guest.wait_for(rb"init: started lock \(pid [0-9]+\)")
            guest.pump(5)
            screenshots["locked"] = guest.shot("locked")
            guest.type(PASSWORD)
            guest.key("ret")
            guest.wait_for(rb"init: unlocked")
            guest.wait_for(rb"home> ")
        screenshots["home"] = guest.shot("home")
        # Production home lists configured applications (including host
        # code/foot entries) before files and rc.conf's first program, term.
        guest.key("home")
        for _ in range(napps + 1):
            guest.key("down")
        guest.key("ret")
        # term: the user's shell (uid 1000), its exit status term's.
        guest.wait_for(rb"term: started /usr/local/lib/locachy/user-shell")
        guest.wait_for(rb"term: shown 1")
        guest.pump(0.5)
        screenshots["term"] = guest.shot("term")
        guest.type('test "$(id -u)" = 1000 && exit 7')
        guest.key("ret")
        guest.wait_for(rb"home: ended /usr/local/bin/term /usr/local/lib/locachy/user-shell 7")
        guest.wait_for(rb"home> ")
        # admin, the next program: root's shell.
        guest.key("home")
        for _ in range(napps + 2):
            guest.key("down")
        guest.key("ret")
        guest.wait_for(rb"term: started /bin/bash")
        guest.wait_for(rb"term: shown 1")
        guest.pump(0.5)
        guest.type("echo installer-target-terminal-ok >/dev/kmsg")
        guest.key("ret")
        guest.wait_for(rb"installer-target-terminal-ok")
        guest.type("exit")
        guest.key("ret")
        guest.wait_for(rb"home: ended /usr/local/bin/term 0")
        guest.wait_for(rb"home> ")
        screenshots["home-returned"] = guest.shot("home-returned")
        guest.key("end")
        guest.key("ret")
        guest.key("y")
        guest.wait_for(rb"init: power off")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
        log = (out / "installed-boot/serial.log").read_bytes()
        if not re.search(devices, log):
            raise RuntimeError("Installed root/ESP were not the test target disk")
        if re.search(rb"Kernel panic|screen: none|cannot switch to the root|memory BAD", log):
            raise RuntimeError("Installed guest reported a boot failure")
        completed = True
    finally:
        end_vm(process, guest, error, completed=completed)
    return screenshots


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("smoke", "install", "tui", "tui-stops"))
    parser.add_argument("iso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="uefi")
    parser.add_argument("--level", choices=("x86-64", "x86-64-v2", "x86-64-v3", "x86-64-v4"), default="x86-64-v2")
    parser.add_argument("--timeout", type=int)
    args = parser.parse_args()
    args.timeout = args.timeout or (600 if args.mode == "smoke" else 14400)
    try:
        tools()
        iso = args.iso.resolve(strict=True)
        out = args.output.absolute()
        if not iso.is_file() or any(char in str(iso) + str(out) for char in (",", "\n")):
            raise ValueError("ISO must be a regular file and QEMU paths cannot contain commas/newlines")
        if out.exists() or out.is_symlink():
            raise ValueError("Refusing an existing test directory: " + str(out))
        if args.mode != "smoke" and args.firmware != "uefi":
            raise ValueError("The target installer currently commits a UEFI ESP; use --firmware uefi")
        out.mkdir(parents=True)
        kernel, initrd = iso_commands(iso, out)
        if args.mode == "smoke":
            smoke_test(args, iso, out, kernel, initrd)
        elif args.mode in ("tui", "tui-stops"):
            disk, screenshots = install_tui(args, iso, out, kernel, initrd, stops=args.mode == "tui-stops")
            # The whole disk: ESP, swap, root, both encrypted (locachy-install --disk).
            screenshots.update(target_boot(args, disk, out, PASSWORD, rb"esp: /dev/sda1"))
            (out / "result.json").write_text(json.dumps({"passed": True, "mode": args.mode, "iso": str(iso), "disk": str(disk),
                                                         "screenshots": screenshots}, indent=2) + "\n")
            print("PASS full-screen installer: terminal and back, whole encrypted disk, target PAM/home/terminal")
        else:
            disk = install_guest(args, iso, out, kernel, initrd)
            screenshots = target_boot(args, disk, out)
            (out / "result.json").write_text(json.dumps({"passed": True, "mode": "install", "cpu_level": args.level,
                                                         "iso": str(iso), "disk": str(disk), "screenshots": screenshots}, indent=2) + "\n")
            print("PASS installation: guest-local build, ISO removed, target PAM/home/terminal and clean power-off")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("FAIL installer VM: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
