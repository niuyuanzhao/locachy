#!/usr/bin/env bash
# Automated rootless test suite for Maintenance Windows tail partition subsystem.
# Covers:
#   1. Pure-Python GPT builder, CRC32, sfdisk -V, exact Disk/Partition GUID preservation
#      and complete isolation of LOCachy p1/p2/p3 in virtual dm-linear disk layout
#   2. Online create, format, sanitize-esp (Layer-1 BOOTX64.EFI stripping), and online reclaim
#   3. Shrink plan calculation + init.c shrink-plan parser & ESP disambiguation (musl-gcc & gcc)
#   4. autounattend.xml XML well-formedness & Windows hardening/Layer-4 seed image
#   5. locachy-efi --ensure-first, --bootnext-entry, and --remove-entry (mocked efibootmgr)
#   6. Exclusive lock refusal of boot-once while VM lock is held
#   7. locachy-boot lean vs shrink-armed initramfs packing
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/test-winmaint.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo "=== [1/7] Syntax checks ==="
sh -n "$SRC/link/locachy-boot"
python3 -B -m py_compile "$SRC/link/locachy-winmaint" "$SRC/link/locachy-efi"
echo "PASS syntax checks (locachy-boot, locachy-winmaint, locachy-efi)"

echo "=== [2/7] Python unit, GPT, dm-linear, seed, EFI & lock tests ==="
python3 -B - "$SRC" "$TMP" <<'PY'
import contextlib
import fcntl
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import types
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

SRC = Path(sys.argv[1])
TMP = Path(sys.argv[2])

def load_script(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod

wm = load_script("locachy_winmaint", SRC / "link/locachy-winmaint")
efi = load_script("locachy_efi", SRC / "link/locachy-efi")


class WinmaintTests(unittest.TestCase):
    def make_phys_disk(self, name, size_gib=64, root_gib=8):
        img = TMP / name
        with open(img, "wb") as f:
            f.truncate(size_gib * 1024 * 1024 * 1024)
        script = (
            "label: gpt\n"
            ",512M,U\n"
            ",4G,S\n"
            f",{root_gib}G,L\n"
        )
        subprocess.run(["sfdisk", "-q", str(img)], input=script, text=True, check=True)
        return img

    def test_01_create_virtual_gpt_guid_preservation_and_isolation(self):
        phys = self.make_phys_disk("phys-create.img", size_gib=64, root_gib=8)
        gpt_before = wm.parse_gpt_disk(phys)
        self.assertEqual(len(gpt_before["partitions"]), 3)

        # Write unmistakable sentinels in LOCachy p1/p2/p3.
        with open(phys, "r+b") as f:
            for idx, tag in enumerate((b"LOCACHY_P1_ESP_SECRET", b"LOCACHY_P2_SWAP_SECRET", b"LOCACHY_P3_LUKS_SECRET")):
                f.seek(gpt_before["partitions"][idx]["start_lba"] * 512)
                f.write(tag)

        # Online create tail triplet via CLI --image mode.
        state_file = TMP / "state-create.json"
        rc = wm.main(["create", "--size-gib", "48", "--image", str(phys), "--state-file", str(state_file)])
        self.assertEqual(rc, 0)
        v = subprocess.run(["sfdisk", "-V", str(phys)], capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)

        gpt_after = wm.parse_gpt_disk(phys)
        self.assertEqual(gpt_after["disk_guid"], gpt_before["disk_guid"])
        triplet = wm.find_winmaint_triplet(gpt_after)
        self.assertIsNotNone(triplet)
        self.assertEqual(triplet["esp"]["partn"], 4)
        self.assertEqual(triplet["msr"]["partn"], 5)
        self.assertEqual(triplet["c"]["partn"], 6)
        self.assertEqual(triplet["esp"]["sectors"], 128 * 2048)
        self.assertEqual(triplet["msr"]["sectors"], 16 * 2048)
        self.assertEqual(triplet["c"]["sectors"], 48 * 1024 * 2048)

        # Verify state.json matches physical GPT.
        state = wm.load_state(state_file)
        wm.verify_state_against_gpt(state, gpt_after)

        # Put sentinel markers in p4, p5, p6.
        with open(phys, "r+b") as f:
            f.seek((triplet["msr"]["start_lba"] + 1) * 512)
            f.write(b"WIN_MSR_SENTINEL")
            f.seek((triplet["c"]["start_lba"] + 1) * 512)
            f.write(b"WIN_C_SENTINEL")

        # Build virtual GPT head/tail blobs and assemble a sparse dm-linear equivalent image.
        head_out = TMP / "gpt-head.img"
        tail_out = TMP / "gpt-tail.img"
        head_bytes, tail_bytes, layout = wm.build_virtual_gpt_blobs(gpt_after["disk_guid"], triplet)
        head_out.write_bytes(head_bytes)
        tail_out.write_bytes(tail_bytes)

        # Verify dm-linear table mentions ONLY loop head, p4, p5, p6, loop tail (never p1/p2/p3).
        table = wm.dm_linear_table("/dev/loop0", "/dev/nvme0n1p4", "/dev/nvme0n1p5", "/dev/nvme0n1p6", "/dev/loop1", layout)
        lines = table.strip().splitlines()
        self.assertEqual(len(lines), 5)
        for forbidden in ("nvme0n1p1", "nvme0n1p2", "nvme0n1p3"):
            self.assertNotIn(forbidden, table)

        # Assemble sparse virtual disk image with exact dm-linear offsets (only copying non-zero sectors).
        virt = TMP / "virt-dm.img"
        with open(virt, "wb") as vf, open(phys, "rb") as pf:
            vf.truncate(layout["total_sectors"] * 512)
            vf.seek(0)
            vf.write(head_bytes)
            # Copy first 4 MiB of p4 (FAT metadata) and sentinel sectors of p5/p6.
            pf.seek(triplet["esp"]["start_lba"] * 512)
            vf.seek(layout["esp"]["start_lba"] * 512)
            vf.write(pf.read(4 * 1024 * 1024))
            pf.seek((triplet["msr"]["start_lba"] + 1) * 512)
            vf.seek((layout["msr"]["start_lba"] + 1) * 512)
            vf.write(pf.read(512))
            pf.seek((triplet["c"]["start_lba"] + 1) * 512)
            vf.seek((layout["c"]["start_lba"] + 1) * 512)
            vf.write(pf.read(512))
            vf.seek(layout["tail_start"] * 512)
            vf.write(tail_bytes)

        v2 = subprocess.run(["sfdisk", "-V", str(virt)], capture_output=True, text=True)
        self.assertEqual(v2.returncode, 0, v2.stdout + v2.stderr)

        vg = wm.parse_gpt_disk(virt)
        self.assertEqual(vg["disk_guid"], gpt_after["disk_guid"])
        vt = wm.find_winmaint_triplet(vg)
        self.assertIsNotNone(vt)
        for key in ("esp", "msr", "c"):
            self.assertEqual(vt[key]["part_guid"], triplet[key]["part_guid"])
            self.assertEqual(vt[key]["type_guid"], triplet[key]["type_guid"])
            self.assertEqual(vt[key]["sectors"], triplet[key]["sectors"])
            self.assertEqual(vt[key]["name"], triplet[key]["name"])

        # Confirm virtual disk contains p5/p6 sentinels and NONE of p1/p2/p3 secrets.
        with open(virt, "rb") as vf:
            vf.seek((layout["msr"]["start_lba"] + 1) * 512)
            self.assertTrue(vf.read(16) == b"WIN_MSR_SENTINEL")
            vf.seek((layout["c"]["start_lba"] + 1) * 512)
            self.assertTrue(vf.read(14) == b"WIN_C_SENTINEL")
            vf.seek(0)
            head_chunk = vf.read(4 * 1024 * 1024)
            for secret in (b"LOCACHY_P1_ESP_SECRET", b"LOCACHY_P2_SWAP_SECRET", b"LOCACHY_P3_LUKS_SECRET"):
                self.assertNotIn(secret, head_chunk)
        print("PASS pure-Python GPT builder + sfdisk -V + exact Disk/Part GUID preservation + p1/p2/p3 isolation")

    def test_02_sanitize_esp_strips_bootx64_and_keeps_bootmgfw(self):
        phys = self.make_phys_disk("phys-esp.img", size_gib=64, root_gib=8)
        gpt, triplet = wm.append_tail_triplet_sfdisk(phys, 48, is_image=True)
        wm.format_winmaint_esp_and_msr(phys, triplet, is_image=True)
        spec = f"{phys}@@{triplet['esp']['start_lba'] * 512}"
        subprocess.run(["mmd", "-i", spec, "::/EFI", "::/EFI/BOOT", "::/EFI/Microsoft", "::/EFI/Microsoft/Boot"], check=True)
        stub = TMP / "stub.efi"
        stub.write_bytes(b"MZ-stub")
        subprocess.run(["mcopy", "-i", spec, str(stub), "::/EFI/BOOT/BOOTX64.EFI"], check=True)
        subprocess.run(["mcopy", "-i", spec, str(stub), "::/EFI/Microsoft/Boot/bootmgfw.efi"], check=True)

        info = wm.sanitize_esp_image(phys, triplet["esp"]["start_lba"])
        self.assertTrue(any(p.lower().endswith("/bootx64.efi") for p in info["removed"]), info)
        self.assertTrue(info["bootmgfw_present"])

        # Second call is idempotent: nothing left to remove, bootmgfw still present.
        info2 = wm.sanitize_esp_image(phys, triplet["esp"]["start_lba"])
        self.assertEqual(info2["removed"], [])
        self.assertTrue(info2["bootmgfw_present"])
        print("PASS Layer-1 sanitize-esp strips p4 \\EFI\\BOOT\\BOOTX64.EFI while keeping bootmgfw.efi")

    def test_03_online_reclaim_deletes_triplet_and_grows_root(self):
        phys = self.make_phys_disk("phys-reclaim.img", size_gib=64, root_gib=8)
        gpt0 = wm.parse_gpt_disk(phys)
        p3_before = gpt0["partitions"][2]
        gpt1, triplet = wm.append_tail_triplet_sfdisk(phys, 48, is_image=True)
        state = wm.state_from_gpt(gpt1, phys, on_root_disk=True, root_part_uuid=p3_before["part_guid"])

        # Tampered GUID in state must refuse without deleting anything.
        bad_state = json.loads(json.dumps(state))
        bad_state["partitions"]["c"]["part_guid"] = "00000000-0000-0000-0000-000000000001"
        with self.assertRaisesRegex(RuntimeError, "GUID mismatch"):
            wm.reclaim_tail_triplet(phys, bad_state, is_image=True, grow_root=True)

        res = wm.reclaim_tail_triplet(phys, state, is_image=True, grow_root=True)
        self.assertEqual(res["deleted"], [4, 5, 6])
        self.assertTrue(res["grew_root"])

        v = subprocess.run(["sfdisk", "-V", str(phys)], capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        gpt2 = wm.parse_gpt_disk(phys)
        self.assertEqual(len(gpt2["partitions"]), 3)
        self.assertEqual(gpt2["disk_guid"], gpt0["disk_guid"])
        self.assertEqual(gpt2["partitions"][0]["part_guid"], gpt0["partitions"][0]["part_guid"])
        self.assertEqual(gpt2["partitions"][1]["part_guid"], gpt0["partitions"][1]["part_guid"])
        self.assertEqual(gpt2["partitions"][2]["part_guid"], p3_before["part_guid"])
        self.assertGreater(gpt2["partitions"][2]["sectors"], p3_before["sectors"] + 48 * 1024 * 2048)
        print("PASS online reclaim deletes verified tail triplet and grows root partition with intact GUIDs")

    def test_04_shrink_plan_validation_and_free_space_guard(self):
        # 200 GiB root partition, 40 GiB used, request 48 GiB Windows C: -> succeeds (LUKS & plain).
        root_uuid = "11223344-5566-7788-99aa-bbccddeeff00"
        gib = 1024 ** 3
        p_luks = wm.compute_shrink_plan(
            root_part_uuid=root_uuid, root_luks=True,
            cur_part_sectors=200 * 1024 * 2048,
            fs_total_bytes=199 * gib, fs_used_bytes=40 * gib, fs_min_bytes=42 * gib,
            win_c_gib=48,
        )
        self.assertEqual(p_luks["root_luks"], 1)
        self.assertGreater(p_luks["part_target_sectors"], p_luks["luks_target_sectors"])
        self.assertGreaterEqual(p_luks["luks_target_sectors"], p_luks["fs_target_kib"] * 2)

        # Refuse if < 8 GiB free would remain on LOCachy root.
        with self.assertRaisesRegex(RuntimeError, "Not enough free space"):
            wm.compute_shrink_plan(
                root_part_uuid=root_uuid, root_luks=True,
                cur_part_sectors=60 * 1024 * 2048,
                fs_total_bytes=59 * gib, fs_used_bytes=8 * gib, fs_min_bytes=8 * gib,
                win_c_gib=48,
            )

        # Refuse size below minimum 40 GiB.
        with self.assertRaises(ValueError):
            wm.compute_shrink_plan(
                root_part_uuid=root_uuid, root_luks=False,
                cur_part_sectors=200 * 1024 * 2048,
                fs_total_bytes=199 * gib, fs_used_bytes=10 * gib, fs_min_bytes=10 * gib,
                win_c_gib=32,
            )
        print("PASS shrink-plan math and 8 GiB free-space / 40 GiB minimum guards")

    def test_05_seed_image_autounattend_xml_and_hardening(self):
        seed = TMP / "seed.img"
        rc = wm.main(["build-seed", "--output", str(seed)])
        self.assertEqual(rc, 0)
        self.assertEqual(seed.stat().st_size, 4 * 1024 * 1024)

        xml_text = subprocess.run(["mtype", "-i", str(seed), "::/autounattend.xml"],
                                  capture_output=True, text=True, check=True).stdout
        root = ET.fromstring(xml_text)
        self.assertTrue(root.tag.endswith("unattend"))
        for needle in ("PreventDeviceEncryption", "HiberbootEnabled", "RealTimeIsUniversal",
                       "BypassNRO", "locachy-postinstall.cmd"):
            self.assertIn(needle, xml_text)

        cmd_text = subprocess.run(["mtype", "-i", str(seed), "::/locachy-postinstall.cmd"],
                                  capture_output=True, text=True, check=True).stdout
        for needle in ("powercfg /h off", "HiberbootEnabled", "RealTimeIsUniversal",
                       "PreventDeviceEncryption", "standby-timeout-ac 0", "locachy-win-shutdown.ps1",
                       "EventID=1074"):
            self.assertIn(needle, cmd_text)

        ps_text = subprocess.run(["mtype", "-i", str(seed), "::/locachy-win-shutdown.ps1"],
                                 capture_output=True, text=True, check=True).stdout
        self.assertIn("bcdedit /enum firmware", ps_text)
        self.assertIn("LOCachy Maintenance Windows", ps_text)
        self.assertIn("/addfirst", ps_text)
        print("PASS seed image autounattend.xml XML well-formedness and Windows hardening/Layer-4 scripts")

    def test_06_efi_ensure_first_bootnext_only_and_remove(self):
        esp = TMP / "efi-esp"
        (esp / "EFI/locachy").mkdir(parents=True)
        (esp / "EFI/locachy/locachy.efi").write_bytes(b"MZ-locachy")
        (esp / "EFI/locachy/locachy-prev.efi").write_bytes(b"MZ-prev")
        fw = TMP / "efi-fw"
        ev = fw / "efivars"
        ev.mkdir(parents=True)

        p1_guid = "8129ca5c-784a-47e7-921a-d7a952726599"
        p4_guid = "a1b2c3d4-e5f6-4708-9a0b-c1d2e3f45678"
        order = ["0005", "0001", "0002", "0004"]
        bootnext = None
        records = [
            f"Boot0001* LOCachy\tHD(1,GPT,{p1_guid},0x800,0x100000)/File(\\EFI\\locachy\\locachy.efi)\n",
            f"Boot0002* LOCachy (Previous)\tHD(1,GPT,{p1_guid},0x800,0x100000)/File(\\EFI\\locachy\\locachy-prev.efi)\n",
            f"Boot0004* Other OS\tHD(2,GPT,11111111-2222-3333-4444-555555555555,0x800,0x100000)/File(\\EFI\\other\\grub.efi)\n",
            f"Boot0005* Windows Boot Manager\tHD(4,GPT,{p4_guid},0x800,0x40000)/File(\\EFI\\Microsoft\\Boot\\bootmgfw.efi)\n",
        ]
        next_num = 6
        real_stat = os.stat

        def fake_stat(path, *a, **kw):
            if str(path) == "/dev/nvme0n1p1":
                return types.SimpleNamespace(st_mode=stat.S_IFBLK | 0o600, st_rdev=os.makedev(259, 1))
            return real_stat(path, *a, **kw)

        def fake_cmd(args):
            nonlocal order, bootnext, records, next_num
            if args[0] == "findmnt":
                target = args[args.index("--mountpoint") + 1]
                if target == str(esp):
                    return json.dumps({"filesystems": [{"target": str(esp), "source": "/dev/nvme0n1p1",
                                                        "fstype": "vfat", "options": "rw", "maj:min": "259:1"}]})
                return json.dumps({"filesystems": [{"target": str(ev), "fstype": "efivarfs", "options": "rw"}]})
            if args[0] == "lsblk":
                if args[-1] == "/dev/nvme0n1p1":
                    return json.dumps({"blockdevices": [{"path": "/dev/nvme0n1p1", "type": "part",
                                                         "pkname": "/dev/nvme0n1", "partn": 1,
                                                         "partuuid": p1_guid, "parttype": efi.ESP_TYPE,
                                                         "pttype": "gpt", "maj:min": "259:1"}]})
                return json.dumps({"blockdevices": [{"type": "disk"}]})
            self.assertEqual(args[0], "efibootmgr")
            if args[1:] == ["--verbose"]:
                bn = f"BootNext: {bootnext}\n" if bootnext else ""
                return f"BootCurrent: 0001\n{bn}BootOrder: {','.join(order)}\n" + "".join(records)
            if "--create-only" in args:
                num = f"{next_num:04X}"
                next_num += 1
                lbl = args[args.index("--label") + 1]
                ldr = args[args.index("--loader") + 1]
                part = args[args.index("--part") + 1]
                g = p4_guid if int(part) == 4 else p1_guid
                records.append(f"Boot{num}* {lbl}\tHD({part},GPT,{g},0x800,0x40000)/File({ldr})\n")
            elif "--bootorder" in args:
                order = [x for x in args[-1].split(",") if x]
            elif "--bootnext" in args:
                bootnext = args[-1].upper()
            elif "--delete-bootnext" in args:
                bootnext = None
            elif "--delete-bootnum" in args:
                num = args[args.index("--bootnum") + 1].upper()
                records = [r for r in records if not r.startswith(f"Boot{num}")]
            elif "--active" in args:
                num = args[args.index("--bootnum") + 1].upper()
                records = [r.replace(f"Boot{num} ", f"Boot{num}*") for r in records]
            else:
                self.fail(f"Unexpected efibootmgr args: {args}")
            return ""

        with mock.patch.object(efi, "EFI", fw), \
             mock.patch.object(efi, "EFIVARS", ev), \
             mock.patch.object(efi, "EFI_LOCK_PATH", str(TMP / "efi.lock")), \
             mock.patch.object(efi, "command", fake_cmd), \
             mock.patch.object(efi.os, "stat", fake_stat), \
             contextlib.redirect_stdout(io.StringIO()):
            # 1) Layer-4 heal strips Windows entry on p4_guid from BootOrder and puts LOCachy first.
            efi.ensure_first(esp, winmaint_guid=p4_guid)
            self.assertEqual(order, ["0001", "0002", "0004"])
            self.assertNotIn("0005", order)

            # 2) Layer-3 BootNext-only creates entry without touching BootOrder and sets BootNext.
            num = efi.set_bootnext_entry("/dev/nvme0n1", 4, p4_guid, esp=esp)
            self.assertEqual(bootnext, num)
            self.assertEqual(order, ["0001", "0002", "0004"])
            self.assertNotIn(num, order)

            # 3) Reclaim remove_entry clears BootNext and deletes p4 entries while keeping LOCachy intact.
            efi.remove_entry(partuuid=p4_guid)
            self.assertIsNone(bootnext)
            self.assertEqual(order, ["0001", "0002", "0004"])
            _, _, remaining = efi.parse_entries(fake_cmd(["efibootmgr", "--verbose"]))
            self.assertEqual({r["label"] for r in remaining}, {"LOCachy", "LOCachy (Previous)", "Other OS"})
        print("PASS locachy-efi --ensure-first, --bootnext-entry, and --remove-entry")

    def test_07_exclusive_lock_blocks_boot_once(self):
        lock_file = TMP / "vm.lock"
        # Hold the exclusive flock in a separate child process to simulate a live VM.
        holder = subprocess.Popen(
            [sys.executable, "-c",
             "import fcntl, os, sys, time; "
             "fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600); "
             "fcntl.flock(fd, fcntl.LOCK_EX); "
             "print('LOCKED', flush=True); "
             "sys.stdin.read()",
             str(lock_file)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
        )
        try:
            self.assertEqual(holder.stdout.readline().strip(), "LOCKED")
            with mock.patch.object(wm, "LOCK_PATH", lock_file):
                self.assertTrue(wm.is_lock_held())
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = wm.main(["boot-once", "--allow-unprivileged-test", "--skip-esp-check"])
                self.assertNotEqual(rc, 0)
                self.assertIn("lock is busy", buf.getvalue())
        finally:
            holder.stdin.close()
            holder.wait(timeout=5)
        with mock.patch.object(wm, "LOCK_PATH", lock_file):
            self.assertFalse(wm.is_lock_held())
        print("PASS exclusive VM lock blocks boot-once while held and releases cleanly on exit")


suite = unittest.defaultTestLoader.loadTestsFromTestCase(WinmaintTests)
result = unittest.TextTestRunner(verbosity=0).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
PY

echo "=== [3/7] init.c shrink-plan parser & ESP disambiguation C unit test (gcc + musl-gcc) ==="
python3 -B - "$SRC" "$TMP" <<'PY'
import importlib.machinery
import importlib.util
from pathlib import Path
import subprocess
import sys

SRC = Path(sys.argv[1])
TMP = Path(sys.argv[2])

loader = importlib.machinery.SourceFileLoader("wm", str(SRC / "link/locachy-winmaint"))
spec = importlib.util.spec_from_loader(loader.name, loader)
wm = importlib.util.module_from_spec(spec)
loader.exec_module(wm)

init_src = (SRC / "init/init.c").read_text()

def extract(marker_start, marker_end):
    a = init_src.index(marker_start)
    b = init_src.index(marker_end, a)
    return init_src[a:b]

plan_code = extract("struct winmaint_shrink_plan {", "static int winmaint_run_cmd(")
esp_disk_code = extract("static char root_data_dev[64];", "static int switch_root(")
esp_mount_code = extract("/* The ESP at /efi", "/* Keep the kernel's log")

gib = 1024 ** 3
plan_luks = wm.format_shrink_plan(wm.compute_shrink_plan(
    "11223344-5566-7788-99aa-bbccddeeff00", True,
    200 * 1024 * 2048, 199 * gib, 40 * gib, 42 * gib, 48,
))
plan_plain = wm.format_shrink_plan(wm.compute_shrink_plan(
    "aabbccdd-1122-3344-5566-778899001122", False,
    200 * 1024 * 2048, 199 * gib, 40 * gib, 42 * gib, 64,
))

def c_escape(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'

test_c = f"""
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ESP_CANDS 8
#define MS_NOATIME 0
#define MS_NOSUID 0
#define MS_NODEV 0
#define SWAP_FLAG_DISCARD 0
#define F_OK 0

static char swap_dev[64], esp_dev[64];
static char esp_cands[MAX_ESP_CANDS][64];
static int nesp_cands;
static int luks_only;

static void say(const char *fmt, ...) {{ (void)fmt; }}
static int is_mapper(const char *dev, const char *name) {{ (void)dev; (void)name; return 1; }}
static int swapon(const char *path, int flags) {{ (void)path; (void)flags; return 0; }}
static int mock_mkdir(const char *path, mode_t m) {{ (void)path; (void)m; return 0; }}
#define mkdir mock_mkdir

/* Scenario control for ESP disambiguation test. */
static int scenario;
static char mounted_efi_dev[64];

static int has_signature(const char *dev, off_t offset, const char *sig) {{
    if (offset == 82 && strcmp(sig, "FAT32   ") == 0) {{
        return strcmp(dev, "/dev/nvme0n1p4") == 0 || strcmp(dev, "/dev/nvme0n1p1") == 0;
    }}
    if (offset == 71 && strcmp(sig, "LOCACHY_EFI") == 0) {{
        if (scenario == 0) return strcmp(dev, "/dev/nvme0n1p1") == 0;
        return 0; /* scenario 1: neither ESP has LOCACHY_EFI FAT label */
    }}
    return 0;
}}
static int has_label(const char *dev, off_t offset, const char *label) {{
    (void)dev; (void)offset; (void)label; return 0;
}}
static void each_block_dev(int (*fn)(const char *)) {{
    /* Deliberately enumerate p4 (Windows ESP) BEFORE p1 (LOCachy ESP)! */
    fn("/dev/nvme0n1p4");
    fn("/dev/nvme0n1p1");
}}
static int mock_mount(const char *src, const char *tgt, const char *fs, unsigned long fl, const void *data) {{
    (void)fs; (void)fl; (void)data;
    if (strcmp(tgt, "/efi") == 0) {{
        snprintf(mounted_efi_dev, sizeof mounted_efi_dev, "%s", src);
        return 0;
    }}
    return -1;
}}
static int mock_umount(const char *tgt) {{
    if (strcmp(tgt, "/efi") == 0) mounted_efi_dev[0] = '\\0';
    return 0;
}}
static int mock_access(const char *path, int mode) {{
    (void)mode;
    if (strcmp(mounted_efi_dev, "/dev/nvme0n1p1") == 0) {{
        if (strcmp(path, "/efi/EFI/locachy/locachy.efi") == 0 ||
            strcmp(path, "/efi/EFI/locachy") == 0 ||
            strcmp(path, "/efi/EFI") == 0) return 0;
    }}
    if (strcmp(mounted_efi_dev, "/dev/nvme0n1p4") == 0) {{
        if (strcmp(path, "/efi/EFI") == 0) return 0;
    }}
    return -1;
}}
#define mount mock_mount
#define umount mock_umount
#define access mock_access

{plan_code}
{esp_disk_code}
{esp_mount_code}

int main(void) {{
    struct winmaint_shrink_plan p;
    assert(winmaint_parse_shrink_plan({c_escape(plan_luks)}, &p) == 1);
    assert(p.version == 1 && p.root_luks == 1 && p.win_esp_mib == 128 && p.win_msr_mib == 16 && p.win_c_mib == 48 * 1024);
    assert(winmaint_uuid_equal(p.root_part_uuid, "11223344-5566-7788-99AA-BBCCDDEEFF00"));

    assert(winmaint_parse_shrink_plan({c_escape(plan_plain)}, &p) == 1);
    assert(p.version == 1 && p.root_luks == 0 && p.win_c_mib == 64 * 1024);

    /* Reject malformed / unsafe plans */
    assert(winmaint_parse_shrink_plan("version=2\\naction=shrink-create\\n", &p) == 0);
    assert(winmaint_parse_shrink_plan("version=1\\naction=other\\nroot_part_uuid=11223344-5566-7788-99aa-bbccddeeff00\\n", &p) == 0);
    assert(winmaint_parse_shrink_plan(
        "version=1\\naction=shrink-create\\nroot_part_uuid=11223344-5566-7788-99aa-bbccddeeff00\\n"
        "root_luks=1\\nfs_target_kib=10485760\\nluks_target_sectors=20971520\\npart_target_sectors=20971520\\n"
        "win_esp_mib=128\\nwin_msr_mib=16\\nwin_c_mib=49152\\n", &p) == 0); /* missing LUKS2 header margin */
    assert(winmaint_parse_shrink_plan(
        "version=1\\naction=shrink-create\\nroot_part_uuid=11223344-5566-7788-99aa-bbccddeeff00\\n"
        "root_luks=0\\nfs_target_kib=10485760\\nluks_target_sectors=0\\npart_target_sectors=20971520\\n"
        "win_esp_mib=128\\nwin_msr_mib=16\\nwin_c_mib=20480\\n", &p) == 0); /* win_c_mib < 40 GiB */

    /* ESP disambiguation: p4 enumerated before p1, with LOCACHY_EFI FAT label */
    scenario = 0;
    mounted_efi_dev[0] = '\\0';
    disks();
    esp_mount();
    assert(strcmp(esp_dev, "/dev/nvme0n1p1") == 0);
    assert(strcmp(mounted_efi_dev, "/dev/nvme0n1p1") == 0);

    /* ESP disambiguation: p4 enumerated before p1, WITHOUT FAT label (probe /efi/EFI/locachy) */
    scenario = 1;
    mounted_efi_dev[0] = '\\0';
    disks();
    esp_mount();
    assert(strcmp(esp_dev, "/dev/nvme0n1p1") == 0);
    assert(strcmp(mounted_efi_dev, "/dev/nvme0n1p1") == 0);

    puts("PASS init.c winmaint_parse_shrink_plan and multi-ESP disambiguation");
    return 0;
}}
"""
c_path = TMP / "test-init-winmaint.c"
c_path.write_text(test_c)
subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror", str(c_path), "-o", str(TMP / "test-init-gcc")], check=True)
subprocess.run([str(TMP / "test-init-gcc")], check=True)
subprocess.run(["musl-gcc", "-static", "-O2", "-Wall", "-Wextra", "-Werror", str(c_path), "-o", str(TMP / "test-init-musl")], check=True)
subprocess.run([str(TMP / "test-init-musl")], check=True)
print("PASS init.c unit test compiled and passed with both gcc and musl-gcc -static")
PY

echo "=== [4/7] Full init.c + apps.c musl-gcc -static compilation (zero warnings) ==="
mkdir -p "$TMP/uapi" "$TMP/obj"
for d in linux asm asm-generic drm sound; do
    ln -sfn "/usr/include/$d" "$TMP/uapi/$d"
done
for file in "$SRC"/base/*.c; do
    name=$(basename "$file" .c)
    [ "$name" != open ] || continue
    [ "$name" != jobs ] || continue
    musl-gcc -static -O2 -ffunction-sections -fdata-sections -Wall -Wextra \
        -I"$SRC/base/include" -I"$SRC/base" -isystem "$TMP/uapi" \
        -c "$file" -o "$TMP/obj/musl-$name.o"
done
ar rcs "$TMP/obj/liblocachy-musl.a" "$TMP/obj"/musl-*.o
musl-gcc -static -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra -Werror \
    -I"$SRC/base/include" -I"$SRC/base" -isystem "$TMP/uapi" \
    "$SRC/init/init.c" "$SRC/init/apps.c" \
    -Wl,-u,theme_get -Wl,-u,theme_reload \
    "$TMP/obj/liblocachy-musl.a" -o "$TMP/init-static"
echo "PASS full init binary compiled cleanly with musl-gcc -static -Wall -Wextra -Werror"

echo "=== [5/7] locachy-boot lean vs shrink-armed initramfs check ==="
python3 -B - "$SRC" "$TMP" <<'PY'
from pathlib import Path
import subprocess
import sys

SRC = Path(sys.argv[1])
TMP = Path(sys.argv[2])
boot = (SRC / "link/locachy-boot").read_text()

# Verify shrink tools are gated behind shrink-plan.conf / --winmaint-shrink.
assert "/var/lib/locachy/winmaint/shrink-plan.conf" in boot
assert "--winmaint-shrink" in boot
for tool in ("e2fsck", "resize2fs", "sfdisk", "partx"):
    assert tool in boot

# Simulate initramfs.list generation snippet both with and without shrink plan.
start = boot.index("want_winmaint_shrink=0")
end = boot.index("    ldd $bins", start)
snippet = boot[start:end]

def run_probe(plan_exists, arg1=""):
    root = TMP / f"boot-probe-{plan_exists}-{arg1 or 'none'}"
    root.mkdir(parents=True, exist_ok=True)
    plan_file = root / "shrink-plan.conf"
    if plan_exists:
        plan_file.write_text("version=1\n")
    out_list = root / "initramfs.list"
    out_list.write_text("")
    s = snippet.replace("/var/lib/locachy/winmaint/shrink-plan.conf", str(plan_file))
    # Only exercise the want_winmaint_shrink block.
    block_start = s.index("want_winmaint_shrink=0")
    if_start = s.index("if [ \"$want_winmaint_shrink\" = 1 ]; then")
    if_end = s.index("    fi", if_start) + len("    fi")
    prog = (
        "set -eu\n"
        f'work="{root}"\n'
        'bins=""\n'
        + s[block_start:s.index("if [ -x /usr/bin/kmod")]
        + s[if_start:if_end]
        + '\nprintf "BINS=%s\\n" "$bins"\n'
    )
    res = subprocess.run(["sh", "-c", prog, "sh", arg1], capture_output=True, text=True, check=True)
    return out_list.read_text(), res.stdout

lean_list, lean_out = run_probe(False)
assert "e2fsck" not in lean_list and "sfdisk" not in lean_list and lean_out.strip() == "BINS="

armed_list, armed_out = run_probe(True)
for t in ("/sbin/e2fsck", "/sbin/resize2fs", "/sbin/sfdisk", "/sbin/partx"):
    assert t in armed_list, armed_list
assert "/usr/bin/sfdisk" in armed_out

flag_list, _ = run_probe(False, "--winmaint-shrink")
for t in ("/sbin/e2fsck", "/sbin/resize2fs", "/sbin/sfdisk", "/sbin/partx"):
    assert t in flag_list, flag_list
print("PASS locachy-boot keeps normal initramfs lean and packs shrink tools only when armed")
PY

echo "=== [6/7] Existing locachy-efi and locachy-boot test suites ==="
python3 -B "$SRC/tools/test-efi.py" "$TMP"
python3 -B "$SRC/tools/test-boot-kernel.py" "$TMP"
echo "PASS existing test-efi.py and test-boot-kernel.py suites"

echo "=== [7/7] Existing init unlock & storage unit suites ==="
python3 -B "$SRC/tools/test-init-unlock.py" "$TMP"
python3 -B "$SRC/tools/test-init-storage.py" "$TMP"
echo "PASS existing test-init-unlock.py and test-init-storage.py suites"

echo "ALL WINMAINT TESTS PASSED"
