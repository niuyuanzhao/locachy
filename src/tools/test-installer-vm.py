#!/usr/bin/env python3
"""Boot the real installer ISO; optionally install and diagnose a private target.

smoke ISO NEW_TEST_DIRECTORY [--firmware bios|uefi]
install ISO NEW_TEST_DIRECTORY [--level x86-64-v2] [--timeout 14400]
tui ISO NEW_TEST_DIRECTORY        the full-screen installer, by its keys
tui-stops ISO NEW_TEST_DIRECTORY  the same with the network cut during
                                  package download; verify that c resumes
                                  on the same partitions and filesystems

No mode approves source signing keys. An unresolved key stops the test.
Installation modes check the original UKI copies, then rebuild a diagnostic
UKI for serial observation. They do not prove the original UKI boots.

No host block devices, privileged mounts, SSH service, guest test hook or
prebuilt target root are used. Only the test VM partitions its new disk.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
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

    def shell(self, command, *, label=None, output=False):
        marker = "LOCACHY-CMD-" + str(time.monotonic_ns())
        start = f"printf '\\n{marker}-BEGIN\\n'; " if output else ""
        self.send(start + command + "; _rc=$?; printf '\\n" + marker + "-%s\\n' \"$_rc\"\n")
        prefix = rb"(?:\r?\n)" + marker.encode()
        body = prefix + rb"-BEGIN\r?\n([\s\S]*?)" if output else b""
        result = self.any(body + prefix + rb"-([0-9]+)\r?\n")
        status = result.group(2 if output else 1)
        if status != b"0":
            raise RuntimeError(f"Guest command failed ({status.decode()}): {label or command}")
        if output:
            return result.group(1).decode(errors="replace").replace("\r", "")


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
    process = None
    try:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
        guest = Guest(out, seconds)
    except BaseException:
        try:
            if process is not None:
                stop_qemu(process)
        finally:
            error.close()
        raise
    return process, guest, error


def stop_qemu(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def end_vm(process, guest, error, *, completed=False):
    try:
        if completed:
            process.wait(timeout=30)
            if process.returncode:
                raise RuntimeError("QEMU exit status " + str(process.returncode))
    finally:
        try:
            stop_qemu(process)
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
        # printk.devkmsg=on, as LOCachy boots: the installer's lines in the
        # kernel log (what the test follows) are not dropped in a burst.
        guest.type(" console=ttyS0,115200 printk.devkmsg=on")
        guest.pending = b""
        guest.key("ret")
    else:
        guest.any(rb"Arch Linux|LOCachy")
        guest.key("esc")
        guest.any(rb"boot:")
        guest.pending = b""
        guest.type(kernel + " initrd=" + initrd + " console=ttyS0,115200 printk.devkmsg=on")
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


DIAGNOSTIC_BOOT = "diagnostic-boot"
BOOT_EVIDENCE = {"boot_image": "diagnostic-serial-uki", "original_uki_booted": False,
                 "installer_nvram_tested": False}


def prepare_diagnostic_boot(guest, target):
    """Check the installer's original files, then build a serial-only variant."""
    root = shlex.quote(target)
    guest.shell(
        f"root={root} && esp=\"$root/efi/EFI\" && lib=\"$root/usr/local/lib/locachy/boot/locachy.efi\" && "
        'fallback="$esp/BOOT/BOOTX64.EFI" && uki="$esp/locachy/locachy.efi" && '
        'findmnt -rn --mountpoint "$root/efi" -t vfat && test -s "$fallback" && '
        'cmp "$fallback" "$uki" && cmp "$fallback" "$lib" && original=$(sha256sum <"$fallback") && '
        'rc="$root/etc/locachy/rc.conf" && cp -p "$rc" /run/installer-original-rc.conf && '
        "printf '\\nboot_verbose=\"1\"\\nboot_cmdline=\"console=ttyS0\"\\n' >>\"$rc\" && "
        '(arch-chroot "$root" /usr/local/bin/locachy-boot >/run/installer-diagnostic-boot.log 2>&1 || '
        '{ status=$?; tail -n 60 /run/installer-diagnostic-boot.log; exit "$status"; }) && '
        'grep -F "command line:" /run/installer-diagnostic-boot.log | grep -F "console=ttyS0" && '
        'cmp "$fallback" "$uki" && cmp "$fallback" "$lib" && '
        'saved=$(sha256sum <"$esp/locachy/locachy-prev.efi") && test "$saved" = "$original" && '
        'diagnostic=$(sha256sum <"$fallback") && test "$diagnostic" != "$original" && '
        'cp -p /run/installer-original-rc.conf "$rc" && '
        'printf "Original UKI (preserved as locachy-prev.efi): %s\\nDiagnostic serial UKI: %s\\n" '
        '"${original%% *}" "${diagnostic%% *}" && sync && umount "$root/efi" && umount "$root"',
        label="check original UKI and prepare diagnostic serial UKI")


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
        prepare_diagnostic_boot(guest, "/mnt")
        guest.shell("printf 'LOCACHY-GUEST-INSTALL-PASSED\\n'")
        guest.send("poweroff\n")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
        completed = True
    finally:
        end_vm(process, guest, error, completed=completed)
    return disk


# The kernel log as the journal keeps it: dmesg's ring drops early lines
# during an installation, and counts taken from it go down. (Its short form:
# the journal takes "installer:" for the sender, which -o cat leaves out.)
KLOG = "journalctl -k -o short --no-pager"
TUI_RUN = "/run/locachy-installer"
TUI_LOG = TUI_RUN + "/install.log"
REFRESH_WAIT = 1260  # Two 300 s clones and a 600 s live build can run in sequence.
INSTALL_DONE = "installer: installation completed"
INSTALL_FAILED = "installer: installation failed"


# Read the install backend's actual exec environment, not the frontend's
# /proc environ (setenv after a same-binary refresh isn't reflected there).
# The selection record also identifies the exact bundled bytes at the same HEAD.
TUI_PROVENANCE = r"""
from hashlib import sha256
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import json
import os
from pathlib import Path
import re
import sys
import time

run, action, target, slots, origin, proc = map(Path, sys.argv[1:])


def require(ok, what):
    if not ok:
        sys.exit('Installer provenance: ' + what)


def backend(source):
    spec = spec_from_loader('install_backend', SourceFileLoader('install_backend', str(source / 'link/locachy-install')))
    impl = module_from_spec(spec)
    spec.loader.exec_module(impl)
    return impl


def catalog_identity(impl, path):
    meta = impl.APPS.read_meta(path)
    require(re.fullmatch('[0-9a-f]{40}', meta.get('commit', '')), 'catalog has no full commit: ' + str(path))
    meta.pop('installed', None)  # The copy sets its own installation timestamp.
    presets = impl.APPS.read_catalog(path)
    return {'meta': meta, 'presets': {name: sha256(Path(p).read_bytes()).hexdigest() for name, p in presets.items()}}


def stable_source(impl, path):
    # make-root.sh rewrites these two on the target. Their original bytes are
    # included in the full source stage fingerprint checked below.
    return impl.INSTALL_STATE.files_digest(path, set(impl.INSTALL_STATE.SOURCE_INPUTS) - {'rc.conf', 'packages'})


def observe(resume):
    for _ in range(60):
        for pid in proc.glob('[0-9]*/cmdline'):
            try:
                args = pid.read_bytes().split(b'\0')
                if len(args) < 4 or not args[2].endswith(b'/link/locachy-install') or b'--private' in args:
                    continue
                if b'--yes' not in args or (b'--resume' in args) != resume:
                    continue
                if not resume and not any(a.startswith(b'--disk=') for a in args):
                    continue
                env = dict(e.split(b'=', 1) for e in pid.with_name('environ').read_bytes().split(b'\0') if b'=' in e)
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
            if b'LOCACHY_INSTALL_SNAPSHOT' in env:
                return os.fsdecode(args[2]), {os.fsdecode(k): os.fsdecode(v) for k, v in env.items()}
        time.sleep(0.25)
    sys.exit('Installer provenance: did not observe the selected install backend')


def capture(resume):
    entry, env = observe(resume)
    token = env['LOCACHY_INSTALL_SNAPSHOT']
    require(re.fullmatch('[0-9a-f]{64}', token), 'invalid live selection token')
    raw = (slots / (token + '.json')).read_bytes()
    require(sha256(raw).hexdigest() == token, 'selection record hash does not match')
    options, source, catalog, source_how, apps_how = json.loads(raw)
    source, catalog = Path(source), Path(catalog)
    require(all(p.is_relative_to(slots) and p.resolve(strict=True) == p for p in (source, catalog)),
            'selection did not pin both input paths')
    require(Path(entry) == source / 'link/locachy-install' and Path(env['LOCACHY_APPS_CATALOG']) == catalog and
            env['LOCACHY_SOURCE'] == source_how and env['LOCACHY_APPS'] == apps_how,
            'backend inputs do not match the selection record')
    impl = backend(source)
    repo, branch, commit = impl.detect_source_git(source, origin)
    require(re.fullmatch('[0-9a-f]{40}', commit), 'source has no full commit: ' + str(source))
    if source_how == 'bundled: local changes over the fetched commit':
        medium = json.loads(origin.read_text())
        require(medium.get('source_changed') is True and options[:2] == [repo, branch] and
                medium.get('commit') == commit and source.name.startswith('bundled-source.'),
                'local changes did not come from the selected medium snapshot')
    state = impl.INSTALL_STATE
    data = {'token': token, 'source': str(source), 'commit': commit, 'source_how': source_how,
            'source_tree': state.files_digest(source, state.SOURCE_INPUTS), 'stable_source': stable_source(impl, source),
            'catalog': str(catalog), 'catalog_how': apps_how, 'catalog_identity': catalog_identity(impl, catalog)}
    (run / ('test-selection-resume.json' if resume else 'test-selection.json')).write_text(json.dumps(data))
    print(f'Selected installer {token}: source {commit} ({source_how}); '
          f'app presets {data["catalog_identity"]["meta"]["commit"]} ({apps_how})', flush=True)


def verify(resume):
    first = json.loads((run / 'test-selection.json').read_text())
    last = json.loads((run / 'test-selection-resume.json').read_text()) if resume else first
    impl = backend(Path(last['source']))
    require((target / 'usr/local/lib/locachy/source-commit').read_text().strip() == last['commit'],
            'target source commit differs from the selected backend')
    require(stable_source(impl, target / 'usr/local/src/locachy') == last['stable_source'],
            'installed source bytes differ from the selected backend')
    installed_catalog = target / 'var/lib/locachy/apps/catalog'
    require(catalog_identity(impl, installed_catalog) == first['catalog_identity'],
            'target app presets differ from the initial pinned catalog')

    # A resume can use a refreshed source, but uses the first run's catalog.
    # Check the completed source stage against the original (pre-rewrite) tree.
    os.environ['LOCACHY_APPS_CATALOG'] = first['catalog']
    config = json.loads((target / 'var/lib/locachy/install.json').read_text())
    require(not config['extra_volumes'], 'unexpected extra volumes in the TUI fixture')
    extra = list(dict.fromkeys(['cryptsetup', *impl.host_preset_packages(config['containers'])]))
    os.environ.update(LOCACHY_INSTALLER='1', WITH_IME='0' if config['ime'] == 'off' else config['ime'],
                      WITH_FEATURES=','.join(config['features']), WITH_EXTRA_PACKAGES=','.join(extra),
                      WITH_REPOS=','.join(config['repos']))
    expected = impl.INSTALL_STATE.build_input(last['source'], target, 'sources', config['cpu_level'], config['cpu'])
    require(impl.INSTALL_STATE.files_digest(last['source'], impl.INSTALL_STATE.SOURCE_INPUTS) == last['source_tree'],
            'selected source tree changed during installation')
    stage = json.loads((target / '.locachy-installer/stages.json').read_text())['stages']['sources']
    require(stage['success'] is True and stage['input'] == expected,
            'completed source stage does not match the exact selected source tree')
    print(f'PASS target matches source selection {last["token"]} and catalog selection {first["token"]}', flush=True)


{'capture': lambda: capture(False), 'capture-resume': lambda: capture(True),
 'verify': lambda: verify(False), 'verify-resume': lambda: verify(True)}[str(action)]()
"""


def tui_provenance(guest, target, action):
    # Bash's serial console has a per-line size limit, so send Python via a
    # here-doc rather than as one large quoted argument on a physical line.
    limit = min(60, int(guest.remaining()) - 8)
    if limit <= 0:
        raise RuntimeError(f"Guest acceptance timed out: installer provenance {action}")
    command = ["timeout", "-k", "2", str(limit), "python3", "-IB", "-", TUI_RUN, action, target,
               "/run/locachy-source.snapshots", "/etc/locachy/source-origin.json", "/proc"]
    guest.shell("{ " + shlex.join(command) + " <<'LOCACHY_PROVENANCE'\n" + TUI_PROVENANCE +
                "LOCACHY_PROVENANCE\n}", label="installer provenance " + action)


def tui_wait(guest, seconds, check, label):
    # Leave time to read the logs after the guest-side wait returns.
    limit = min(seconds, int(guest.remaining()) - 8)
    if limit <= 0:
        raise RuntimeError(f"Guest acceptance timed out: {label}")
    guest.shell(shlex.join(["timeout", "-k", "2", str(limit), "sh", "-c", check]), label=label)


def wait_kmsg(guest, pattern, seconds, *, fail=None, after=None, label=None):
    """Test success and failure against the same journal snapshot."""
    def has(expression):
        return "printf '%s\\n' \"$log\" | grep -qE " + shlex.quote(expression)

    since = "" if after is None else ("log=$(printf '%s\\n' \"$log\" | sed -n " +
                                      shlex.quote(f"/{after}/,$p") + "); ")
    stop = "" if fail is None else has(fail) + " && exit 3; "
    check = ("while :; do log=$(" + KLOG + ") || exit 4; " + since + stop +
             has(pattern) + " && exit 0; sleep 2; done")
    tui_wait(guest, seconds, check, label or pattern)


def record_target_identity(guest, target, suffix):
    """Record the partitions, encrypted volumes and filesystems."""
    guest.shell(f"root={shlex.quote(target)} && "
                'rootdev=$(findmnt -nro SOURCE --mountpoint "$root") && '
                'espdev=$(findmnt -nro SOURCE --mountpoint "$root/efi") && '
                '(for dev in /dev/sda1 /dev/sda2 /dev/sda3; do '
                'blkid -s PARTUUID -o value "$dev" || exit 1; done; '
                'for dev in /dev/sda2 /dev/sda3 "$rootdev" "$espdev" /dev/mapper/locachy-swap-crypt; do '
                'blkid -p -s UUID -o value "$dev" || exit 1; done) '
                f"> /run/installer-target-{suffix} && "
                f'cp "$root/.locachy-installer/install-id" /run/installer-id-{suffix} && '
                f"grep -Eq '^[0-9a-f]{{32}}$' /run/installer-id-{suffix}",
                label=f"record target identity ({suffix})")


def verify_resumed_target(guest, target):
    record_target_identity(guest, target, "after")
    guest.shell("cmp /run/installer-target-before /run/installer-target-after && "
                "cmp /run/installer-id-before /run/installer-id-after && "
                f"cmp /run/installer-test-nonce {target}/.locachy-installer/test-nonce")


def wait_download(guest, target, seconds):
    # The pacman log is from the target's package transaction, not mirror
    # ranking, key preflight, or an old UI download line.
    pacman_log = shlex.quote(target + "/.locachy-installer/pacman.log")
    check = ("while :; do log=$(" + KLOG + ") || exit 4; "
             "printf '%s\\n' \"$log\" | grep -qE 'installer: installation (completed|failed)' && exit 3; "
             f"if [ -s {pacman_log} ] && grep -qE '^\\([0-9]+/[0-9]+\\) downloading ' {TUI_LOG}; then "
             f"grep -qE '^(checking keys in keyring|installing )' {pacman_log} && exit 5; exit 0; fi; sleep 1; done")
    tui_wait(guest, seconds, check, "target package download (after key preflight)")


def verify_network_stop(guest, target, offset):
    log = shlex.quote(target + "/.locachy-installer/pacman.log")
    network_error = (r"failed retrieving file .* (Could not resolve|[Ff]ailed to connect|"
                     r"Couldn't connect|[Tt]imed out|[Tt]oo slow|Network is unreachable|"
                     r"No route to host|Connection reset|Recv failure|Could not resolve host)")
    guest.shell(f"tail -c +{offset + 1} {log} >/run/installer-network-stop.log && "
                "tail -n 40 /run/installer-network-stop.log && "
                "grep -qEi " + shlex.quote(network_error) + " /run/installer-network-stop.log && "
                f"grep -qF 'make-root: pacman failed;' {TUI_LOG} && "
                "locachy-install --resume-status >/run/installer-stop.json && "
                "python3 -I -c " + shlex.quote(
                    "import json, sys; d = json.load(open('/run/installer-stop.json')); "
                    "sys.exit(0 if d and d.get('phase') == 'target' and d.get('root') == sys.argv[1] else 1)"
                ) + " " + shlex.quote(target), label="confirm package retrieval failed and the same target is resumable")


def tui_failure(guest, out, serial_ready):
    print(f"Installer VM logs: {out / 'install/serial.log'}, {out / 'install/qemu.err'}", file=sys.stderr, flush=True)
    if serial_ready and guest.alive:
        try:
            logs = guest.shell(f"for log in {TUI_RUN}/refresh.log {TUI_LOG}; do "
                               "if [ -f \"$log\" ]; then printf '\\n--- %s ---\\n' \"$log\"; tail -n 60 \"$log\"; fi; done; "
                               f"{KLOG} | grep -E 'installer:|term:' | tail -n 18", output=True,
                               label="read installer failure logs")
            if logs.strip():
                print(logs.rstrip(), file=sys.stderr, flush=True)
        except (OSError, RuntimeError) as error:
            print(f"Could not read live installer logs: {error}", file=sys.stderr, flush=True)
    try:
        guest.shot("tui-failed")
    except (OSError, RuntimeError):
        pass  # A blank or stopped display must not hide the original failure.


def install_tui(args, iso, out, kernel, initrd, stops=False):
    """The full-screen installer on tty1, driven by its keys alone: the
    terminal (and back), then the whole disk, a passphrase and a user.
    With stops, cut the network during package download and continue with c
    after reconnecting. No run of this harness adds trust to an unknown key."""
    disk = out / "target.img"
    subprocess.run(["qemu-img", "create", "-f", "raw", str(disk), "32G"], check=True)
    process, guest, error = launch(out / "install", args.timeout, iso=iso, disk=disk, network=True)
    completed = False
    screenshots = {}
    serial_ready = False
    try:
        boot_live(guest, "uefi", kernel, initrd)
        serial_ready = True
        guest.shell("locale -a | grep -qi '^zh_CN.utf8$' && test -s /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc")
        wait_kmsg(guest, "installer: started", REFRESH_WAIT)
        guest.pump(15)  # its description of this computer (locachy-install --describe-json)
        screenshots["start"] = guest.shot("tui-start")
        guest.key("down")
        guest.key("ret")
        wait_kmsg(guest, "installer: running /usr/local/bin/term", 30)
        wait_kmsg(guest, "term: started /bin/bash", 30)
        screenshots["terminal"] = guest.shot("tui-terminal")
        guest.type("echo live-rescue-ok >/dev/kmsg && exit")
        guest.key("ret")
        wait_kmsg(guest, "installer: /usr/local/bin/term ended 0", 30)
        wait_kmsg(guest, "live-rescue-ok", 10)
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
        for _ in range(2):                                # Software, System: next
            guest.key("end")
            guest.key("ret")
        guest.pump(1)
        screenshots["summary"] = guest.shot("tui-summary")
        guest.key("end")
        guest.key("ret")                                  # Install: --check, then the question
        wait_kmsg(guest, "installer: waiting for erase confirmation", 120)
        guest.pump(15)
        screenshots["confirm"] = guest.shot("tui-confirm")
        guest.key("y")
        wait_kmsg(guest, "installer: installing", REFRESH_WAIT)
        target = "/run/locachy-install-target"
        tui_provenance(guest, target, "capture")
        if stops:
            wait_download(guest, target, 3600)
            record_target_identity(guest, target, "before")
            guest.shell("cat /proc/sys/kernel/random/uuid > /run/installer-test-nonce && "
                        f"cp /run/installer-test-nonce {target}/.locachy-installer/test-nonce")
            log = target + "/.locachy-installer/pacman.log"
            offset = int(guest.shell(f"state=$({KLOG}) && "
                                     "! printf '%s\\n' \"$state\" | grep -qE 'installer: installation (completed|failed)' && "
                                     f"! grep -qE '^(checking keys in keyring|installing )' {log} && stat -c %s {log}",
                                     label="recheck target download before link cut", output=True))
            guest.command("set_link", {"name": "nic0", "up": False})
            wait_kmsg(guest, INSTALL_FAILED, 3600, fail=INSTALL_DONE)
            verify_network_stop(guest, target, offset)
            screenshots["stopped-network"] = guest.shot("tui-stopped-network")
            guest.command("set_link", {"name": "nic0", "up": True})
            tui_wait(guest, 180, "nm-online -q -t 170", "network reconnect")
            guest.key("c")
            wait_kmsg(guest, "installer: continuing", REFRESH_WAIT)
            tui_provenance(guest, target, "capture-resume")
        wait_kmsg(guest, INSTALL_DONE, args.timeout, fail=INSTALL_FAILED,
                  after="installer: continuing" if stops else None,
                  label="installation completion (no automatic source-key approval)")
        if stops:
            verify_resumed_target(guest, target)
            print("PASS installer continued after network loss with unchanged partition, filesystem and install IDs", flush=True)
        guest.pump(2)
        screenshots["done"] = guest.shot("tui-done")
        tui_provenance(guest, target, "verify-resume" if stops else "verify")
        guest.shell("test -s " + target + "/var/lib/locachy/install.json && grep -q '\"encrypted\": true' " + target +
                    "/var/lib/locachy/install.json && test -s " + target + "/efi/EFI/BOOT/BOOTX64.EFI && "
                    "test ! -e /run/locachy-installer/passphrase && test ! -e /run/locachy-installer/password && "
                    "cryptsetup isLuks /dev/sda3 && df -h " + target + " && du -sh " + target + "/var/cache/pacman/pkg")
        prepare_diagnostic_boot(guest, target)
        guest.shell("printf 'LOCACHY-GUEST-INSTALL-PASSED\\n'")
        guest.send("poweroff\n")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
        completed = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        tui_failure(guest, out, serial_ready)
        raise
    finally:
        end_vm(process, guest, error, completed=completed)
    return disk, screenshots


def target_boot(disk, out, passphrase=None):
    boot_out = out / DIAGNOSTIC_BOOT
    process, guest, error = launch(boot_out, 300, disk=disk)
    completed = False
    screenshots = {}
    try:
        if passphrase:
            # The initramfs' prompt: white on black, too few colours for shot().
            guest.wait_for(rb"init: waiting for LUKS2 passphrase")
            guest.pump(3)
            guest.type(passphrase)
            guest.key("ret")
            guest.wait_for(rb"init: unlocked LUKS2 [^\r\n]+(?:\r?\n)")
        else:
            guest.wait_for(rb"init: locking")
            guest.wait_for(rb"init: started lock \(pid [0-9]+\)")
            guest.pump(5)
            screenshots["locked"] = guest.shot("locked")
            guest.type(PASSWORD)
            guest.key("ret")
            guest.wait_for(rb"init: unlocked(?:\r?\n)")
        guest.wait_for(rb"home> ")
        # Home can log its entry count before the unencrypted boot lock
        # finishes. Read the complete log rather than the consumed buffer.
        home = re.search(rb"home: ([0-9]+) containers, [0-9]+ host programs",
                         (boot_out / "serial.log").read_bytes())
        if home is None:
            raise RuntimeError("Installed home did not report its launcher entries")
        napps = int(home.group(1))
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
            guest.wait_for(rb"init: unlocked(?:\r?\n)")
            guest.wait_for(rb"home> ")
        screenshots["home"] = guest.shot("home")
        # Home's first section is apps, then Graphical Session and Files,
        # then rc.conf's term and admin. Right advances one tile; Down skips rows.
        guest.key("home")
        for _ in range(napps + 2):
            guest.key("right")
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
        for _ in range(napps + 3):
            guest.key("right")
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
        log = (boot_out / "serial.log").read_bytes()
        expected = (rb"swap: /dev/dm-[0-9]+, root: /dev/dm-[0-9]+, esp: /dev/sda1"
                    if passphrase else rb"root: /dev/sda2, esp: /dev/sda1")
        if not re.search(expected, log):
            raise RuntimeError("Installed root/ESP were not the expected target devices")
        if re.search(rb"Kernel panic|screen: none|cannot switch to the root|memory BAD|"
                     rb"init: cannot run|init: cannot send|init: the lock ended \(status [1-9]|"
                     rb"cannot make / read-only|umount /efi:", log):
            raise RuntimeError("Installed guest reported a boot or shutdown failure")
        if (boot_out / "locked.ppm").read_bytes() == (boot_out / "home.ppm").read_bytes():
            raise RuntimeError("Locked and unlocked screenshots are identical")
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
            screenshots.update(target_boot(disk, out, PASSWORD))
            (out / "result.json").write_text(json.dumps({"passed": True, "mode": args.mode, "iso": str(iso), "disk": str(disk),
                                                         **BOOT_EVIDENCE, "screenshots": screenshots}, indent=2) + "\n")
            print("PASS full-screen installer and diagnostic serial UKI: encrypted target, PAM/home/terminal "
                  "(original UKI boot and installer NVRAM not tested)")
        else:
            disk = install_guest(args, iso, out, kernel, initrd)
            screenshots = target_boot(disk, out)
            (out / "result.json").write_text(json.dumps({"passed": True, "mode": "install", "cpu_level": args.level,
                                                         "iso": str(iso), "disk": str(disk), **BOOT_EVIDENCE,
                                                         "screenshots": screenshots}, indent=2) + "\n")
            print("PASS installation and diagnostic serial UKI: guest-local build, PAM/home/terminal and clean power-off "
                  "(original UKI boot and installer NVRAM not tested)")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("FAIL installer VM: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
