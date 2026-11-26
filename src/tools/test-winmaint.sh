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
#   8. boot-default (Windows first until switched back): state, status, guard/heal/boot-once
#      respecting the mode; the Windows-side Return to LOCachy and guard scripts; driver-seed;
#      the locachy-winiso lookup of vm (mocked)
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
        self.assertEqual(seed.stat().st_size, 16 * 1024 * 1024)

        part = f"{seed}@@{wm.SEED_PART_OFFSET_BYTES}"
        xml_text = subprocess.run(["mtype", "-i", part, "::/autounattend.xml"],
                                  capture_output=True, text=True, check=True).stdout
        root = ET.fromstring(xml_text)
        self.assertTrue(root.tag.endswith("unattend"))
        for needle in ("PreventDeviceEncryption", "HiberbootEnabled", "RealTimeIsUniversal",
                       "BypassNRO", "locachy-postinstall.cmd"):
            self.assertIn(needle, xml_text)

        cmd_text = subprocess.run(["mtype", "-i", part, "::/locachy-postinstall.cmd"],
                                  capture_output=True, text=True, check=True).stdout
        for needle in ("powercfg /h off", "HiberbootEnabled", "RealTimeIsUniversal",
                       "PreventDeviceEncryption", "standby-timeout-ac 0", "locachy-win-shutdown",
                       "locachy-register"):
            self.assertIn(needle, cmd_text)
        registration = subprocess.run(["mtype", "-i", part, "::/locachy-register.ps1"],
                                      capture_output=True, text=True, check=True).stdout
        self.assertEqual(registration, wm.REGISTER_PS1)

        ps_text = subprocess.run(["mtype", "-i", part, "::/locachy-win-shutdown.ps1"],
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

    def test_08_resolve_state_disk_swapped_nvme_and_commands(self):
        disk_a = self.make_phys_disk("swap-disk-a.img", size_gib=64, root_gib=8)
        disk_b = self.make_phys_disk("swap-disk-b.img", size_gib=64, root_gib=8)
        gpt_b, _triplet = wm.append_tail_triplet_sfdisk(disk_b, 48, is_image=True)
        state = wm.state_from_gpt(gpt_b, disk_a, on_root_disk=False)
        state_file = TMP / "state-swap.json"
        wm.save_state(state, state_file)
        with open(disk_a, "rb") as f:
            disk_a_head_before = f.read(4096)

        # Direct helper resolves swapped disk to disk_b by Disk GUID.
        resolved, resolved_gpt = wm.resolve_state_disk(state, disk_candidates=[disk_a, disk_b])
        self.assertEqual(resolved, str(disk_b))
        self.assertEqual(resolved_gpt["disk_guid"], state["disk_guid"])

        # Read-only status reports resolved_disk without mutating state.json.
        with mock.patch.object(wm, "STATE_FILE", state_file), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", TMP / "no-shrink-plan"), \
             mock.patch.object(wm, "SHRINK_STATUS_VAR", TMP / "no-shrink-status"), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "swap-status.lock"), \
             mock.patch.object(wm, "detect_root_layout", side_effect=RuntimeError("no root")), \
             mock.patch.object(wm, "candidate_disks", return_value=[]), \
             mock.patch.object(wm, "_is_block_device_path", return_value=True), \
             mock.patch.object(wm, "_scan_whole_disk_candidates", return_value=[str(disk_a), str(disk_b)]):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = wm.main(["status", "--json"])
            self.assertEqual(rc, 0)
            info = json.loads(out.getvalue())
            self.assertTrue(info["gpt_verified"], info)
            self.assertEqual(info["resolved_disk"], str(disk_b))
            self.assertEqual(info["esp_dev"], wm.partition_child(str(disk_b), 4))
            # state.json keeps the recorded path unchanged during read-only status.
            self.assertEqual(wm.load_state(state_file)["disk"], str(disk_a))

        # boot-once resolves disk_b and passes disk_b (never disk_a) to locachy-efi.
        seen_cmds = []

        def fake_run_cmd(args, **_kw):
            seen_cmds.append(list(args))
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(wm, "LOCK_PATH", TMP / "swap-boot.lock"), \
             mock.patch.object(wm, "_is_block_device_path", return_value=True), \
             mock.patch.object(wm, "_scan_whole_disk_candidates", return_value=[str(disk_a), str(disk_b)]), \
             mock.patch.object(wm, "run_cmd", side_effect=fake_run_cmd):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = wm.main([
                    "boot-once", "--allow-unprivileged-test", "--skip-esp-check",
                    "--state-file", str(state_file),
                ])
            self.assertEqual(rc, 0)
        self.assertEqual(len(seen_cmds), 1)
        efi_call = seen_cmds[0]
        self.assertIn("--bootnext-entry", efi_call)
        self.assertEqual(efi_call[efi_call.index("--disk") + 1], str(disk_b))
        self.assertEqual(efi_call[efi_call.index("--part") + 1], "4")
        with open(disk_a, "rb") as f:
            self.assertEqual(f.read(4096), disk_a_head_before)
        print("PASS swapped-disk resolution finds unique matching disk_guid for status and boot-once")

    def test_09_resolve_state_disk_duplicate_guid_fails_closed(self):
        disk_a = self.make_phys_disk("dup-disk-a.img", size_gib=6, root_gib=1)
        disk_b = self.make_phys_disk("dup-disk-b.img", size_gib=64, root_gib=8)
        gpt_b, _triplet = wm.append_tail_triplet_sfdisk(disk_b, 48, is_image=True)
        # Clone GPT head and tail into a sparse image to simulate a raw dd clone attached simultaneously.
        disk_clone = TMP / "dup-disk-b-clone.img"
        size_b = disk_b.stat().st_size
        with open(disk_b, "rb") as src, open(disk_clone, "wb") as dst:
            dst.truncate(size_b)
            dst.write(src.read(1024 * 1024))
            src.seek(size_b - 1024 * 1024)
            dst.seek(size_b - 1024 * 1024)
            dst.write(src.read(1024 * 1024))

        state_swapped = wm.state_from_gpt(gpt_b, disk_a, on_root_disk=False)
        with self.assertRaisesRegex(RuntimeError, "Ambiguous disk GUID"):
            wm.resolve_state_disk(state_swapped, disk_candidates=[disk_a, disk_b, disk_clone])

        state_direct = wm.state_from_gpt(gpt_b, disk_b, on_root_disk=False)
        with self.assertRaisesRegex(RuntimeError, "Ambiguous disk GUID"):
            wm.resolve_state_disk(state_direct, disk_candidates=[disk_b, disk_clone])
        print("PASS duplicate Disk GUID across two disks fails closed with Ambiguous disk GUID")

    def test_10_resolve_state_disk_missing_guid_fails_closed(self):
        disk_a = self.make_phys_disk("miss-disk-a.img", size_gib=6, root_gib=1)
        disk_c = self.make_phys_disk("miss-disk-c.img", size_gib=6, root_gib=1)
        fake_state = {
            "disk": str(disk_a),
            "disk_guid": "00000000-0000-4000-8000-000000000099",
        }
        with self.assertRaisesRegex(RuntimeError, "Disk GUID"):
            wm.resolve_state_disk(fake_state, disk_candidates=[disk_a, disk_c])
        # Regular-file image path without disk_candidates fails closed without scanning host /dev.
        with self.assertRaisesRegex(RuntimeError, "Disk GUID mismatch"):
            wm.resolve_state_disk(fake_state)
        print("PASS missing Disk GUID fails closed on candidate scan and image mismatch")

    def test_11_secondary_disk_create_reclaim_and_candidate_listing(self):
        # Secondary GPT disk with an existing data partition: create appends the triplet,
        # reclaim removes only the triplet and leaves the data partition untouched.
        sec = TMP / "sec-disk.img"
        with open(sec, "wb") as f:
            f.truncate(64 * 1024 * 1024 * 1024)
        subprocess.run(["sfdisk", "-q", str(sec)], input="label: gpt\n,8G,L\n", text=True, check=True)
        gpt0 = wm.parse_gpt_disk(sec)
        data_part = gpt0["partitions"][0]
        state_file = TMP / "sec-state.json"

        rc = wm.main(["create", "--size-gib", "48", "--image", str(sec), "--state-file", str(state_file)])
        self.assertEqual(rc, 0)
        state = wm.load_state(state_file)
        self.assertFalse(state["on_root_disk"])
        self.assertEqual(state["root_part_uuid"], "")

        # Candidate listing marks root vs secondary, filters live medium, skips unreadable disks.
        small = TMP / "small-disk.img"
        with open(small, "wb") as f:
            f.truncate(8 * 1024 * 1024 * 1024)
        subprocess.run(["sfdisk", "-q", str(small)], input="label: gpt\n,7G,L\n", text=True, check=True)
        live_iso = TMP / "live-medium.img"
        with open(live_iso, "wb") as f:
            f.truncate(2 * 1024 * 1024 * 1024)
        subprocess.run(["sfdisk", "-q", str(live_iso)], input="label: gpt\n,1G,L\n", text=True, check=True)

        fake_disks = [
            {"path": str(small), "model": "Small", "size_bytes": 8 * 1024 ** 3},
            {"path": str(sec), "model": "Second", "size_bytes": 64 * 1024 ** 3},
            {"path": str(live_iso), "model": "LiveUSB", "size_bytes": 2 * 1024 ** 3},
        ]
        with mock.patch.object(wm, "_whole_disks", return_value=fake_disks), \
             mock.patch.object(wm, "live_medium_disks", return_value={str(live_iso)}):
            cands = wm.candidate_disks(root_disk=str(small))
        self.assertEqual([c["path"] for c in cands], [str(small), str(sec)])
        self.assertTrue(cands[0]["is_root"])
        self.assertFalse(cands[1]["is_root"])
        self.assertTrue(cands[1]["has_maintenance_windows"])

        # Reclaiming a secondary disk deletes the triplet and does not grow the data partition.
        res = wm.reclaim_tail_triplet(sec, state, is_image=True, grow_root=True)
        self.assertEqual(res["deleted"], [2, 3, 4])
        self.assertFalse(res["grew_root"])
        gpt_after = wm.parse_gpt_disk(sec)
        self.assertEqual(len(gpt_after["partitions"]), 1)
        self.assertEqual(gpt_after["partitions"][0]["part_guid"], data_part["part_guid"])
        self.assertEqual(gpt_after["partitions"][0]["sectors"], data_part["sectors"])

        # A secondary disk without enough tail space refuses without touching partitions.
        with self.assertRaisesRegex(RuntimeError, "free tail space"):
            wm.append_tail_triplet_sfdisk(small, 40, is_image=True)
        print("PASS secondary-disk create/reclaim leaves existing partitions intact and candidate_disks filters live medium")

    def test_12_create_with_root_targets_chosen_disk_and_refuses_small_disk(self):
        root_disk = self.make_phys_disk("r12-root.img", size_gib=64, root_gib=8)
        sec = self.make_phys_disk("r12-sec.img", size_gib=64, root_gib=8)
        small = self.make_phys_disk("r12-small.img", size_gib=20, root_gib=8)
        real_format = wm.format_winmaint_esp_and_msr

        def format_as_image(disk_path, triplet, is_image=False):
            return real_format(disk_path, triplet, is_image=True)

        root_before = wm.parse_gpt_disk(root_disk)["partitions"]
        small_before = wm.parse_gpt_disk(small)["partitions"]
        target = TMP / "r12-installed"
        shrink_plan = TMP / "r12-no-plan"
        quiet = io.StringIO()
        with mock.patch.object(wm.os, "geteuid", return_value=0), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "r12.lock"), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", shrink_plan), \
             mock.patch.object(wm, "live_medium_disks", return_value=set()), \
             mock.patch.object(wm, "format_winmaint_esp_and_msr", side_effect=format_as_image), \
             contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(wm.main(["create", "--root", str(target), "--size-gib", "48"]), 1)
            self.assertEqual(wm.main(["create", "--root", str(target), "--disk", str(small), "--size-gib", "48"]), 1)
            self.assertEqual(wm.main(["create", "--root", str(target), "--disk", str(sec), "--size-gib", "48"]), 0)
        self.assertIn("unallocated tail space", quiet.getvalue())
        state = wm.load_state(target / "var/lib/locachy/winmaint/state.json")
        self.assertEqual(state["disk"], str(sec))
        self.assertFalse(state["on_root_disk"])
        self.assertFalse(shrink_plan.exists())
        self.assertEqual((target / "etc/locachy/containers/winmaint.conf").read_text(), wm.CONTAINER_CONF_TEXT)
        self.assertEqual(len(wm.parse_gpt_disk(sec)["partitions"]), 6)
        self.assertEqual(wm.parse_gpt_disk(small)["partitions"], small_before)
        self.assertEqual(wm.parse_gpt_disk(root_disk)["partitions"], root_before)
        print("PASS create --root --disk targets the chosen secondary disk and refuses a disk without tail space")

    def test_13_status_reports_resolved_disk_and_on_root_flag(self):
        disk_b = self.make_phys_disk("r13-b.img", size_gib=64, root_gib=8)
        gpt_b, _triplet = wm.append_tail_triplet_sfdisk(disk_b, 48, is_image=True)
        state = wm.state_from_gpt(gpt_b, disk_b, on_root_disk=False)
        state_file = TMP / "r13-state.json"
        wm.save_state(state, state_file)
        with mock.patch.object(wm, "STATE_FILE", state_file), \
             mock.patch.object(wm, "detect_root_layout", side_effect=RuntimeError("no root")), \
             mock.patch.object(wm, "candidate_disks", return_value=[{"path": str(disk_b), "is_root": False}]), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", TMP / "r13-no-plan"), \
             mock.patch.object(wm, "SHRINK_STATUS_VAR", TMP / "r13-no-status"), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "r13.lock"):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(wm.main(["status", "--json"]), 0)
        info = json.loads(out.getvalue())
        self.assertTrue(info["gpt_verified"], info)
        self.assertEqual(info["resolved_disk"], str(disk_b))
        self.assertFalse(info["on_root_disk"])
        self.assertEqual(info["candidate_disks"][0]["path"], str(disk_b))
        print("PASS status reports resolved_disk, on_root_disk and candidate_disks")

    def test_14_vm_qmp_and_wayland_gtk_display(self):
        import socket as _sock
        import threading

        # On a Wayland session, GTK display disables GL; other displays stay untouched;
        # -qmp is emitted only when requested.
        with mock.patch.dict(os.environ, {"WAYLAND_DISPLAY": "wayland-1"}, clear=False):
            argv = wm.build_qemu_argv("/dev/mapper/x", "/code", "/vars", qmp_sock="/tmp/q.sock")
            # window-close=off: closing the window never pulls Windows' power.
            self.assertEqual(argv[argv.index("-display") + 1], "gtk,gl=off,window-close=off")
            argv_gtk = wm.build_qemu_argv("/dev/mapper/x", "/code", "/vars", display_spec="gtk,zoom-to-fit=on")
            self.assertEqual(argv_gtk[argv_gtk.index("-display") + 1], "gtk,zoom-to-fit=on,window-close=off")
            self.assertEqual(wm.qemu_gtk_display("gtk,window-close=on"), "gtk,window-close=on")
            self.assertEqual(wm.qemu_gtk_display("none"), "none")
            self.assertEqual(argv[argv.index("-qmp") + 1], "unix:/tmp/q.sock,server=on,wait=off")
            argv_no_qmp = wm.build_qemu_argv("/dev/mapper/x", "/code", "/vars")
            self.assertNotIn("-qmp", argv_no_qmp)
            argv_sdl = wm.build_qemu_argv("/dev/mapper/x", "/code", "/vars", display_spec="sdl")
            self.assertEqual(argv_sdl[argv_sdl.index("-display") + 1], "sdl")

        # vm-stop talks QMP: greeting -> qmp_capabilities -> system_powerdown (and quit when still alive).
        sock_path = TMP / "fake-qmp.sock"
        received = []
        ready = threading.Event()

        def serve():
            with _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM) as srv:
                srv.bind(str(sock_path))
                srv.listen(4)
                ready.set()
                for _ in range(2):
                    conn, _ = srv.accept()
                    with conn:
                        conn.sendall(b'{"QMP":{"version":{}}}\n')
                        f = conn.makefile("r", encoding="utf-8")
                        for line in f:
                            req = json.loads(line)
                            received.append(req["execute"])
                            conn.sendall(b'{"return":{}}\n')
                            if req["execute"] != "qmp_capabilities":
                                break

        t = threading.Thread(target=serve, daemon=True)
        t.start()
        ready.wait(timeout=5)
        rc = wm.main(["vm-stop", "--qmp-sock", str(sock_path), "--wait", "0.2", "--quit-after"])
        t.join(timeout=5)
        self.assertEqual(rc, 0)
        self.assertEqual(received, ["qmp_capabilities", "system_powerdown", "qmp_capabilities", "quit"])
        # Missing socket is a clean no-op (also exercises LOCACHY_VM_QMP_SOCK).
        with mock.patch.dict(os.environ, {"LOCACHY_VM_QMP_SOCK": str(TMP / "no-such-qmp.sock")}, clear=False):
            self.assertEqual(wm.main(["vm-stop"]), 0)
        with mock.patch.dict(os.environ, {"DISPLAY": ":0"}, clear=False):
            os.environ.pop("WAYLAND_DISPLAY", None)
            argv_x = wm.build_qemu_argv("/dev/mapper/x", "/code", "/vars")
            self.assertEqual(argv_x[argv_x.index("-display") + 1], "gtk,window-close=off")
        print("PASS QEMU GTK display: gl=off on Wayland, window-close=off everywhere; vm-stop QMP shutdown flow")

    def test_15_container_conf_lifecycle_and_vm_sigterm(self):
        import signal as _sig

        root = TMP / "r15-root"
        conf = root / "etc/locachy/containers/winmaint.conf"
        self.assertTrue(wm.write_container_conf(root))
        self.assertEqual(conf.read_text(), wm.CONTAINER_CONF_TEXT)
        # Never overwrites an existing file.
        self.assertFalse(wm.write_container_conf(root))
        # Removes only when vm_kind="winmaint" is present.
        self.assertTrue(wm.remove_container_conf(root))
        self.assertFalse(conf.exists())
        conf.write_text('command="/usr/bin/custom"\n')
        self.assertFalse(wm.write_container_conf(root))
        self.assertFalse(wm.remove_container_conf(root))
        self.assertEqual(conf.read_text(), 'command="/usr/bin/custom"\n')

        # SIGTERM before QEMU starts raises SystemExit(128 + SIGTERM) so finally cleans up.
        state_pre = {}
        with self.assertRaises(SystemExit) as cm:
            wm._vm_sigterm(state_pre)(_sig.SIGTERM, None)
        self.assertEqual(cm.exception.code, 128 + _sig.SIGTERM)

        # SIGTERM while QEMU runs forwards SIGTERM to QEMU without raising.
        sent = []
        fake_qemu = types.SimpleNamespace(poll=lambda: None, send_signal=lambda s: sent.append(s))
        state_run = {"qemu": fake_qemu}
        wm._vm_sigterm(state_run)(_sig.SIGTERM, None)
        self.assertEqual(sent, [_sig.SIGTERM])

        # SIGTERM during cleanup is ignored.
        state_run["cleaning"] = True
        wm._vm_sigterm(state_run)(_sig.SIGTERM, None)
        self.assertEqual(sent, [_sig.SIGTERM])
        print("PASS managed winmaint.conf lifecycle and cmd_vm SIGTERM forwarding/cleanup")

    def test_16_vm_argv_defaults_hyperv_nvme_io_uring_e1000e_virtio_gpu(self):
        argv = wm.build_qemu_argv("/dev/mapper/x", "/c.fd", "/v.fd", vnc_addr="127.0.0.1:1")
        s = " ".join(argv)
        cpu = argv[argv.index("-cpu") + 1]
        self.assertEqual(cpu, wm.HYPERV_CPU)
        flags = cpu.split(",")
        for need in ("host", "hv-relaxed", "hv-vapic", "hv-spinlocks=0x1fff", "hv-time", "hv-vpindex", "hv-synic",
                     "hv-stimer", "hv-stimer-direct", "hv-reset", "hv-frequencies", "hv-tlbflush", "hv-ipi"):
            self.assertIn(need, flags)
        # UNVERIFIED on Windows, verified only as QEMU options: no hv-passthrough, no hv-default.
        self.assertNotIn("hv-passthrough", s)
        self.assertNotIn("hv-default", s)
        disk = next(a for a in argv if "id=disk0" in a)
        self.assertIn("aio=io_uring", disk)
        self.assertIn("cache=none", disk)
        self.assertIn("nvme,drive=disk0", s)
        self.assertNotIn("iothread", s)
        self.assertEqual(argv[argv.index("-netdev") + 1], "user,id=net0")
        self.assertIn("e1000e,netdev=net0", s)
        self.assertIn("virtio-rng-pci,rng=rng0", s)
        self.assertIn("-vga none", s)
        self.assertIn("-device virtio-vga", s)
        self.assertNotIn("memory-backend", s)
        self.assertNotIn("vhost-user", s)
        print("PASS vm argv defaults: explicit hv-* CPU set (no hv-passthrough, no hv-default), aio=io_uring, "
              "nvme, e1000e, user netdev, virtio-rng, virtio-vga (Windows behaviour unverified)")

    def test_17_vm_argv_virtio_bus_virtio_nic_passt_virtio_iso_std_gpu(self):
        argv = wm.build_qemu_argv("/dev/mapper/x", "/c.fd", "/v.fd", vnc_addr="127.0.0.1:1",
                                  iso_path="/iso/win11.iso",
                                  disk_bus="virtio", nic="virtio", netdev="passt",
                                  virtio_iso="/iso/virtio-win.iso", gpu="std")
        s = " ".join(argv)
        self.assertIn("-object iothread,id=io0", s)
        self.assertIn("virtio-blk-pci,drive=disk0,iothread=io0,discard=on", s)
        self.assertNotIn("nvme,drive=disk0", s)
        self.assertIn("virtio-net-pci,netdev=net0", s)
        self.assertEqual(argv[argv.index("-netdev") + 1], "passt,id=net0")
        self.assertIn("file=/iso/win11.iso,media=cdrom,readonly=on,if=none,id=cd0", s)
        self.assertIn("ide-cd,drive=cd0,bus=ide.0,bootindex=1", s)
        self.assertIn("file=/iso/virtio-win.iso,media=cdrom,readonly=on,if=none,id=cd1", s)
        self.assertIn("ide-cd,drive=cd1,bus=ide.1", s)
        self.assertNotIn("virtio-vga", s)
        self.assertNotIn("-vga", argv)
        for bad in (dict(disk_bus="scsi"), dict(nic="rtl8139"), dict(netdev="bridge"), dict(gpu="qxl")):
            with self.assertRaises(ValueError):
                wm.build_qemu_argv("/dev/mapper/x", "/c.fd", "/v.fd", **bad)
        print("PASS vm argv: --disk-bus virtio (iothread, discard), --nic virtio, passt netdev, virtio-win CD, "
              "std GPU; unknown choices refused")

    def test_18_share_dir_virtiofs_argv_and_parsing(self):
        argv = wm.build_qemu_argv("/dev/mapper/x", "/c.fd", "/v.fd", vnc_addr="127.0.0.1:1",
                                  memory_mib=4096, share_sockets=[("datasets", "/run/x/virtiofs-0.sock")])
        s = " ".join(argv)
        self.assertIn("-object memory-backend-memfd,id=mem,size=4096M,share=on", s)
        self.assertIn("-machine q35,smm=on,memory-backend=mem", s)
        self.assertIn("-chardev socket,id=vfs0,path=/run/x/virtiofs-0.sock", s)
        self.assertIn("-device vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=datasets", s)
        stub = TMP / "r18-virtiofsd"
        stub.write_text("#!/bin/sh\nexit 0\n")
        stub.chmod(0o755)
        with mock.patch.dict(os.environ, {"LOCACHY_VIRTIOFSD": str(stub)}):
            self.assertEqual(wm.find_virtiofsd(), str(stub))
        vargv = wm.virtiofsd_argv("/usr/lib/virtiofsd", "/srv/data", "/run/x/virtiofs-0.sock")
        self.assertEqual(vargv[0], "/usr/lib/virtiofsd")
        for need in ("--socket-path=/run/x/virtiofs-0.sock", "--shared-dir=/srv/data",
                     "--sandbox=namespace", "--cache=auto"):
            self.assertIn(need, vargv)
        share = TMP / "r18-share-src"
        share.mkdir()
        self.assertEqual(wm.parse_share_dir(f"{share}:work", 0), {"host": str(share), "tag": "work"})
        self.assertEqual(wm.parse_share_dir(str(share), 2)["tag"], "share2")
        with self.assertRaises(RuntimeError):
            wm.parse_share_dir("relative/path", 0)
        with self.assertRaises(RuntimeError):
            wm.parse_share_dir(f"{share}:bad tag!", 0)
        print("PASS --share-dir: memfd backend, machine memory-backend, vhost-user-fs-pci, virtiofsd argv, PATH[:TAG]")

    def test_19_vm_dry_run_wires_cli_options_into_argv(self):
        disk_img = self.make_phys_disk("vm-cli.img", size_gib=64, root_gib=8)
        gpt, _triplet = wm.append_tail_triplet_sfdisk(disk_img, 48, is_image=True)
        state = wm.state_from_gpt(gpt, disk_img, on_root_disk=False)
        state_file = TMP / "vm-cli-state.json"
        wm.save_state(state, state_file)
        share = TMP / "vm-cli-share"
        share.mkdir()
        stub = TMP / "vm-cli-virtiofsd"
        stub.write_text("#!/bin/sh\nexit 0\n")
        stub.chmod(0o755)
        code = TMP / "vm-cli-code.fd"
        code.write_bytes(b"c")
        vars_tpl = TMP / "vm-cli-vars.fd"
        vars_tpl.write_bytes(b"v")
        iso = TMP / "vm-cli-virtio.iso"
        iso.write_bytes(b"i")
        out = io.StringIO()
        with mock.patch.object(wm, "STATE_FILE", state_file), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "vm-cli.lock"), \
             mock.patch.object(wm, "OVMF_CODE_CANDIDATES", (str(code),)), \
             mock.patch.object(wm, "OVMF_VARS_CANDIDATES", (str(vars_tpl),)), \
             mock.patch.object(wm, "qemu_device_names", return_value={"virtio-vga", "virtio-blk-pci"}), \
             mock.patch.object(wm, "choose_netdev", return_value="passt"), \
             mock.patch.object(wm, "winiso_helper_path", return_value=None), \
             mock.patch.dict(os.environ, {"LOCACHY_VIRTIOFSD": str(stub)}), \
             contextlib.redirect_stdout(out):
            rc = wm.main(["vm", "--dry-run", "--disk-bus", "virtio", "--nic", "virtio",
                          "--share-dir", f"{share}:work", "--virtio-iso", str(iso),
                          "--no-tpm", "--display", "none"])
        self.assertEqual(rc, 0)
        info = json.loads(out.getvalue())
        s = " ".join(info["qemu_argv"])
        self.assertIn("virtio-blk-pci,drive=disk0,iothread=io0,discard=on", s)
        self.assertIn("virtio-net-pci,netdev=net0", s)
        self.assertIn("passt,id=net0", s)
        self.assertIn(f"file={iso},media=cdrom,readonly=on,if=none,id=cd1", s)
        self.assertIn("-device virtio-vga", s)
        self.assertIn("memory-backend-memfd,id=mem,size=4096M,share=on", s)
        self.assertIn("-device vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=work", s)
        self.assertEqual(info["virtiofsd"][0][0], str(stub))
        self.assertIn(f"--shared-dir={share}", info["virtiofsd"][0])
        print("PASS vm --dry-run: --disk-bus, --nic, --share-dir (virtiofsd argv too), --virtio-iso, "
              "passt and virtio-vga wired into the argv")

    def test_20_real_qemu_accepts_vm_argv_with_kvm(self):
        import shutil
        import socket
        import threading
        import time

        if not shutil.which("qemu-system-x86_64") or not os.access("/dev/kvm", os.R_OK | os.W_OK):
            self.skipTest("needs qemu-system-x86_64 and /dev/kvm")
        code = wm.pick_first_existing(wm.OVMF_CODE_CANDIDATES)
        vars_tpl = wm.pick_first_existing(wm.OVMF_VARS_CANDIDATES)
        if not code or not vars_tpl:
            self.skipTest("no OVMF firmware")
        disk = TMP / "qemu-parse-disk.raw"
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)
        iso = TMP / "qemu-parse-virtio.iso"
        iso.write_bytes(b"")

        def parse_run(argv):
            """Start QEMU paused (-S); alive after 2.5 s means the argv parsed and devices realized."""
            proc = subprocess.Popen([argv[0], "-S", *argv[1:]], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(2.5)
            exited = proc.poll() is not None
            if not exited:
                proc.kill()
            _out, err = proc.communicate(timeout=10)
            return (exited, err)

        vars_a = TMP / "qemu-parse-vars-a.fd"
        shutil.copyfile(vars_tpl, vars_a)
        argv_a = wm.build_qemu_argv(str(disk), code, str(vars_a), memory_mib=512, cpus=2,
                                    display_spec="none", gpu="std")
        exited_a, err_a = parse_run(argv_a)
        self.assertFalse(exited_a, f"default VM argv rejected by QEMU: {err_a}")
        print("PASS real QEMU accepts the default VM argv: hv-* CPU, aio=io_uring nvme, e1000e, virtio-rng")

        vfs_sock = TMP / "vfs0.sock"
        vfs_sock.unlink(missing_ok=True)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(vfs_sock))
        listener.listen(4)
        held = []

        def accept_forever():
            while True:
                try:
                    conn, _addr = listener.accept()
                    held.append(conn)
                except OSError:
                    return

        threading.Thread(target=accept_forever, daemon=True).start()
        vars_b = TMP / "qemu-parse-vars-b.fd"
        shutil.copyfile(vars_tpl, vars_b)
        argv_b = wm.build_qemu_argv(str(disk), code, str(vars_b), memory_mib=512, cpus=2, display_spec="none",
                                    gpu="std", disk_bus="virtio", nic="virtio", netdev="user",
                                    virtio_iso=str(iso), share_sockets=[("work", str(vfs_sock))])
        try:
            exited_b, err_b = parse_run(argv_b)
        finally:
            for conn in held:
                conn.close()
            listener.close()
            vfs_sock.unlink(missing_ok=True)
        self.assertFalse(exited_b, f"virtio/virtiofs VM argv rejected by QEMU: {err_b}")
        print("PASS real QEMU accepts the virtio-blk iothread, virtio-net, virtio-win CD and memfd/vhost-user-fs argv")

    def test_21_guest_storage_guard_and_seed_layout(self):
        cmd, registration = wm.POSTINSTALL_CMD, wm.REGISTER_PS1
        self.assertIn(r'set "LOCACHY=%SystemRoot%\System32\LOCachy"', cmd)
        self.assertIn("locachy-storage", cmd)
        self.assertNotIn("pnputil", cmd)
        self.assertNotIn("msiexec", cmd)
        self.assertIn("-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries", registration)
        self.assertIn("Register-ScheduledTask -TaskName 'LOCachyStorageBoot' -Action $taskAction -Settings $taskSettings", registration)
        self.assertIn("-Trigger (New-ScheduledTaskTrigger -AtStartup) -User 'SYSTEM'", registration)
        self.assertIn("New-ScheduledTaskAction -Execute $powershell -Argument $argsForRepair", registration)
        self.assertIn("$repair = Join-Path $PSScriptRoot 'locachy-win-shutdown.ps1'", registration)
        self.assertIn("Unregister-ScheduledTask -TaskName 'LOCachyKeepBootOrder'", registration)
        self.assertIn("$scripts = Get-IniSection $scriptsPath 'Shutdown'", registration)
        self.assertIn("Write-IniSection $scriptsPath 'Shutdown' $scripts ([Text.Encoding]::Unicode)", registration)
        self.assertIn("gPCMachineExtensionNames=", registration)
        self.assertIn("$extension = '[{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}]'", registration)
        self.assertIn("gpupdate.exe", registration)
        self.assertNotIn("EventID=1074", registration + cmd)
        self.assertIn("locachy-storage.ps1", wm.SHUTDOWN_PS1)
        self.assertIn("StartOverride", wm.STORAGE_PS1)
        self.assertIn("'stornvme', 'viostor'", wm.STORAGE_PS1)
        self.assertIn("-Name Start -Value 0 -Type DWord", wm.STORAGE_PS1)
        seed = TMP / "r21-seed.img"
        wm.build_seed_image(seed)
        self.assertEqual(seed.stat().st_size, 16 * 1024 * 1024)
        ps = subprocess.run(["mtype", "-i", f"{seed}@@{wm.SEED_PART_OFFSET_BYTES}", "::/locachy-storage.ps1"],
                            capture_output=True, text=True, check=True).stdout
        self.assertIn("StartOverride", ps)
        print("PASS guest storage guard: install-time and boot-task StartOverride clearing, shutdown hook, "
              "seed carries locachy-storage.ps1 (boot after a VM run is UNVERIFIED on hardware)")

    def test_22_virtio_iso_viostor_and_netkvm_staged_as_winpedriver(self):
        import shutil

        if not shutil.which("xorriso") or not shutil.which("bsdtar"):
            self.skipTest("needs xorriso and bsdtar")
        tree = TMP / "r22-virtio-tree"
        for drv in ("viostor", "NetKVM"):
            for os_dir in ("w11", "w10"):
                d = tree / drv / os_dir / "amd64"
                d.mkdir(parents=True)
                (d / f"{drv.lower()}.inf").write_text(f"; {drv} {os_dir}\n")
                (d / f"{drv.lower()}.sys").write_bytes(b"MZ-test")
            x86 = tree / drv / "w11" / "x86"
            x86.mkdir(parents=True)
            (x86 / "junk.inf").write_text("x86 build, not staged\n")
        (tree / "viogpudo" / "w11" / "amd64").mkdir(parents=True)
        (tree / "viogpudo" / "w11" / "amd64" / "viogpudo.inf").write_text("not staged\n")
        iso = TMP / "r22-virtio-win.iso"
        subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-r", "-J", "-o", str(iso), str(tree)], check=True)
        seed = TMP / "r22-seed-drv.img"
        wm.build_seed_image(seed, virtio_iso=str(iso), virtio_os="w11")
        self.assertEqual(seed.stat().st_size, 64 * 1024 * 1024)

        def mtext(path):
            return subprocess.run(["mtype", "-i", f"{seed}@@{wm.SEED_PART_OFFSET_BYTES}", path],
                                  capture_output=True, text=True, check=True).stdout

        self.assertEqual(mtext("::/$WinPEDriver$/viostor/viostor.inf"), "; viostor w11\n")
        self.assertEqual(mtext("::/$WinPEDriver$/NetKVM/netkvm.inf"), "; NetKVM w11\n")
        top = subprocess.run(["mdir", "-b", "-i", f"{seed}@@{wm.SEED_PART_OFFSET_BYTES}", "::/$WinPEDriver$"],
                             capture_output=True, text=True, check=True).stdout.lower()
        self.assertIn("viostor", top)
        self.assertIn("netkvm", top)
        self.assertNotIn("viogpudo", top)
        inner = subprocess.run(["mdir", "-b", "-i", f"{seed}@@{wm.SEED_PART_OFFSET_BYTES}", "::/$WinPEDriver$/viostor"],
                               capture_output=True, text=True, check=True).stdout.lower()
        self.assertNotIn("junk", inner)
        only_w11 = TMP / "r22-w11-only-tree"
        (only_w11 / "viostor" / "w11" / "amd64").mkdir(parents=True)
        (only_w11 / "viostor" / "w11" / "amd64" / "viostor.inf").write_text("w11 only\n")
        iso_w11 = TMP / "r22-w11-only.iso"
        subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-r", "-J", "-o", str(iso_w11), str(only_w11)],
                       check=True)
        with self.assertRaisesRegex(RuntimeError, "use --virtio-os"):
            wm.build_seed_image(TMP / "r22-seed-w10.img", virtio_iso=str(iso_w11), virtio_os="w10")
        print("PASS virtio-win viostor and NetKVM amd64 folders staged as $WinPEDriver$ on the seed; "
              "other builds and folders left out")

    def test_23_unprivileged_lsblk_fallback_candidates_status_and_root_layout(self):
        gib = 1024 ** 3
        mib = 1024 ** 2
        nvme0_guid = "11111111-2222-4333-8444-555555555501"
        nvme1_guid = "11111111-2222-4333-8444-555555555502"
        live_guid = "11111111-2222-4333-8444-555555555503"
        root_part_guid = "aaaa1111-2222-4333-8444-555555555501"
        esp_part_guid = "bbbb1111-2222-4333-8444-555555555501"
        msr_part_guid = "bbbb1111-2222-4333-8444-555555555502"
        winc_part_guid = "bbbb1111-2222-4333-8444-555555555503"

        lsblk_tree = {
            "blockdevices": [
                {
                    "path": "/dev/nvme0n1",
                    "type": "disk",
                    "pkname": None,
                    "model": "NVMe Root SSD",
                    "serial": "SER-NVME0",
                    "size": 256 * gib,
                    "pttype": "gpt",
                    "ptuuid": nvme0_guid,
                    "start": None,
                    "partn": None,
                    "partuuid": None,
                    "partlabel": None,
                    "parttype": None,
                    "children": [
                        {
                            "path": "/dev/nvme0n1p1",
                            "type": "part",
                            "pkname": "/dev/nvme0n1",
                            "model": None,
                            "serial": None,
                            "size": 512 * mib,
                            "pttype": "gpt",
                            "ptuuid": nvme0_guid,
                            "start": 2048,
                            "partn": 1,
                            "partuuid": "aaaa0000-2222-4333-8444-555555555500",
                            "partlabel": "LOCACHY_EFI",
                            "parttype": wm.GPT_TYPE_ESP,
                        },
                        {
                            "path": "/dev/nvme0n1p2",
                            "type": "part",
                            "pkname": "/dev/nvme0n1",
                            "model": None,
                            "serial": None,
                            "size": 155 * gib,
                            "pttype": "gpt",
                            "ptuuid": nvme0_guid,
                            "start": 2048 + (512 * mib // 512),
                            "partn": 2,
                            "partuuid": root_part_guid,
                            "partlabel": "locachy-root",
                            "parttype": "0fc63daf-8483-4772-8e79-3d69d8477de4",
                        },
                    ],
                },
                {
                    "path": "/dev/nvme1n1",
                    "type": "disk",
                    "pkname": None,
                    "model": "NVMe Secondary SSD",
                    "serial": "SER-NVME1",
                    "size": 128 * gib,
                    "pttype": "gpt",
                    "ptuuid": nvme1_guid,
                    "start": None,
                    "partn": None,
                    "partuuid": None,
                    "partlabel": None,
                    "parttype": None,
                    "children": [
                        {
                            "path": "/dev/nvme1n1p1",
                            "type": "part",
                            "pkname": "/dev/nvme1n1",
                            "model": None,
                            "serial": None,
                            "size": 16 * gib,
                            "pttype": "gpt",
                            "ptuuid": nvme1_guid,
                            "start": 2048,
                            "partn": 1,
                            "partuuid": "bbbb0000-2222-4333-8444-555555555500",
                            "partlabel": "data",
                            "parttype": "0fc63daf-8483-4772-8e79-3d69d8477de4",
                        },
                        {
                            "path": "/dev/nvme1n1p2",
                            "type": "part",
                            "pkname": "/dev/nvme1n1",
                            "model": None,
                            "serial": None,
                            "size": 128 * mib,
                            "pttype": "gpt",
                            "ptuuid": nvme1_guid,
                            "start": 2048 + (16 * gib // 512),
                            "partn": 2,
                            "partuuid": esp_part_guid,
                            "partlabel": wm.LABEL_ESP,
                            "parttype": wm.GPT_TYPE_ESP,
                        },
                        {
                            "path": "/dev/nvme1n1p3",
                            "type": "part",
                            "pkname": "/dev/nvme1n1",
                            "model": None,
                            "serial": None,
                            "size": 16 * mib,
                            "pttype": "gpt",
                            "ptuuid": nvme1_guid,
                            "start": 2048 + ((16 * gib + 128 * mib) // 512),
                            "partn": 3,
                            "partuuid": msr_part_guid,
                            "partlabel": wm.LABEL_MSR,
                            "parttype": wm.GPT_TYPE_MSR,
                        },
                        {
                            "path": "/dev/nvme1n1p4",
                            "type": "part",
                            "pkname": "/dev/nvme1n1",
                            "model": None,
                            "serial": None,
                            "size": 64 * gib,
                            "pttype": "gpt",
                            "ptuuid": nvme1_guid,
                            "start": 2048 + ((16 * gib + 144 * mib) // 512),
                            "partn": 4,
                            "partuuid": winc_part_guid,
                            "partlabel": wm.LABEL_C,
                            "parttype": wm.GPT_TYPE_BASIC,
                        },
                    ],
                },
                {
                    "path": "/dev/sda",
                    "type": "disk",
                    "pkname": None,
                    "model": "Live ISO USB",
                    "serial": "SER-LIVE",
                    "size": 16 * gib,
                    "pttype": "gpt",
                    "ptuuid": live_guid,
                    "start": None,
                    "partn": None,
                    "partuuid": None,
                    "partlabel": None,
                    "parttype": None,
                    "children": [],
                },
                {
                    "path": "/dev/sdb",
                    "type": "disk",
                    "pkname": None,
                    "model": "Blank Raw Disk",
                    "serial": "SER-BLANK",
                    "size": 500 * gib,
                    "pttype": None,
                    "ptuuid": None,
                    "start": None,
                    "partn": None,
                    "partuuid": None,
                    "partlabel": None,
                    "parttype": None,
                },
            ]
        }
        lsblk_nodeps_disks = {
            "blockdevices": [
                {"path": d["path"], "type": "disk", "model": d["model"],
                 "serial": d["serial"], "size": d["size"]}
                for d in lsblk_tree["blockdevices"]
            ]
        }

        def fake_run_cmd(args, **_kw):
            cmd = list(args)
            if cmd[0] == "findmnt":
                if "--mountpoint" in cmd and cmd[cmd.index("--mountpoint") + 1] == "/":
                    return types.SimpleNamespace(
                        returncode=0,
                        stdout=json.dumps({"filesystems": [{"source": "/dev/mapper/locachy-crypt", "fstype": "ext4"}]}),
                        stderr="",
                    )
                if "--mountpoint" in cmd and cmd[cmd.index("--mountpoint") + 1] == "/run/archiso/bootmnt":
                    return types.SimpleNamespace(returncode=0, stdout="/dev/sda1\n", stderr="")
                return types.SimpleNamespace(returncode=1, stdout="", stderr="")
            if cmd[0] == "cryptsetup":
                return types.SimpleNamespace(returncode=1, stdout="", stderr="permission denied")
            if cmd[0] == "lsblk":
                if "--inverse" in cmd:
                    # Nested children only come with --tree; without it lsblk lists flat rows.
                    if "--tree" not in cmd:
                        raise AssertionError(f"detect_root_layout must ask lsblk --inverse with --tree: {cmd}")
                    return types.SimpleNamespace(
                        returncode=0,
                        stdout=json.dumps({
                            "blockdevices": [{
                                "path": "/dev/mapper/locachy-crypt",
                                "type": "crypt",
                                "pkname": None,
                                "partn": None,
                                "partuuid": None,
                                "pttype": None,
                                "children": [{
                                    "path": "/dev/nvme0n1p2",
                                    "type": "part",
                                    "pkname": "/dev/nvme0n1",
                                    "partn": 2,
                                    "partuuid": root_part_guid,
                                    "pttype": "gpt",
                                }],
                            }]
                        }),
                        stderr="",
                    )
                if "--nodeps" in cmd and cmd[-1] == "/dev/sda1":
                    return types.SimpleNamespace(
                        returncode=0,
                        stdout=json.dumps({"blockdevices": [{"path": "/dev/sda1", "type": "part", "pkname": "/dev/sda"}]}),
                        stderr="",
                    )
                if "--nodeps" in cmd and cmd[-1] == "/dev/nvme0n1p2":
                    # Non-root blkid-only probing yields no UUIDs; the fallback must ask for blkid,udev.
                    if cmd[cmd.index("--properties-by") + 1] != "blkid,udev":
                        raise AssertionError(f"fallback must probe with blkid,udev: {cmd}")
                    return types.SimpleNamespace(
                        returncode=0,
                        stdout=json.dumps({"blockdevices": [{"path": "/dev/nvme0n1p2", "type": "part",
                                                             "pkname": "/dev/nvme0n1", "partn": 2,
                                                             "partuuid": root_part_guid, "pttype": "gpt"}]}),
                        stderr="",
                    )
                if "--nodeps" in cmd:
                    return types.SimpleNamespace(returncode=0, stdout=json.dumps(lsblk_nodeps_disks), stderr="")
                if "--tree" in cmd:
                    if cmd[-1].startswith("/dev/"):
                        target = cmd[-1]
                        filtered = [d for d in lsblk_tree["blockdevices"] if d["path"] == target]
                        return types.SimpleNamespace(
                            returncode=0,
                            stdout=json.dumps({"blockdevices": filtered}),
                            stderr="",
                        )
                    return types.SimpleNamespace(returncode=0, stdout=json.dumps(lsblk_tree), stderr="")
            raise AssertionError(f"unexpected command in non-root mock: {cmd}")

        # State recorded with swapped device path (/dev/nvme0n1), while real GUID lives on /dev/nvme1n1.
        state = {
            "schema": 1,
            "disk": "/dev/nvme0n1",
            "disk_guid": nvme1_guid,
            "on_root_disk": False,
            "root_part_uuid": "",
            "partitions": {
                "esp": {"partn": 2, "part_guid": esp_part_guid, "type_guid": wm.GPT_TYPE_ESP,
                        "start_lba": 2048 + (16 * gib // 512), "sectors": 128 * mib // 512, "name": wm.LABEL_ESP},
                "msr": {"partn": 3, "part_guid": msr_part_guid, "type_guid": wm.GPT_TYPE_MSR,
                        "start_lba": 2048 + ((16 * gib + 128 * mib) // 512), "sectors": 16 * mib // 512, "name": wm.LABEL_MSR},
                "c": {"partn": 4, "part_guid": winc_part_guid, "type_guid": wm.GPT_TYPE_BASIC,
                      "start_lba": 2048 + ((16 * gib + 144 * mib) // 512), "sectors": 64 * gib // 512, "name": wm.LABEL_C},
            },
        }
        state_file = TMP / "r23-nonroot-state.json"
        wm.save_state(state, state_file)

        with mock.patch.object(wm, "parse_gpt_disk", side_effect=PermissionError(13, "Permission denied")), \
             mock.patch.object(wm, "run_cmd", side_effect=fake_run_cmd), \
             mock.patch.object(wm, "STATE_FILE", state_file), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", TMP / "r23-no-plan"), \
             mock.patch.object(wm, "SHRINK_STATUS_VAR", TMP / "r23-no-status"), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "r23-no.lock"):
            # Direct candidate_disks() works without raw block read permission.
            cands = wm.candidate_disks(root_disk="/dev/nvme0n1")
            self.assertEqual([c["path"] for c in cands], ["/dev/nvme0n1", "/dev/nvme1n1"])
            c0, c1 = cands
            self.assertTrue(c0["is_root"])
            self.assertEqual(c0["disk_guid"], nvme0_guid)
            self.assertEqual(c0["serial"], "SER-NVME0")
            self.assertEqual(c0["free_tail_gib"], 100.5)
            self.assertFalse(c0["has_maintenance_windows"])

            self.assertFalse(c1["is_root"])
            self.assertEqual(c1["disk_guid"], nvme1_guid)
            self.assertEqual(c1["serial"], "SER-NVME1")
            self.assertAlmostEqual(c1["free_tail_gib"], 47.86, places=2)
            self.assertTrue(c1["has_maintenance_windows"])

            # Unprivileged detect_root_layout() succeeds on LUKS root without cryptsetup/raw disk read.
            root_layout = wm.detect_root_layout(allow_lsblk_fallback=True)
            self.assertEqual(root_layout["disk"], "/dev/nvme0n1")
            self.assertEqual(root_layout["partn"], 2)
            self.assertEqual(root_layout["part_uuid"], root_part_guid)
            self.assertTrue(root_layout["is_luks"])

            # status --json populates candidate_disks (marking root), resolved_disk, and gpt_verified.
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = wm.main(["status", "--json"])
            self.assertEqual(rc, 0)
            info = json.loads(out.getvalue())
            self.assertTrue(info["gpt_verified"], info)
            self.assertEqual(info["resolved_disk"], "/dev/nvme1n1")
            self.assertEqual(info["esp_dev"], "/dev/nvme1n1p2")
            self.assertEqual(info["msr_dev"], "/dev/nvme1n1p3")
            self.assertEqual(info["c_dev"], "/dev/nvme1n1p4")
            self.assertEqual(info["win_c_gib"], 64.0)
            self.assertEqual([c["path"] for c in info["candidate_disks"]], ["/dev/nvme0n1", "/dev/nvme1n1"])
            self.assertTrue(info["candidate_disks"][0]["is_root"])
            self.assertFalse(info["candidate_disks"][1]["is_root"])

            # Privileged resolve_state_disk() MUST NOT fall back to lsblk when raw GPT open is denied.
            with self.assertRaisesRegex(RuntimeError, "Disk GUID mismatch"):
                wm.resolve_state_disk(state)
            # Duplicate Disk GUID still fails closed under lsblk fallback.
            dup_tree = json.loads(json.dumps(lsblk_tree))
            dup_tree["blockdevices"][0]["ptuuid"] = nvme1_guid
            with mock.patch.object(wm, "_lsblk_gpt_map", return_value={
                "/dev/nvme0n1": wm._gpt_from_lsblk_node(dup_tree["blockdevices"][0], dup_tree["blockdevices"][0]["children"]),
                "/dev/nvme1n1": wm._gpt_from_lsblk_node(dup_tree["blockdevices"][1], dup_tree["blockdevices"][1]["children"]),
            }):
                with self.assertRaisesRegex(RuntimeError, "Ambiguous disk GUID"):
                    wm.resolve_state_disk(state, allow_lsblk_fallback=True)
        print("PASS unprivileged lsblk GPT fallback populates candidate_disks, root is_root, resolved_disk and "
              "gpt_verified while privileged resolve_state_disk stays raw-only")

    def test_24_boot_default_state_status_guard_heal_and_boot_once(self):
        disk = self.make_phys_disk("bd-disk.img", size_gib=64, root_gib=8)
        gpt, triplet = wm.append_tail_triplet_sfdisk(disk, 48, is_image=True)
        state_file = TMP / "bd-state.json"
        wm.save_state(wm.state_from_gpt(gpt, disk, on_root_disk=False), state_file)
        p4 = triplet["esp"]["part_guid"]
        efi_path = str(wm.efi_helper_path())
        boot_id = TMP / "bd-boot-id"
        boot_id.write_text("boot-a\n")
        esp = TMP / "bd-esp"
        (esp / "EFI/locachy").mkdir(parents=True)
        (esp / "EFI/locachy/locachy.efi").write_bytes(b"MZ")
        efi_dir = TMP / "bd-efi"
        efi_dir.mkdir()
        calls = []
        replies = {}
        reboots = []

        def fake_run(args, **kw):
            args = list(args)
            calls.append(args)
            rc = 0
            if args[0] == efi_path and "--ensure-first" in args:
                rc = replies.get("ensure", 0)
            if args[0] == efi_path and "--windows-first" in args:
                rc = replies.get("windows-first", 0)
            if kw.get("check", True) and rc:
                raise RuntimeError(f"{' '.join(args)}: exit {rc}")
            return types.SimpleNamespace(returncode=rc, stdout="", stderr="")

        def run(argv):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = wm.main(argv)
            return rc, out.getvalue(), err.getvalue()

        def status():
            rc, out, _ = run(["status", "--json"])
            self.assertEqual(rc, 0)
            return json.loads(out)

        def efi_calls(option):
            return [c for c in calls if c[0] == efi_path and option in c]

        with contextlib.ExitStack() as stack:
            for target, value in (("LOCK_PATH", TMP / "bd.lock"), ("BOOT_ID_PATH", boot_id),
                                  ("STATE_FILE", state_file), ("EFI_DIR", efi_dir),
                                  ("SHRINK_PLAN_FILE", TMP / "bd-no-plan"),
                                  ("SHRINK_STATUS_VAR", TMP / "bd-no-status")):
                stack.enter_context(mock.patch.object(wm, target, value))
            stack.enter_context(mock.patch.object(wm, "run_cmd", side_effect=fake_run))
            stack.enter_context(mock.patch.object(wm, "detect_root_layout", side_effect=RuntimeError("no root")))
            stack.enter_context(mock.patch.object(wm, "candidate_disks", return_value=[]))
            stack.enter_context(mock.patch.object(wm, "sanitize_esp_blockdev",
                                                  return_value={"removed": [], "bootmgfw_present": True}))
            stack.enter_context(mock.patch.object(wm, "request_reboot", side_effect=lambda: reboots.append(1)))
            stack.enter_context(mock.patch.object(wm.os, "geteuid", return_value=0))

            info = status()
            self.assertEqual((info["boot_default"], info["boot_default_pending_reboot"],
                              info["boot_default_overridden"]), ("locachy", False, False))

            # A failed firmware write records nothing.
            replies["windows-first"] = 1
            self.assertEqual(run(["boot-default", "windows", "--esp", str(esp)])[0], 1)
            self.assertNotIn("boot_default", wm.load_state(state_file))
            replies.pop("windows-first")

            calls.clear()
            self.assertEqual(run(["boot-default", "windows", "--esp", str(esp)])[0], 0)
            self.assertEqual(calls, [[efi_path, "--windows-first", "--disk", str(disk), "--part", "4",
                                      "--partuuid", p4, "--loader", wm.WIN_EFI_LOADER, "--esp", str(esp)]])
            saved = wm.load_state(state_file)
            self.assertEqual((saved["boot_default"], saved["boot_default_boot_id"]), ("windows", "boot-a"))
            info = status()
            self.assertEqual((info["boot_default"], info["boot_default_pending_reboot"],
                              info["boot_default_overridden"]), ("windows", True, False))

            # LOCachy picked in the firmware menu (another boot): status says so.
            boot_id.write_text("boot-b\n")
            info = status()
            self.assertEqual((info["boot_default_pending_reboot"], info["boot_default_overridden"]), (False, True))
            self.assertIn("picked in the firmware boot menu", run(["status"])[1])

            # LOCachy's boot-time guard and heal-boot keep Windows first; boot-once arms nothing.
            calls.clear()
            self.assertEqual(run(["boot-guard", "--esp", str(esp)])[0], 0)
            self.assertEqual(run(["heal-boot", "--esp", str(esp)])[0], 0)
            ensure = efi_calls("--ensure-first")
            self.assertEqual(len(ensure), 2)
            for c in ensure:
                self.assertEqual(c[c.index("--winmaint-guid") + 1], p4)
                self.assertIn("--keep-windows-first", c)
            self.assertEqual(efi_calls("--remove-entry"), [])
            calls.clear()
            rc, out, _ = run(["boot-once", "--esp", str(esp)])
            self.assertEqual((rc, calls), (0, []))
            self.assertIn("already starts first", out)
            self.assertEqual(wm.load_state(state_file)["boot_default"], "windows")

            # Return to LOCachy in Windows put LOCachy first: the next guard run ends the mode.
            replies["ensure"] = wm.EFI_RELEASED
            calls.clear()
            self.assertEqual(run(["boot-guard", "--esp", str(esp)])[0], 0)
            self.assertEqual(efi_calls("--remove-entry"),
                             [[efi_path, "--remove-entry", "--label", wm.WIN_FIRST_LABEL]])
            saved = wm.load_state(state_file)
            self.assertNotIn("boot_default", saved)
            self.assertNotIn("boot_default_boot_id", saved)
            self.assertEqual(status()["boot_default"], "locachy")
            replies.pop("ensure")

            # Switching back from LOCachy: plain ensure-first, the entry deleted, the mode cleared.
            self.assertEqual(run(["boot-default", "windows", "--esp", str(esp)])[0], 0)
            calls.clear()
            self.assertEqual(run(["boot-default", "locachy", "--esp", str(esp)])[0], 0)
            self.assertEqual(calls, [[efi_path, "--ensure-first", "--esp", str(esp), "--winmaint-guid", p4],
                                     [efi_path, "--remove-entry", "--label", wm.WIN_FIRST_LABEL]])
            self.assertNotIn("boot_default", wm.load_state(state_file))
            calls.clear()
            self.assertEqual(run(["heal-boot", "--esp", str(esp)])[0], 0)
            self.assertNotIn("--keep-windows-first", efi_calls("--ensure-first")[0])
            # Every removal names the Windows-first label: LOCachy's own entries are never removed.
            self.assertFalse(any(c[c.index("--label") + 1] != wm.WIN_FIRST_LABEL for c in efi_calls("--remove-entry")))

            # Reboot into Windows now.
            self.assertEqual(run(["boot-default", "windows", "--reboot", "--esp", str(esp)])[0], 0)
            self.assertEqual(reboots, [1])

            # The VM (or another disk operation) holding the lock refuses.
            with wm.WinmaintLock():
                rc, _, err = run(["boot-default", "locachy", "--esp", str(esp)])
            self.assertEqual(rc, 1)
            self.assertIn("lock is busy", err)
        print("PASS boot-default: firmware first then state, status pending/overridden, boot-guard/heal-boot keep "
              "Windows first, boot-once arms nothing, a release from Windows clears the mode, switching back")

    def test_25_windows_return_shortcut_and_guard_scripts(self):
        import re as _re
        import shutil
        ret, guard, cmd = wm.RETURN_PS1, wm.SHUTDOWN_PS1, wm.POSTINSTALL_CMD
        self.assertEqual(wm.WIN_FIRST_LABEL, efi.WINFIRST_LABEL)
        code = [line for line in ret.splitlines() if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(code[0], "param([switch]$InstallShortcut)")
        for needle in ("$lines = bcdedit /enum firmware /v",
                       "$fwMgr = '{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}'",
                       "if ($val -ceq 'LOCachy') {",
                       "} elseif ($val -ceq 'LOCachy Maintenance Windows (default)') {",
                       "bcdedit /set '{fwbootmgr}' displayorder $fw.LOCachy /addfirst\n",
                       "bcdedit /set '{fwbootmgr}' displayorder $id /remove | Out-Null",
                       "$link = $shell.CreateShortcut((Join-Path $env:PUBLIC 'Desktop\\Return to LOCachy.lnk'))",
                       "Start-Process -FilePath $powershell -Verb RunAs",
                       "[Security.Principal.WindowsBuiltInRole]::Administrator",
                       "$guard = Join-Path $PSScriptRoot 'locachy-win-shutdown.ps1'",
                       "$answer = Read-Host 'Restart now? [y/N]'",
                       "shutdown.exe /r /t 0"):
            self.assertIn(needle, ret)
        self.assertLess(ret.index("/addfirst"), ret.index("/remove"))  # LOCachy first, then the guard re-armed
        self.assertLess(ret.index("/remove"), ret.index("Restart now?"))
        # The guard: stands down while the (default) entry precedes LOCachy; storage still runs.
        for needle in ("if (-not (Test-LOCachyWindowsFirst $fw)) {",
                       "foreach ($wid in @($fw.WinMaint) + @($fw.WinFirst)) {",
                       "bcdedit /set '{fwbootmgr}' displayorder $wid /remove | Out-Null",
                       "bcdedit /set '{fwbootmgr}' displayorder $fw.LOCachy /addfirst | Out-Null",
                       "if ($i -ge 0 -and ($loc -lt 0 -or $i -lt $loc)) { return $true }"):
            self.assertIn(needle, guard)
        self.assertLess(guard.index("if (-not (Test-LOCachyWindowsFirst"), guard.index("& $storage"))
        self.assertGreater(guard.index("& $storage"), guard.rindex("\n}\n"))

        # Parser must never match localized English field labels (identifier/displayorder/description).
        fn_src = wm.FIRMWARE_PS_FUNCTIONS
        regexes = _re.findall(r"-match\s+'([^']+)'", fn_src)
        self.assertEqual(len(regexes), 7, regexes)
        for rx in regexes:
            for forbidden in ("identifier", "displayorder", "description"):
                self.assertNotIn(forbidden, rx.lower(), rx)

        # Mirror Get-LOCachyFirmware + Test-LOCachyWindowsFirst using the exact regexes extracted from PowerShell.
        rx_blank, rx_sep, rx_id, rx_ord0, rx_ord1, rx_val2, rx_val1 = [_re.compile(r) for r in regexes]

        def parse_fw(text):
            fw = {"Order": [], "LOCachy": None, "WinMaint": [], "WinFirst": []}
            fw_mgr = "{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}"
            current = None
            is_fw = False
            order_state = 0
            for line in text.splitlines():
                if rx_blank.search(line) or rx_sep.search(line):
                    current, is_fw, order_state = None, False, 0
                    continue
                if not current:
                    m = rx_id.search(line)
                    if m:
                        current = m.group(1).lower()
                        is_fw = current in ("{fwbootmgr}", fw_mgr)
                        order_state = 0
                    continue
                if is_fw:
                    if order_state == 0:
                        m = rx_ord0.search(line)
                        if m:
                            fw["Order"].append(m.group(1).lower())
                            order_state = 1
                        else:
                            order_state = 2
                        continue
                    if order_state == 1:
                        m = rx_ord1.search(line)
                        if m:
                            fw["Order"].append(m.group(1).lower())
                        else:
                            order_state = 2
                    continue
                m = rx_val2.search(line) or rx_val1.search(line)
                if not m:
                    continue
                val = m.group(1)
                if val == "LOCachy":
                    fw["LOCachy"] = current
                elif val == "LOCachy Maintenance Windows":
                    fw["WinMaint"].append(current)
                elif val == "LOCachy Maintenance Windows (default)":
                    fw["WinFirst"].append(current)
            return fw

        def win_first(fw):
            loc = fw["Order"].index(fw["LOCachy"]) if fw["LOCachy"] in fw["Order"] else -1
            for wid in fw["WinFirst"]:
                if wid in fw["Order"]:
                    i = fw["Order"].index(wid)
                    if loc < 0 or i < loc:
                        return True
            return False

        loc_id = "{11111111-2222-3333-4444-555555555501}"
        wf_id = "{11111111-2222-3333-4444-555555555502}"
        wm_id = "{11111111-2222-3333-4444-555555555503}"
        prev_id = "{11111111-2222-3333-4444-555555555504}"
        other_id = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}"

        en_win_first = f"""
Firmware Boot Manager
---------------------
identifier              {{fwbootmgr}}
displayorder            {wf_id.upper()}
                        {loc_id}
                        {prev_id}
bootsequence            {other_id}
timeout                 2

Windows Boot Manager
--------------------
identifier              {other_id}
device                  partition=\\Device\\HarddiskVolume1
path                    \\EFI\\Microsoft\\Boot\\bootmgfw.efi
description             Windows Boot Manager

Firmware Application (101fffff)
-------------------------------
identifier              {loc_id.upper()}
device                  partition=\\Device\\HarddiskVolume1
path                    \\EFI\\LOCachy\\locachy.efi
description             LOCachy

Firmware Application (101fffff)
-------------------------------
identifier              {prev_id}
description             LOCachy (Previous)

Firmware Application (101fffff)
-------------------------------
identifier              {wf_id}
device                  partition=\\Device\\HarddiskVolume4
path                    \\EFI\\Microsoft\\Boot\\bootmgfw.efi
description             LOCachy Maintenance Windows (default)

Firmware Application (101fffff)
-------------------------------
identifier              {wm_id}
description             LOCachy Maintenance Windows
"""
        fw_en = parse_fw(en_win_first)
        self.assertEqual(fw_en["Order"], [wf_id, loc_id, prev_id])
        self.assertEqual(fw_en["LOCachy"], loc_id)
        self.assertEqual(fw_en["WinFirst"], [wf_id])
        self.assertEqual(fw_en["WinMaint"], [wm_id])
        self.assertTrue(win_first(fw_en))

        # Simulated Chinese Windows `bcdedit /enum firmware /v` with canonical {fwbootmgr} GUID.
        zh_win_first = f"""
固件启动管理器
--------------------
标识符                  {{A5A30FA2-3D06-4E9F-B5F4-A01DF9D1FCBA}}
显示顺序                {wf_id}
                        {loc_id}
                        {other_id}
超时                    0

固件应用程序 (101fffff)
-------------------------------
标识符                  {loc_id}
设备                    partition=\\Device\\HarddiskVolume1
路径                    \\EFI\\locachy\\locachy.efi
描述                    LOCachy

固件应用程序 (101fffff)
-------------------------------
标识符                  {wf_id}
描述                    LOCachy Maintenance Windows (default)

固件应用程序 (101fffff)
-------------------------------
标识符                  {wm_id}
描述                    LOCachy Maintenance Windows
"""
        fw_zh = parse_fw(zh_win_first)
        self.assertEqual(fw_zh["Order"], [wf_id, loc_id, other_id])
        self.assertEqual(fw_zh["LOCachy"], loc_id)
        self.assertEqual(fw_zh["WinFirst"], [wf_id])
        self.assertEqual(fw_zh["WinMaint"], [wm_id])
        self.assertTrue(win_first(fw_zh))

        # Chinese Windows after Return to LOCachy (LOCachy first): guard must re-arm (not Windows-first).
        zh_loc_first = zh_win_first.replace(
            f"显示顺序                {wf_id}\n                        {loc_id}",
            f"显示顺序                {loc_id}\n                        {wf_id}",
        )
        self.assertNotEqual(zh_loc_first, zh_win_first)  # the reordered fixture really differs
        fw_zh_back = parse_fw(zh_loc_first)
        self.assertEqual(fw_zh_back["Order"], [loc_id, wf_id, other_id])
        self.assertFalse(win_first(fw_zh_back))

        # Multi-word localized label (e.g. German/French) + single-space separator fallback.
        multi_word = f"""
Start-Manager fuer Firmware
---------------------------
Eindeutiger Bezeichner  {{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}}
Reihenfolge der Anzeige {loc_id}
Zeitlimit               1

Firmwareanwendung (101fffff)
----------------------------
Eindeutiger Bezeichner  {loc_id}
Beschreibung des Eintrags  LOCachy

Firmwareanwendung (101fffff)
----------------------------
Bezeichner {wf_id}
描述 LOCachy Maintenance Windows (default)
"""
        fw_mw = parse_fw(multi_word)
        self.assertEqual(fw_mw["Order"], [loc_id])
        self.assertEqual(fw_mw["LOCachy"], loc_id)
        self.assertEqual(fw_mw["WinFirst"], [wf_id])
        self.assertFalse(win_first(fw_mw))

        self.assertIn('for %%s in (locachy-win-shutdown locachy-storage locachy-return locachy-register)', cmd)
        self.assertIn(r'copy /Y "%SEED%\%%s.ps1" "%LOCACHY%\%%s.ps1" >nul || exit /b 1', cmd)
        self.assertIn(r'-File "%LOCACHY%\locachy-register.ps1"', cmd)
        self.assertIn("-InstallShortcut", wm.REGISTER_PS1)
        for text in (ret, guard, cmd, wm.STORAGE_PS1, (SRC / "link/locachy-winmaint").read_text(encoding="utf-8")):
            text.encode("ascii")
        seed = TMP / "r25-seed.img"
        wm.build_seed_image(seed)
        raw = subprocess.run(["mtype", "-i", f"{seed}@@{wm.SEED_PART_OFFSET_BYTES}", "::/locachy-return.ps1"],
                             capture_output=True, check=True).stdout
        self.assertIn(b"\r\n", raw)
        self.assertEqual(raw.decode("ascii").replace("\r\n", "\n"), ret)
        pwsh = shutil.which("pwsh")
        if pwsh:
            for name, text in (("return", ret), ("guard", guard),
                               ("register", wm.REGISTER_PS1), ("storage", wm.STORAGE_PS1)):
                path = TMP / f"r25-{name}.ps1"
                path.write_text(text)
                res = subprocess.run([pwsh, "-NoProfile", "-Command",
                                      "$e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile("
                                      f"'{path}', [ref]$null, [ref]$e); if ($e.Count) {{ $e | % {{ $_.ToString() }}; exit 1 }}"],
                                     capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            print("PASS PowerShell parser accepts the Return to LOCachy and guard scripts")
        else:
            print("SKIP PowerShell parse of the Windows scripts (no pwsh on this host)")
        print("PASS static Windows checks: firmware parsing fixtures (English, Chinese, multi-word labels), "
              "Return to LOCachy shortcut and ASCII seed scripts")

    def test_26_driver_seed_image_holds_only_winpedriver(self):
        import shutil
        tree = TMP / "r26-virtio-tree"
        for drv in ("viostor", "NetKVM"):
            for os_dir in ("w11", "w10"):
                # Place the primary file under amd64/<os>/ and hard-link from <drv>/<os>/amd64/,
                # mirroring real virtio-win.iso's deduplicated ISO-9660 shared-extent hard links.
                prim = tree / "amd64" / os_dir
                prim.mkdir(parents=True, exist_ok=True)
                inf_p = prim / f"{drv.lower()}.inf"
                sys_p = prim / f"{drv.lower()}.sys"
                inf_p.write_text(f"; {drv} {os_dir}\n")
                sys_p.write_bytes(b"MZ-test")
                d = tree / drv / os_dir / "amd64"
                d.mkdir(parents=True)
                os.link(inf_p, d / f"{drv.lower()}.inf")
                os.link(sys_p, d / f"{drv.lower()}.sys")
        (tree / "viogpudo/w11/amd64").mkdir(parents=True)
        (tree / "viogpudo/w11/amd64/viogpudo.inf").write_text("not staged\n")
        out_img = TMP / "r26-out" / "drivers.img"
        if not shutil.which("xorriso") or not shutil.which("bsdtar"):
            self.skipTest("needs xorriso and bsdtar")
        iso = TMP / "r26-virtio-win.iso"
        subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-r", "-J", "-o", str(iso), str(tree)], check=True)
        # Confirm the test ISO actually encodes hard links (matching real virtio-win.iso)
        tvf = subprocess.run(["bsdtar", "-tvf", str(iso)], capture_output=True, text=True, check=True).stdout
        self.assertIn(" link to ", tvf, "expected Rock Ridge hard links in test virtio-win ISO")
        sources = [iso]

        def mtext(path):
            return subprocess.run(["mtype", "-i", f"{out_img}@@{wm.SEED_PART_OFFSET_BYTES}", path],
                                  capture_output=True, text=True, check=True).stdout

        def listing(path):
            return subprocess.run(["mdir", "-b", "-i", f"{out_img}@@{wm.SEED_PART_OFFSET_BYTES}", path],
                                  capture_output=True, text=True, check=True).stdout.lower()

        for src in sources:
            for os_dir in ("w11", "w10"):
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    rc = wm.main(["driver-seed", "--virtio-iso", str(src), "--out", str(out_img),
                                  "--virtio-os", os_dir])
                self.assertEqual(rc, 0)
                self.assertEqual(buf.getvalue(), f"{out_img}\n")
                self.assertEqual(out_img.stat().st_size, 16 * 1024 * 1024)
                with open(out_img, "rb") as f:
                    mbr = f.read(512)
                    self.assertEqual(mbr[510:512], b"\x55\xaa")
                    self.assertEqual(mbr[446 + 4], 0x0E)  # FAT16 LBA partition type
                    f.seek(wm.SEED_PART_OFFSET_BYTES)
                    self.assertEqual(f.read(62)[54:62], b"FAT16   ")
                top = [line for line in listing("::/").splitlines() if line.strip()]
                self.assertEqual(len(top), 1, top)
                self.assertIn("$winpedriver$", top[0])
                self.assertEqual(mtext("::/$WinPEDriver$/viostor/viostor.inf"), f"; viostor {os_dir}\n")
                self.assertEqual(mtext("::/$WinPEDriver$/NetKVM/netkvm.inf"), f"; NetKVM {os_dir}\n")
                drivers = listing("::/$WinPEDriver$")
                self.assertIn("viostor", drivers)
                self.assertIn("netkvm", drivers)
                self.assertNotIn("viogpudo", drivers)
        if shutil.which("fsck.fat"):
            part_copy = TMP / "r26-part.fat"
            with open(out_img, "rb") as src_f, open(part_copy, "wb") as dst_f:
                src_f.seek(wm.SEED_PART_OFFSET_BYTES)
                shutil.copyfileobj(src_f, dst_f)
            res = subprocess.run(["fsck.fat", "-n", str(part_copy)], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        missing = TMP / "r26-missing.img"
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(wm.main(["driver-seed", "--virtio-iso", str(TMP / "no-such.iso"), "--out", str(missing)]), 1)
        self.assertIn("virtio-win ISO not found", err.getvalue())
        self.assertFalse(missing.exists())
        for not_regular in (tree, Path("/dev/null")):  # a directory, a device
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(wm.main(["driver-seed", "--virtio-iso", str(not_regular), "--out", str(missing)]), 1)
            self.assertIn("must be a regular file", err.getvalue())
            self.assertFalse(missing.exists())
        print("PASS driver-seed: FAT16 image with only $WinPEDriver$ viostor and NetKVM (amd64, w11/w10) from a "
              "virtio-win ISO, prints its absolute path; directories and devices refused")

    def test_27_vm_asks_locachy_winiso_only_without_options(self):
        disk_img = self.make_phys_disk("r27.img", size_gib=64, root_gib=8)
        gpt, _triplet = wm.append_tail_triplet_sfdisk(disk_img, 48, is_image=True)
        state_file = TMP / "r27-state.json"
        wm.save_state(wm.state_from_gpt(gpt, disk_img, on_root_disk=False), state_file)
        code = TMP / "r27-code.fd"
        code.write_bytes(b"c")
        vars_tpl = TMP / "r27-vars.fd"
        vars_tpl.write_bytes(b"v")
        win_iso = TMP / "r27-win11.iso"
        win_iso.write_bytes(b"iso")
        own_iso = TMP / "r27-own.iso"
        own_iso.write_bytes(b"iso")
        log = TMP / "r27-winiso.log"

        def stub(name, body):
            path = TMP / name
            path.write_text(f'#!/bin/sh\necho "$*" >> "{log}"\n{body}\n')
            path.chmod(0o755)
            return path

        helper = stub("r27-locachy-winiso",
                      f'[ "$1" = path ] || exit 2\ncase "$2" in\n  windows) echo "{win_iso}"; exit 0 ;;\n'
                      '  virtio-win) exit 3 ;;\nesac\nexit 2')

        def dry(extra, helper_path, installed=None):
            log.unlink(missing_ok=True)
            out = io.StringIO()
            with mock.patch.object(wm, "STATE_FILE", state_file), \
                 mock.patch.object(wm, "LOCK_PATH", TMP / "r27.lock"), \
                 mock.patch.object(wm, "OVMF_CODE_CANDIDATES", (str(code),)), \
                 mock.patch.object(wm, "OVMF_VARS_CANDIDATES", (str(vars_tpl),)), \
                 mock.patch.object(wm, "qemu_device_names", return_value={"virtio-vga"}), \
                 mock.patch.object(wm, "choose_netdev", return_value="user"), \
                 mock.patch.object(wm, "winiso_helper_path", return_value=helper_path), \
                 mock.patch.object(wm, "windows_installed", return_value=installed), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(wm.main(["vm", "--dry-run", "--no-tpm", "--display", "none", *extra]), 0)
            asked = log.read_text().splitlines() if log.exists() else []
            return " ".join(json.loads(out.getvalue())["qemu_argv"]), asked

        argv, asked = dry([], None)  # no locachy-winiso: today's behaviour
        self.assertNotIn("id=cd0", argv)
        self.assertNotIn("id=seed0", argv)
        argv, asked = dry([], helper)
        self.assertEqual(asked, ["path windows", "path virtio-win"])
        self.assertIn(f"file={win_iso},media=cdrom,readonly=on,if=none,id=cd0", argv)
        self.assertIn("id=seed0", argv)
        self.assertNotIn("id=cd1", argv)  # exit 3: no virtio-win ISO
        argv, asked = dry(["--iso", str(own_iso)], helper)  # an explicit option wins
        self.assertEqual(asked, ["path virtio-win"])
        self.assertIn(f"file={own_iso},media=cdrom", argv)
        argv, asked = dry([], helper, installed=True)  # Windows installed: nothing attached unasked
        self.assertEqual(asked, [])
        self.assertNotIn("id=cd0", argv)

        with mock.patch.object(wm, "winiso_helper_path", return_value=helper):
            self.assertEqual(wm.winiso_path("windows"), str(win_iso))
            self.assertIsNone(wm.winiso_path("virtio-win"))
        for body in ('echo relative.iso; exit 0', f'echo "{TMP}/r27-absent.iso"; exit 0', 'echo broken >&2; exit 1',
                     f'echo "{win_iso}"; echo "{own_iso}"; exit 0'):
            bad = stub("r27-bad-winiso", body)
            err = io.StringIO()
            with mock.patch.object(wm, "winiso_helper_path", return_value=bad), contextlib.redirect_stderr(err):
                self.assertIsNone(wm.winiso_path("windows"))
            self.assertIn("locachy-winiso path windows", err.getvalue())
        print("PASS vm asks locachy-winiso path windows / virtio-win only without --iso / --virtio-iso and before "
              "Windows is installed; exit 3 and unusable answers attach nothing; absent command keeps today's behaviour")

    def test_28_root_disk_record_and_regular_file_iso_paths(self):
        # create --root --disk D where D holds the target root: recorded as this system's root.
        root_disk = self.make_phys_disk("r28-root.img", size_gib=64, root_gib=8)
        gpt0 = wm.parse_gpt_disk(root_disk)
        root_guid = gpt0["partitions"][2]["part_guid"]
        other = self.make_phys_disk("r28-other.img", size_gib=64, root_gib=8)
        target = TMP / "r28-installed"
        state_path = wm.state_path_for_root(target)
        seen = []

        def fake_detect(*, allow_lsblk_fallback=False, mountpoint="/"):
            seen.append(str(mountpoint))
            if str(mountpoint) != str(target):
                raise RuntimeError("no root here")
            return {"disk": str(root_disk), "partn": 3, "part_uuid": root_guid, "is_luks": False,
                    "root_fs_dev": "/dev/mapper/r28", "gpt": gpt0, "part_sectors": gpt0["partitions"][2]["sectors"],
                    "luks_payload_offset_sectors": 32768}

        real_append = wm.append_tail_triplet_sfdisk
        real_format = wm.format_winmaint_esp_and_msr

        def append_image(disk_path, win_c_gib, **kw):
            return real_append(disk_path, win_c_gib, is_image=True)

        def format_as_image(disk_path, triplet, is_image=False):
            return real_format(disk_path, triplet, is_image=True)

        quiet = io.StringIO()
        with mock.patch.object(wm.os, "geteuid", return_value=0), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "r28.lock"), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", TMP / "r28-no-plan"), \
             mock.patch.object(wm, "live_medium_disks", return_value=set()), \
             mock.patch.object(wm, "detect_root_layout", side_effect=fake_detect), \
             mock.patch.object(wm, "append_tail_triplet_sfdisk", side_effect=append_image), \
             mock.patch.object(wm, "format_winmaint_esp_and_msr", side_effect=format_as_image), \
             contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(wm.main(["create", "--root", str(target), "--disk", str(root_disk), "--size-gib", "48"]), 0)
        self.assertEqual(seen, [str(target)])
        state = wm.load_state(state_path)
        self.assertTrue(state["on_root_disk"])
        self.assertEqual(state["root_part_uuid"], root_guid)
        # reclaim then grows that root partition into the freed space (the case that stayed unallocated).
        res = wm.reclaim_tail_triplet(root_disk, state, is_image=True, grow_root=True)
        self.assertTrue(res["grew_root"])

        # The same call with a disk that holds no root partition of the target stays a secondary disk.
        target2 = TMP / "r28-installed-2"
        with mock.patch.object(wm.os, "geteuid", return_value=0), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "r28b.lock"), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", TMP / "r28-no-plan-2"), \
             mock.patch.object(wm, "live_medium_disks", return_value=set()), \
             mock.patch.object(wm, "detect_root_layout", side_effect=RuntimeError("not this root")), \
             mock.patch.object(wm, "append_tail_triplet_sfdisk", side_effect=append_image), \
             mock.patch.object(wm, "format_winmaint_esp_and_msr", side_effect=format_as_image), \
             contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(wm.main(["create", "--root", str(target2), "--disk", str(other), "--size-gib", "48"]), 0)
        state2 = wm.load_state(wm.state_path_for_root(target2))
        self.assertFalse(state2["on_root_disk"])
        self.assertEqual(state2["root_part_uuid"], "")

        # --root never shrinks a root partition: a disk without the free tail is refused, nothing armed.
        small_root = self.make_phys_disk("r28-small-root.img", size_gib=20, root_gib=8)
        small_gpt = wm.parse_gpt_disk(small_root)
        target3 = TMP / "r28-installed-3"
        plan = TMP / "r28-plan-3"

        def fake_small(*, allow_lsblk_fallback=False, mountpoint="/"):
            return {"disk": str(small_root), "partn": 3, "part_uuid": small_gpt["partitions"][2]["part_guid"],
                    "is_luks": False, "root_fs_dev": "/dev/mapper/r28s", "gpt": small_gpt,
                    "part_sectors": small_gpt["partitions"][2]["sectors"], "luks_payload_offset_sectors": 32768}

        err = io.StringIO()
        with mock.patch.object(wm.os, "geteuid", return_value=0), \
             mock.patch.object(wm, "LOCK_PATH", TMP / "r28c.lock"), \
             mock.patch.object(wm, "SHRINK_PLAN_FILE", plan), \
             mock.patch.object(wm, "live_medium_disks", return_value=set()), \
             mock.patch.object(wm, "detect_root_layout", side_effect=fake_small), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            self.assertEqual(wm.main(["create", "--root", str(target3), "--disk", str(small_root), "--size-gib", "48"]), 1)
        self.assertIn("unallocated tail space", err.getvalue())
        self.assertFalse(plan.exists())
        self.assertFalse(wm.state_path_for_root(target3).exists())

        # ISO paths must be regular files: directories and devices are refused everywhere.
        iso_dir = TMP / "r28-iso-dir"
        iso_dir.mkdir()
        disk_img = self.make_phys_disk("r28-vm.img", size_gib=64, root_gib=8)
        gpt, _t = wm.append_tail_triplet_sfdisk(disk_img, 48, is_image=True)
        state_file = TMP / "r28-vm-state.json"
        wm.save_state(wm.state_from_gpt(gpt, disk_img, on_root_disk=False), state_file)
        code = TMP / "r28-code.fd"
        code.write_bytes(b"c")
        vars_tpl = TMP / "r28-vars.fd"
        vars_tpl.write_bytes(b"v")
        common = dict(STATE_FILE=state_file, LOCK_PATH=TMP / "r28v.lock", OVMF_CODE_CANDIDATES=(str(code),),
                      OVMF_VARS_CANDIDATES=(str(vars_tpl),))
        for extra in (["--iso", str(iso_dir)], ["--iso", "/dev/null"], ["--virtio-iso", str(iso_dir)],
                      ["--virtio-iso", "/dev/null"]):
            err = io.StringIO()
            with contextlib.ExitStack() as stack:
                for name, value in common.items():
                    stack.enter_context(mock.patch.object(wm, name, value))
                stack.enter_context(mock.patch.object(wm, "qemu_device_names", return_value={"virtio-vga"}))
                stack.enter_context(mock.patch.object(wm, "winiso_helper_path", return_value=None))
                stack.enter_context(mock.patch.object(wm, "windows_installed", return_value=True))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(contextlib.redirect_stderr(err))
                rc = wm.main(["vm", "--dry-run", "--no-tpm", "--display", "none", *extra])
            self.assertEqual(rc, 1, extra)
            self.assertIn("regular file", err.getvalue(), extra)

        # A locachy-winiso answer naming a directory is refused like any other unusable answer.
        helper = TMP / "r28-winiso"
        helper.write_text(f'#!/bin/sh\necho "{iso_dir}"\nexit 0\n')
        helper.chmod(0o755)
        err = io.StringIO()
        with mock.patch.object(wm, "winiso_helper_path", return_value=helper), contextlib.redirect_stderr(err):
            self.assertIsNone(wm.winiso_path("windows"))
        self.assertIn("locachy-winiso path windows", err.getvalue())
        print("PASS create --root --disk D records the root disk and its root partition when D holds the target root "
              "(reclaim then grows it); --root never shrinks; ISO paths must be regular files")


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
