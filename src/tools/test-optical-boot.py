#!/usr/bin/env python3
"""Boot archiso's real EFI FAT construction through a read-only USB optical VM.

This is a firmware/boot-interface regression, not full installer acceptance.
It needs Linux, KVM, packaged archiso and systemd-boot, mtools, xorriso,
musl-gcc, gcc and a readable packaged kernel. No root, networking, host
block devices or full installer ISO are used. Images are removed on exit;
build logs, firmware console logs and result.json remain in the output dir.

  python3 -B tools/test-optical-boot.py NEW_OUTPUT_DIRECTORY [--mkarchiso PATH]
The kernel and OVMF paths can be overridden with --kernel, --ovmf-code and
--ovmf-vars. Each VM uses two CPUs and 512 MiB and runs sequentially.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

SOURCE = Path(__file__).resolve().parents[1]
MARKER = "PASS optical boot reached tiny init"

INIT_C = r'''
#include <fcntl.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <unistd.h>
int main(void) {
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    puts("PASS optical boot reached tiny init");
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
'''

# Source only the actual upstream function definitions. Never execute getopts,
# validation, pacstrap, namespace setup, or the full image-build pipeline.
CONSTRUCT = r'''
set -eu
source "$1"
_msg_info() { printf '%s\n' "$1"; }
_msg_warning() { printf 'WARNING %s\n' "$1"; }
_msg_error() { printf 'ERROR %s\n' "$1" >&2; if (( $2 > 0 )); then exit "$2"; fi; }
_run_once() { "$1"; }
work_dir=$2
profile=$3
pacstrap_dir=$4
boot_dir=$5
isofs_dir=$6
efibootimg=$7
arch=x86_64
install_dir=arch
iso_label=LOCACHY_OPTICAL
iso_uuid=2026-01-01-00-00-00-00
kernel_params=
quiet=n
need_external_ucodes=0
_make_bootmode_uefi.systemd-boot
'''


def run(command, *, log, **kwargs):
    with log.open("ab") as stream:
        return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                              check=True, timeout=120, **kwargs)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def functions_only(upstream):
    text = upstream.read_text()
    match = re.search(r"(?m)^while getopts\b", text)
    if not match:
        raise RuntimeError("upstream mkarchiso getopts boundary not found")
    prefix = text[:match.start()]
    for name in ("_make_bootmode_uefi.systemd-boot", "_make_efibootimg", "_make_boot_on_fat"):
        if name + "() {" not in prefix:
            raise RuntimeError("upstream function missing: " + name)
    return prefix


def construct(root, functions, fixture, name):
    work = root / name
    work.mkdir()
    isofs = work / "iso"
    isofs.mkdir()
    esp = work / "efiboot.img"
    run(["bash", "-c", CONSTRUCT, "optical-fixture", str(functions), str(work),
         str(fixture / "profile"), str(fixture / "pacstrap"), str(fixture / "boot"),
         str(isofs), str(esp)], log=root / (name + "-construct.log"))
    return work, isofs, esp


def make_iso(root, isofs, esp, name):
    shutil.copyfile(esp, isofs / "EFI/efiboot.img")
    image = root / (name + ".iso")
    run(["xorriso", "-no_rc", "-as", "mkisofs", "-R", "-J", "-V", "LOCACHY_OPTICAL",
         "-eltorito-alt-boot", "-e", "EFI/efiboot.img", "-no-emul-boot",
         "-o", str(image), str(isofs)], log=root / (name + "-iso.log"))
    return image


def boot(root, name, image, args):
    variables = root / (name + "-vars.fd")
    shutil.copyfile(args.ovmf_vars, variables)
    serial = root / (name + "-serial.log")
    command = ["qemu-system-x86_64", "-name", "optical-boot-regression", "-machine", "q35",
               "-accel", "kvm", "-cpu", "host", "-smp", "2", "-m", "512",
               "-display", "none", "-monitor", "none", "-no-reboot", "-nic", "none",
               "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={args.ovmf_code}",
               "-drive", f"if=pflash,format=raw,unit=1,file={variables}",
               "-device", "qemu-xhci,id=xhci", "-device", "usb-bot,id=usbcd,bus=xhci.0",
               "-drive", f"if=none,id=optical,format=raw,media=cdrom,readonly=on,file={image}",
               "-device", "scsi-cd,drive=optical,bus=usbcd.0,bootindex=1",
               "-object", "rng-builtin,id=entropy", "-device", "virtio-rng-pci,rng=entropy",
               "-serial", "file:" + str(serial)]
    (root / (name + "-qemu.json")).write_text(json.dumps(command, indent=2) + "\n")
    process = None
    try:
        with (root / (name + "-qemu.err")).open("wb") as error:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=error)
            deadline = time.monotonic() + args.timeout
            while process.poll() is None and time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if name == "stock" and ("OpenVolume" in text or "Error opening root path" in text) and re.search(r"write.protected", text, re.I):
                    break
                time.sleep(0.1)
            text = serial.read_text(errors="replace") if serial.exists() else ""
            if name == "stock":
                root_error = "OpenVolume" in text or "Error opening root path" in text
                if MARKER in text or not root_error or not re.search(r"write.protected", text, re.I):
                    raise RuntimeError("stock optical boot did not reproduce the write-protected root failure; see " + str(serial))
                print("PASS stock upstream FAT reproduces write-protected root failure; tiny init not reached", flush=True)
            elif MARKER not in text or process.poll() != 0:
                raise RuntimeError("patched optical boot did not reach tiny init and power off; see " + str(serial))
            else:
                print("PASS patched upstream FAT boots via OVMF usb-bot/scsi-cd and reaches tiny init", flush=True)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        variables.unlink(missing_ok=True)
    return {"serial": str(serial), "passed": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("--mkarchiso", type=Path, default=Path(shutil.which("mkarchiso") or "/usr/bin/mkarchiso"))
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--systemd-boot", type=Path, default=Path("/usr/lib/systemd/boot/efi/systemd-bootx64.efi"))
    parser.add_argument("--ovmf-code", type=Path, default=Path("/usr/share/edk2/x64/OVMF_CODE.4m.fd"))
    parser.add_argument("--ovmf-vars", type=Path, default=Path("/usr/share/edk2/x64/OVMF_VARS.4m.fd"))
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    for tool in ("bash", "gcc", "musl-gcc", "patch", "mkfs.fat", "mcopy", "mmd", "mattrib", "mdir", "xorriso", "qemu-system-x86_64"):
        if not shutil.which(tool):
            parser.error("missing tool: " + tool)
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        parser.error("KVM access is required")
    if args.kernel is None:
        kernels = sorted(Path("/usr/lib/modules").glob("*/vmlinuz"))
        if len(kernels) != 1:
            parser.error("specify --kernel when exactly one packaged kernel is not present")
        args.kernel = kernels[0]
    for path in (args.mkarchiso, args.kernel, args.systemd_boot, args.ovmf_code, args.ovmf_vars):
        if not path.is_file():
            parser.error("missing input: " + str(path))
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"kind": "firmware/boot-interface harness, not full installer acceptance", "passed": False}
    try:
        upstream = root / "mkarchiso"
        shutil.copyfile(args.mkarchiso, upstream)
        stock = root / "stock-functions.sh"
        stock.write_text(functions_only(upstream))
        patch = SOURCE / "installer/archiso-readonly-seed.patch"
        run(["patch", "--batch", "--fuzz=0", "-p1", "-d", str(root)],
            log=root / "patch.log", input=patch.read_bytes())
        patched = root / "patched-functions.sh"
        patched.write_text(functions_only(upstream))
        fixture = root / "fixture"
        for directory in ("boot", "profile/efiboot/loader/entries", "pacstrap/usr/lib/systemd/boot/efi"):
            (fixture / directory).mkdir(parents=True)
        shutil.copyfile(args.systemd_boot, fixture / "pacstrap/usr/lib/systemd/boot/efi/systemd-bootx64.efi")
        shutil.copyfile(args.kernel, fixture / "boot/vmlinuz-linux")
        (fixture / "init.c").write_text(INIT_C)
        run(["musl-gcc", "-static", "-O2", "-Wall", "-Wextra", "-Werror", str(fixture / "init.c"),
             "-o", str(fixture / "init")], log=root / "fixture-build.log")
        run(["gcc", "-O2", str(SOURCE / "tools/gen_init_cpio.c"), "-o", str(fixture / "gen_init_cpio")],
            log=root / "fixture-build.log")
        listing = fixture / "initramfs.list"
        listing.write_text("dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n"
                           + f"file /init {fixture / 'init'} 0755 0 0\n")
        cpio = subprocess.check_output([str(fixture / "gen_init_cpio"), str(listing)], timeout=30)
        (fixture / "boot/initramfs-linux.img").write_bytes(gzip.compress(cpio, mtime=0))
        (fixture / "profile/efiboot/loader/loader.conf").write_text("default optical.conf\ntimeout 0\nconsole-mode keep\n")
        (fixture / "profile/efiboot/loader/entries/optical.conf").write_text(
            "title Optical interface test\narchitecture x64\nlinux /%INSTALL_DIR%/boot/%ARCH%/vmlinuz-linux\n"
            "initrd /%INSTALL_DIR%/boot/%ARCH%/initramfs-linux.img\noptions console=ttyS0,115200 loglevel=7\n")
        records = {}
        for name, functions in (("stock", stock), ("patched", patched)):
            work, isofs, esp = construct(root, functions, fixture, name)
            seed = subprocess.run(["mcopy", "-i", str(esp), "::/loader/random-seed", "-"],
                                  capture_output=True, timeout=30)
            attributes = subprocess.run(["mattrib", "-i", str(esp), "::/loader/random-seed"],
                                        capture_output=True, text=True, timeout=30)
            (root / (name + "-seed-attributes.log")).write_text(attributes.stdout + attributes.stderr)
            if name == "patched":
                if seed.returncode or seed.stdout or not re.search(r"\bR\b", attributes.stdout):
                    raise RuntimeError("patched FAT needs a zero-byte read-only random-seed")
                print("PASS patched FAT has a zero-byte read-only ::/loader/random-seed", flush=True)
            elif seed.returncode == 0:
                raise RuntimeError("stock upstream unexpectedly supplied random-seed")
            records[name] = {"esp_sha256": digest(esp), "seed_present": seed.returncode == 0,
                             "seed_size": len(seed.stdout) if seed.returncode == 0 else None,
                             "seed_attributes": attributes.stdout.strip()}
            image = make_iso(root, isofs, esp, name)
            records[name].update(boot(root, name, image, args))
        result.update(passed=True, upstream_sha256=digest(args.mkarchiso), patch_sha256=digest(patch),
                      kernel=str(args.kernel), systemd_boot_sha256=digest(args.systemd_boot), cases=records)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print("FAIL optical boot: " + str(error), file=sys.stderr)
        return 1
    finally:
        # The output directory was made by this invocation. Keep evidence,
        # not disposable kernel/ESP/ISO/firmware copies or test binaries.
        for name in ("fixture", "stock", "patched"):
            shutil.rmtree(root / name, ignore_errors=True)
        for pattern in ("*.iso", "*-vars.fd"):
            for path in root.glob(pattern):
                path.unlink(missing_ok=True)
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
