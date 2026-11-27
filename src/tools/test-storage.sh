#!/bin/sh
# End-to-end rootless tests for LOCachy multi-disk storage volumes
# (locachy_storage.py + locachy-storage + real mkfs.ext4 / mkfs.btrfs profiles).
# Runs on Linux without root privileges or host mutations.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: test-storage.sh requires Linux"
    exit 77
fi
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-storage-test.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi

fails=0
note() {
    echo "$1: $2"
    [ "$1" != FAIL ] || fails=$((fails + 1))
}

python3 -m py_compile "$src/link/locachy_storage.py" "$src/link/locachy-storage" "$src/link/locachy-compute"
note PASS "syntax: locachy_storage.py, locachy-storage, locachy-compute compile cleanly"

cat >"$out/driver.py" <<'PYEOF'
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

OUT = Path(sys.argv[1])
SRC = Path(sys.argv[2])
CLI = str(SRC / "link" / "locachy-storage")

spec = importlib.util.spec_from_file_location("locachy_storage", SRC / "link" / "locachy_storage.py")
st = importlib.util.module_from_spec(spec)
spec.loader.exec_module(st)

fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS" if ok else "FAIL") + ": " + name + ("" if ok or not detail else " -- " + detail), flush=True)
    if not ok:
        fails += 1


def expect_error(name, fn, substr=""):
    try:
        fn()
    except st.StorageError as exc:
        ok = (not substr) or (substr.lower() in str(exc).lower())
        check(name, ok, f"got: {exc}")
        return
    except Exception as exc:
        check(name, False, f"unexpected exception {type(exc).__name__}: {exc}")
        return
    check(name, False, "expected StorageError, succeeded instead")


# 1. Schema validation: invalid names, roles, topologies, RAID constraints
expect_error("schema: rejects uppercase/slash volume name",
             lambda: st.validate_volume_name("Bad/Name"))
expect_error("schema: rejects '..' volume name",
             lambda: st.validate_volume_name(".."))
expect_error("schema: rejects invalid role",
             lambda: st.validate_role("desktop"))
expect_error("schema: rejects btrfs-array with non-btrfs fs",
             lambda: st.validate_volume_entry({
                 "name": "arr", "topology": "btrfs-array", "fs": "ext4", "raid": "raid1",
                 "role": "shared", "encrypt": "none", "auto_mount": True, "fs_uuid": "1111-2222",
                 "members": [
                     {"part_uuid": "aaaa-1111", "luks_uuid": "", "mapper": "", "keyfile": ""},
                     {"part_uuid": "aaaa-2222", "luks_uuid": "", "mapper": "", "keyfile": ""},
                 ],
             }), "btrfs")
expect_error("schema: rejects btrfs-array raid10 with fewer than 4 members",
             lambda: st.validate_volume_entry({
                 "name": "arr10", "topology": "btrfs-array", "fs": "btrfs", "raid": "raid10",
                 "role": "compute", "encrypt": "none", "auto_mount": True, "fs_uuid": "1111-2222",
                 "members": [
                     {"part_uuid": "aaaa-1111", "luks_uuid": "", "mapper": "", "keyfile": ""},
                     {"part_uuid": "aaaa-2222", "luks_uuid": "", "mapper": "", "keyfile": ""},
                 ],
             }), "at least 4")
check("btrfs-profile: raid0/single still uses raid1 metadata",
      st.btrfs_metadata_profile("raid0", 2) == "raid1"
      and st.btrfs_metadata_profile("single", 2) == "raid1"
      and st.btrfs_metadata_profile("raid1", 3) == "raid1c3")

# 2. Multi-disk fixture: NVMe renumbering swap + duplicate UUID fail-closed + same-parent-disk RAID rejection
fixture_a = {
    "blockdevices": [
        {
            "path": "/dev/nvme0n1", "name": "/dev/nvme0n1", "kname": "nvme0n1", "type": "disk",
            "pkname": None, "partuuid": None, "uuid": None, "fstype": None, "label": None,
            "size": 512000000000, "model": "OS-NVMe", "serial": "S0", "tran": "nvme",
            "mountpoints": [None], "ro": False, "rm": False,
            "children": [
                {
                    "path": "/dev/nvme0n1p1", "type": "part", "pkname": "nvme0n1",
                    "partuuid": "00000000-0000-0000-0000-000000000001", "uuid": "EFI-1",
                    "fstype": "vfat", "label": "ESP", "size": 536870912, "mountpoints": ["/efi"],
                },
                {
                    "path": "/dev/nvme0n1p2", "type": "part", "pkname": "nvme0n1",
                    "partuuid": "00000000-0000-0000-0000-000000000002", "uuid": "ROOT-LUKS-1",
                    "fstype": "crypto_LUKS", "label": "locachy-crypt", "size": 500000000000,
                    "mountpoints": [None],
                    "children": [
                        {
                            "path": "/dev/mapper/locachy-root", "type": "crypt", "fstype": "ext4",
                            "uuid": "ROOT-FS-1", "mountpoints": ["/"],
                        }
                    ],
                },
            ],
        },
        {
            "path": "/dev/nvme1n1", "name": "/dev/nvme1n1", "kname": "nvme1n1", "type": "disk",
            "pkname": None, "partuuid": None, "uuid": None, "fstype": None, "label": None,
            "size": 2000000000000, "model": "Data-NVMe-A", "serial": "SA", "tran": "nvme",
            "mountpoints": [None], "ro": False, "rm": False,
            "children": [
                {
                    "path": "/dev/nvme1n1p1", "type": "part", "pkname": "nvme1n1",
                    "partuuid": "aaaaaaaa-1111-2222-3333-444444444444",
                    "uuid": "ffffffff-1111-2222-3333-444444444444",
                    "fstype": "crypto_LUKS", "label": "locachy-vol-pool-0", "size": 1000000000000,
                    "mountpoints": [None],
                },
                {
                    "path": "/dev/nvme1n1p2", "type": "part", "pkname": "nvme1n1",
                    "partuuid": "aaaaaaaa-1111-2222-3333-555555555555",
                    "uuid": "eeeeeeee-1111-2222-3333-555555555555",
                    "fstype": "ntfs", "label": "ArchiveNTFS", "size": 900000000000,
                    "mountpoints": [None],
                },
            ],
        },
        {
            "path": "/dev/nvme2n1", "name": "/dev/nvme2n1", "kname": "nvme2n1", "type": "disk",
            "pkname": None, "partuuid": None, "uuid": None, "fstype": None, "label": None,
            "size": 2000000000000, "model": "Data-NVMe-B", "serial": "SB", "tran": "nvme",
            "mountpoints": [None], "ro": False, "rm": False,
            "children": [
                {
                    "path": "/dev/nvme2n1p1", "type": "part", "pkname": "nvme2n1",
                    "partuuid": "bbbbbbbb-1111-2222-3333-444444444444",
                    "uuid": "ffffffff-2222-2222-3333-444444444444",
                    "fstype": "crypto_LUKS", "label": "locachy-vol-pool-1", "size": 2000000000000,
                    "mountpoints": [None],
                },
            ],
        },
        {
            "path": "/dev/nvme3n1", "name": "/dev/nvme3n1", "kname": "nvme3n1", "type": "disk",
            "pkname": None, "partuuid": None, "uuid": None, "fstype": None, "label": None,
            "size": 1000000000000, "model": "Blank-NVMe", "serial": "SC", "tran": "nvme",
            "mountpoints": [None], "ro": False, "rm": False,
        },
    ]
}

# Swap nvme1n1 <-> nvme2n1 (simulated multi-NVMe kernel probe reordering across boots)
fixture_swapped = json.loads(json.dumps(fixture_a))
d1, d2 = fixture_swapped["blockdevices"][1], fixture_swapped["blockdevices"][2]
d1["path"] = d1["name"] = "/dev/nvme2n1"
d1["kname"] = "nvme2n1"
d1["children"][0]["path"] = "/dev/nvme2n1p1"
d1["children"][0]["pkname"] = "nvme2n1"
d1["children"][1]["path"] = "/dev/nvme2n1p2"
d1["children"][1]["pkname"] = "nvme2n1"
d2["path"] = d2["name"] = "/dev/nvme1n1"
d2["kname"] = "nvme1n1"
d2["children"][0]["path"] = "/dev/nvme1n1p1"
d2["children"][0]["pkname"] = "nvme1n1"

sample_vol = st.validate_volume_entry({
    "name": "pool",
    "topology": "btrfs-array",
    "fs": "btrfs",
    "raid": "raid1",
    "role": "shared",
    "encrypt": "luks",
    "auto_mount": True,
    "fs_uuid": "99999999-1111-2222-3333-444444444444",
    "members": [
        {
            "part_uuid": "aaaaaaaa-1111-2222-3333-444444444444",
            "luks_uuid": "ffffffff-1111-2222-3333-444444444444",
            "mapper": "locachy-vol-pool-0",
            "keyfile": "/etc/locachy/keys/vol-pool-0.key",
        },
        {
            "part_uuid": "bbbbbbbb-1111-2222-3333-444444444444",
            "luks_uuid": "ffffffff-2222-2222-3333-444444444444",
            "mapper": "locachy-vol-pool-1",
            "keyfile": "/etc/locachy/keys/vol-pool-1.key",
        },
    ],
})

res_before = st.resolve_volume_members(sample_vol, blockdevices=fixture_a["blockdevices"])
res_after = st.resolve_volume_members(sample_vol, blockdevices=fixture_swapped["blockdevices"])
check("nvme-swap: UUID/PARTUUID resolution tracks members across nvme1n1 <-> nvme2n1 swap",
      res_before[0]["device_path"] == "/dev/nvme1n1p1"
      and res_before[1]["device_path"] == "/dev/nvme2n1p1"
      and res_after[0]["device_path"] == "/dev/nvme2n1p1"
      and res_after[1]["device_path"] == "/dev/nvme1n1p1",
      f"before={res_before} after={res_after}")

# Duplicate UUID fail-closed check (e.g. raw dd clone attached)
fixture_dup = json.loads(json.dumps(fixture_a))
fixture_dup["blockdevices"].append({
    "path": "/dev/nvme4n1", "name": "/dev/nvme4n1", "kname": "nvme4n1", "type": "disk",
    "size": 2000000000000, "model": "Clone", "tran": "nvme", "ro": False, "rm": False,
    "children": [
        {
            "path": "/dev/nvme4n1p1", "type": "part", "pkname": "nvme4n1",
            "partuuid": "aaaaaaaa-1111-2222-3333-444444444444",
            "uuid": "ffffffff-1111-2222-3333-444444444444",
            "fstype": "crypto_LUKS", "size": 1000000000000, "mountpoints": [None],
        },
    ],
})
expect_error("nvme-dup: duplicate member UUID fails closed",
             lambda: st.resolve_volume_members(sample_vol, blockdevices=fixture_dup["blockdevices"]),
             "Ambiguous member UUID")

# Same physical parent disk rejection for btrfs-array (e.g. two partitions on nvme1n1)
expect_error("safety: rejects formatting system/root disk",
             lambda: st._ensure_safe_target_disks(
                 ["/dev/nvme0n1"], mode="format", cfg={"schema": 1, "volumes": []},
                 blockdevices=fixture_a["blockdevices"]
             ), "protected")
expect_error("safety: rejects two members from same physical parent disk in btrfs-array",
             lambda: st._ensure_safe_target_disks(
                 ["/dev/nvme1n1p1", "/dev/nvme1n1p2"], mode="array", cfg={"schema": 1, "volumes": []},
                 blockdevices=fixture_a["blockdevices"]
             ))
# Verify same-parent-disk check specifically when two partitions share _top_disk even if node type check is bypassed:
same_disk_fixture = json.loads(json.dumps(fixture_a["blockdevices"]))
same_disk_fixture[1]["children"][0]["type"] = "disk"
same_disk_fixture[1]["children"][1]["type"] = "disk"
same_disk_fixture[1]["children"][0]["_top_disk"] = "/dev/nvme1n1"
expect_error("safety: btrfs-array rejects duplicate disk entries",
             lambda: st._ensure_safe_target_disks(
                 ["/dev/nvme3n1", "/dev/nvme3n1"], mode="array", cfg={"schema": 1, "volumes": []},
                 blockdevices=fixture_a["blockdevices"]
             ), "duplicate device")
# Also verify with two child partitions forced to type=disk sharing the same top disk
two_parts_same_parent = [
    {
        "path": "/dev/nvme9n1", "type": "disk", "size": 1000, "ro": False, "rm": False,
        "children": [
            {"path": "/dev/nvme9n1p1", "type": "disk", "size": 500, "ro": False},
            {"path": "/dev/nvme9n1p2", "type": "disk", "size": 500, "ro": False},
        ],
    }
]
# Override flatten behavior by passing nodes directly through a tree where top_disk matches parent
flat_same = [
    {"path": "/dev/nvme9n1", "type": "disk", "size": 1000, "ro": False, "rm": False, "children": [
        {"path": "/dev/nvme9n1p1", "type": "part", "size": 500, "ro": False},
        {"path": "/dev/nvme9n1p2", "type": "part", "size": 500, "ro": False},
    ]}
]
# Check that flatten_blockdevices assigns the same _top_disk to both partitions of nvme9n1
fl = st.flatten_blockdevices(flat_same)
p_nodes = [n for n in fl if n["type"] == "part"]
check("safety: flatten_blockdevices maps sibling partitions to identical _top_disk",
      len(p_nodes) == 2 and p_nodes[0]["_top_disk"] == p_nodes[1]["_top_disk"] == "/dev/nvme9n1")

# Verify root_is_encrypted detects LUKS-backed root
check("root-encrypt: detects LUKS2-backed root filesystem", st.root_is_encrypted(fixture_a["blockdevices"]))

# 3. parse_install_spec tests
s_fmt = st.parse_install_spec("name=data,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,encrypt=auto")
check("install-spec: parses format spec",
      s_fmt["name"] == "data" and s_fmt["mode"] == "format" and s_fmt["devs"] == ["/dev/nvme1n1"]
      and s_fmt["fs"] == "ext4" and s_fmt["role"] == "shared" and s_fmt["encrypt"] == "auto")

s_att = st.parse_install_spec("name=archive,mode=attach,dev=/dev/nvme1n1p1,role=shared")
check("install-spec: parses attach spec",
      s_att["name"] == "archive" and s_att["mode"] == "attach" and s_att["devs"] == ["/dev/nvme1n1p1"]
      and s_att["role"] == "shared" and s_att["encrypt"] == "none")

s_arr = st.parse_install_spec("name=pool,mode=array,devs=/dev/nvme1n1+/dev/nvme2n1,raid=raid1,role=compute,encrypt=auto")
check("install-spec: parses btrfs array spec",
      s_arr["name"] == "pool" and s_arr["mode"] == "array"
      and s_arr["devs"] == ["/dev/nvme1n1", "/dev/nvme2n1"]
      and s_arr["fs"] == "btrfs" and s_arr["raid"] == "raid1" and s_arr["role"] == "compute")

expect_error("install-spec: rejects unknown key",
             lambda: st.parse_install_spec("name=x,mode=format,dev=/dev/nvme1n1,bogus=1"), "unknown key")
expect_error("install-spec: rejects raid10 with only 2 devices",
             lambda: st.parse_install_spec("name=x,mode=array,devs=/dev/nvme1n1+/dev/nvme2n1,raid=raid10"), "at least 4")

# 4. --root target creation + keyfile permissions (0700 dir / 0600 keyfile) + CLI status/list-disks/set-role/set-compute-storage/remove
fixture_file = OUT / "lsblk.json"
fixture_file.write_text(json.dumps(fixture_a), encoding="utf-8")

target_root = OUT / "target-root"
target_root.mkdir(parents=True)

cli_env = dict(os.environ, LOCACHY_STORAGE_LSBLK_JSON=str(fixture_file))

def run_cli(*args):
    return subprocess.run(["python3", CLI, *args], env=cli_env, capture_output=True, text=True, timeout=30)

r_ld = run_cli("list-disks", "--json", "--root", str(target_root))
ld_data = json.loads(r_ld.stdout) if r_ld.returncode == 0 else []
disk_paths = [d["path"] for d in ld_data]
check("cli: list-disks excludes system/root disk and lists secondary NVMe disks",
      r_ld.returncode == 0 and "/dev/nvme0n1" not in disk_paths
      and "/dev/nvme1n1" in disk_paths and "/dev/nvme3n1" in disk_paths,
      r_ld.stdout + r_ld.stderr)
check("cli: list-disks reports serial and free_tail_bytes for every disk",
      bool(ld_data) and all("serial" in d and d["serial"] and "free_tail_bytes" in d for d in ld_data),
      json.dumps(ld_data)[:300])

# Attach existing NTFS partition into --root target without formatting
r_att = run_cli("create", "--name", "archive", "--mode", "attach", "--role", "data",
                "--dev", "/dev/nvme1n1p2", "--root", str(target_root), "--no-mount")
vjson_path = target_root / "etc/locachy/volumes.json"
vjson_mode = stat.S_IMODE(vjson_path.stat().st_mode) if vjson_path.is_file() else 0
check("cli: create --mode attach --root writes volumes.json (0644) with PARTUUID & fs_uuid",
      r_att.returncode == 0 and vjson_path.is_file() and vjson_mode == 0o644,
      r_att.stdout + r_att.stderr)

loaded = json.loads(vjson_path.read_text(encoding="utf-8"))
check("cli: attached volume records exact PARTUUID, fs_uuid, ntfs, zero device paths",
      loaded["schema"] == 1
      and len(loaded["volumes"]) == 1
      and loaded["volumes"][0]["fs"] == "ntfs"
      and loaded["volumes"][0]["members"][0]["part_uuid"] == "aaaaaaaa-1111-2222-3333-555555555555"
      and "/dev/" not in json.dumps(loaded),
      json.dumps(loaded))

# Verify keyfile creation enforces 0700 keys dir and 0600 keyfile
kdir = target_root / "etc/locachy/keys"
kf_path = kdir / "vol-pool-0.key"
st._write_keyfile(kf_path, b"K" * 64)
check("keys: key directory is 0700 and keyfile is 0600",
      stat.S_IMODE(kdir.stat().st_mode) == 0o700
      and stat.S_IMODE(kf_path.stat().st_mode) == 0o600
      and len(kf_path.read_bytes()) == 64)

# Register an encrypted volume entry referencing that keyfile to test remove preservation
cfg = st.load_config(root=target_root)
cfg["volumes"].append({
    "name": "pool",
    "topology": "btrfs-array",
    "fs": "btrfs",
    "raid": "raid1",
    "role": "shared",
    "encrypt": "luks",
    "auto_mount": True,
    "fs_uuid": "99999999-1111-2222-3333-444444444444",
    "members": [
        {
            "part_uuid": "aaaaaaaa-1111-2222-3333-444444444444",
            "luks_uuid": "ffffffff-1111-2222-3333-444444444444",
            "mapper": "locachy-vol-pool-0",
            "keyfile": "/etc/locachy/keys/vol-pool-0.key",
        },
        {
            "part_uuid": "bbbbbbbb-1111-2222-3333-444444444444",
            "luks_uuid": "ffffffff-2222-2222-3333-444444444444",
            "mapper": "locachy-vol-pool-1",
            "keyfile": "/etc/locachy/keys/vol-pool-1.key",
        },
    ],
    "mount_options": "",
    "created_at": "2026-11-24T00:00:00Z",
})
st.save_config(cfg, root=target_root)

# set-compute-storage role policy: refuse data, accept shared/compute
r_cs_bad = run_cli("set-compute-storage", "archive", "--root", str(target_root))
check("policy: set-compute-storage refuses role='data' volume",
      r_cs_bad.returncode != 0 and "data" in r_cs_bad.stderr, r_cs_bad.stdout + r_cs_bad.stderr)

r_cs_ok = run_cli("set-compute-storage", "pool", "--root", str(target_root))
rc_text = (target_root / "etc/locachy/rc.conf").read_text(encoding="utf-8") if (target_root / "etc/locachy/rc.conf").is_file() else ""
check("policy: set-compute-storage accepts role='shared' and updates rc.conf",
      r_cs_ok.returncode == 0 and 'compute_storage="pool"' in rc_text, r_cs_ok.stdout + r_cs_ok.stderr)

# Cannot downgrade active compute_storage volume to role='data'
r_down = run_cli("set-role", "pool", "data", "--root", str(target_root))
check("policy: set-role refuses downgrading active compute_storage volume to 'data'",
      r_down.returncode != 0 and "compute_storage" in r_down.stderr, r_down.stdout + r_down.stderr)

# set-role toggling data -> shared -> compute on archive
r_sr1 = run_cli("set-role", "archive", "shared", "--root", str(target_root))
r_sr2 = run_cli("set-role", "archive", "compute", "--root", str(target_root))
r_sa = run_cli("set-auto", "archive", "off", "--root", str(target_root))
r_st = run_cli("status", "--json", "--root", str(target_root))
st_data = json.loads(r_st.stdout) if r_st.returncode == 0 else {}
vol_map = {v["name"]: v for v in st_data.get("volumes", [])}
check("cli: set-role, set-auto, and status JSON report expected fields",
      r_sr1.returncode == 0 and r_sr2.returncode == 0 and r_sa.returncode == 0 and r_st.returncode == 0
      and vol_map.get("archive", {}).get("role") == "compute"
      and vol_map.get("archive", {}).get("auto_mount") is False
      and vol_map.get("pool", {}).get("keyfile_on_encrypted_root") is True
      and st_data.get("compute_storage") == "pool",
      r_st.stdout + r_st.stderr)

# remove default keeps LUKS keyfile intact (and clears compute_storage)
r_rm = run_cli("remove", "pool", "--root", str(target_root))
rc_after = (target_root / "etc/locachy/rc.conf").read_text(encoding="utf-8")
check("remove: default detach preserves LUKS keyfile and clears compute_storage",
      r_rm.returncode == 0 and kf_path.is_file() and "compute_storage=" not in rc_after,
      r_rm.stdout + r_rm.stderr)

# 5. Real mkfs.ext4 and mkfs.btrfs -d raid1 -m raid1 on sparse image files
if shutil.which("mkfs.ext4"):
    ext4_img = OUT / "disk-ext4.img"
    with open(ext4_img, "wb") as fh:
        fh.truncate(32 * 1024 * 1024)
    p_ext4 = subprocess.run(["mkfs.ext4", "-F", "-L", "data", str(ext4_img)], capture_output=True, text=True)
    p_blkid = subprocess.run(["blkid", "-o", "value", "-s", "TYPE", str(ext4_img)], capture_output=True, text=True)
    check("mkfs: real mkfs.ext4 formats sparse image cleanly",
          p_ext4.returncode == 0 and p_blkid.stdout.strip() == "ext4",
          p_ext4.stderr + p_blkid.stdout)
else:
    print("SKIP: mkfs.ext4 not installed", flush=True)

if shutil.which("mkfs.btrfs"):
    b1 = OUT / "btrfs-a.img"
    b2 = OUT / "btrfs-b.img"
    for p in (b1, b2):
        with open(p, "wb") as fh:
            fh.truncate(256 * 1024 * 1024)
    mprof = st.btrfs_metadata_profile("raid1", 2)
    p_btrfs = subprocess.run(
        ["mkfs.btrfs", "-f", "-L", "pool", "-d", "raid1", "-m", mprof, str(b1), str(b2)],
        capture_output=True, text=True,
    )
    out_txt = p_btrfs.stdout + p_btrfs.stderr
    # Verify both disks share the same Btrfs fsid and metadata/data profiles are RAID1
    u1 = subprocess.run(["blkid", "-o", "value", "-s", "UUID", str(b1)], capture_output=True, text=True).stdout.strip()
    u2 = subprocess.run(["blkid", "-o", "value", "-s", "UUID", str(b2)], capture_output=True, text=True).stdout.strip()
    check("mkfs: real mkfs.btrfs -d raid1 -m raid1 creates 2-disk RAID1 array with shared UUID",
          p_btrfs.returncode == 0
          and bool(u1) and u1 == u2
          and "Data:" in out_txt and "RAID1" in out_txt
          and "Metadata:" in out_txt,
          f"rc={p_btrfs.returncode} u1={u1} u2={u2} out={out_txt[:300]!r}")
else:
    print("SKIP: mkfs.btrfs not installed", flush=True)

# 5.5 Reserved tail layout (format_plan + sfdisk GPT on a sparse image + free_tail_bytes)
plan_full = st.format_plan(200 * 1024**3, 512, 0)
plan_res = st.format_plan(200 * 1024**3, 512, 50 * 1024)
check("reserve-tail: format_plan aligns end to 1 MiB and leaves requested tail >= 50 GiB",
      plan_full["full_disk"] is True
      and plan_res["full_disk"] is False
      and (plan_res["end_lba"] + 1) % 2048 == 0
      and plan_res["reserved_tail_bytes"] >= 50 * 1024**3
      and plan_res["size_bytes"] >= 1024**3,
      str(plan_res))

plan_4k = st.format_plan(200 * 1024**3, 4096, 50 * 1024)
check("reserve-tail: 4096-byte logical sectors align to 1 MiB and reserve >= 50 GiB",
      (plan_4k["end_lba"] + 1) % 256 == 0
      and plan_4k["reserved_tail_bytes"] >= 50 * 1024**3
      and plan_4k["log_sec"] == 4096,
      str(plan_4k))

expect_error("reserve-tail: rejects negative --reserve-tail-mib",
             lambda: st.format_plan(200 * 1024**3, 512, -1), "0 or more")
expect_error("reserve-tail: rejects reservation leaving < 1 GiB for volume",
             lambda: st.format_plan(2 * 1024**3, 512, 1500), "less than 1 GiB")
expect_error("reserve-tail: install spec refuses reserve_tail_mib on attach",
             lambda: st.parse_install_spec("name=a,mode=attach,dev=/dev/nvme1n1p1,reserve_tail_mib=1024"), "mode=format")
s_rt = st.parse_install_spec("name=d,mode=format,dev=/dev/nvme1n1,fs=ext4,role=shared,reserve_tail_mib=51200")
check("reserve-tail: parse_install_spec reads reserve_tail_mib",
      s_rt["reserve_tail_mib"] == 51200)

# CLI refuses --reserve-tail-mib on --mode attach before touching any disk
r_rt_bad = run_cli("create", "--name", "badrt", "--mode", "attach", "--role", "shared",
                   "--dev", "/dev/nvme1n1p2", "--reserve-tail-mib", "1024", "--root", str(target_root), "--no-mount")
check("reserve-tail: CLI rejects --reserve-tail-mib with --mode attach",
      r_rt_bad.returncode != 0 and "mode format" in r_rt_bad.stderr, r_rt_bad.stdout + r_rt_bad.stderr)

# Real GPT partitioning with sfdisk on a sparse 2 GiB image leaving a 512 MiB tail
if shutil.which("sfdisk"):
    tail_img = OUT / "tail-disk.img"
    tail_size = 2 * 1024**3
    with open(tail_img, "wb") as fh:
        fh.truncate(tail_size)
    tplan = st.format_plan(tail_size, 512, 512)
    script = (
        f"label: gpt\nstart={tplan['start_lba']}, size={tplan['size_sectors']}, "
        f"type={st.GPT_TYPE_LINUX_DATA}, name=\"locachy-data\"\n"
    )
    p_sf = subprocess.run(["sfdisk", "--no-reread", str(tail_img)], input=script, capture_output=True, text=True)
    p_dump = subprocess.run(["sfdisk", "-J", str(tail_img)], capture_output=True, text=True)
    pt = json.loads(p_dump.stdout)["partitiontable"] if p_dump.returncode == 0 else {}
    parts = pt.get("partitions") or []
    p1 = parts[0] if parts else {}
    free_after = st._free_tail_bytes({
        "size": tail_size,
        "log-sec": 512,
        "children": [{"type": "part", "start": p1.get("start"), "size": int(p1.get("size", 0)) * 512}],
    })
    check("reserve-tail: real sfdisk GPT leaves >= 512 MiB unallocated tail after p1",
          p_sf.returncode == 0
          and len(parts) == 1
          and p1.get("start") == tplan["start_lba"]
          and p1.get("size") == tplan["size_sectors"]
          and free_after is not None and free_after >= 512 * 1024**2,
          f"rc={p_sf.returncode} p1={p1} free_after={free_after} err={p_sf.stderr}")
else:
    print("SKIP: sfdisk not installed", flush=True)

# _free_tail_bytes returns None when a partition lacks START, and exact unallocated bytes on a blank disk
check("free-tail: blank disk reports usable bytes; missing START reports None",
      (st._free_tail_bytes({"size": 10 * 1024**3, "log-sec": 512, "children": []}) or 0) > 9 * 1024**3
      and st._free_tail_bytes({"size": 10 * 1024**3, "log-sec": 512, "children": [{"type": "part", "size": 1024**3}]}) is None)

# 5.6 Maintenance Windows partitions (locachy-winmaint): never formatted, never attached.
def wm_part(path, uid, label, fstype, size, start, uuid=None):
    return {"path": path, "type": "part", "partuuid": uid, "partlabel": label, "uuid": uuid, "fstype": fstype,
            "label": None, "size": size, "start": start, "mountpoints": [None]}

def wm_disk(path, size, children):
    return {"path": path, "name": path, "kname": path[5:], "type": "disk", "size": size, "model": "WM",
            "serial": path[5:], "tran": "nvme", "log-sec": 512, "mountpoints": [None], "ro": False, "rm": False,
            "children": children}

def triplet(disk, base, start):
    return [wm_part(f"{disk}p{base}", f"{disk[-3:]}00000-0000-0000-0000-00000000000{base}", "LOCACHY_WIN_ESP",
                    "vfat", 128 * 1024**2, start, "AAAA-000" + str(base)),
            wm_part(f"{disk}p{base + 1}", f"{disk[-3:]}00000-0000-0000-0000-00000000000{base + 1}", "LOCACHY_WIN_MSR",
                    None, 16 * 1024**2, start + 262144),
            wm_part(f"{disk}p{base + 2}", f"{disk[-3:]}00000-0000-0000-0000-00000000000{base + 2}", "LOCACHY_WIN_C",
                    "ntfs", 48 * 1024**3, start + 294912, "0123456789ABCDE" + str(base))]

wm_tree = json.loads(json.dumps(fixture_a["blockdevices"])) + [
    # A secondary disk holding only the Maintenance Windows partitions.
    wm_disk("/dev/nvme5n1", 256 * 1024**3, triplet("/dev/nvme5n1", 1, 2048)),
    # A shared disk: a LOCachy volume, the triplet in its reserved tail.
    wm_disk("/dev/nvme6n1", 512 * 1024**3,
            [wm_part("/dev/nvme6n1p1", "61616161-0000-0000-0000-000000000001", "locachy-data", "ext4",
                     400 * 1024**3, 2048, "61616161-aaaa-bbbb-cccc-000000000001")]
            + triplet("/dev/nvme6n1", 2, 838862848)),
    # Windows partitions without their GPT names: known by the PARTUUIDs in state.json.
    wm_disk("/dev/nvme7n1", 256 * 1024**3,
            [wm_part("/dev/nvme7n1p1", "71717171-0000-0000-0000-000000000003", "", "ntfs", 48 * 1024**3, 2048,
                     "7171717171717171")]),
]
wm_root = OUT / "wm-root"
wm_state = wm_root / "var/lib/locachy/winmaint/state.json"
wm_state.parent.mkdir(parents=True)
wm_state.write_text(json.dumps({"schema": 1, "disk": "/dev/nvme7n1", "partitions": {
    "c": {"part_guid": "71717171-0000-0000-0000-000000000003", "sectors": 48 * 2097152}}}), encoding="utf-8")
cfg = st.load_config(root=wm_root)
cfg["volumes"].append(st.validate_volume_entry({
    "name": "data", "topology": "single", "fs": "ext4", "raid": "", "role": "shared", "encrypt": "none",
    "auto_mount": True, "fs_uuid": "61616161-aaaa-bbbb-cccc-000000000001",
    "members": [{"part_uuid": "61616161-0000-0000-0000-000000000001", "luks_uuid": "", "mapper": "", "keyfile": ""}],
    "mount_options": "", "created_at": "2026-11-24T00:00:00Z"}))
st.save_config(cfg, root=wm_root)

def wm_disks(root=wm_root):
    return {d["path"]: d for d in st.list_disks(root=root, blockdevices=wm_tree)}

ld = wm_disks()
d5, d6, d7 = ld.get("/dev/nvme5n1", {}), ld.get("/dev/nvme6n1", {}), ld.get("/dev/nvme7n1", {})
check("winmaint: a disk with only Windows partitions is listed, never available for format, and says why",
      d5 and not d5["available_for_format"] and d5["maintenance_windows"] and not d5["system"]
      and "Maintenance Windows" in d5["in_use_reason"], json.dumps(d5)[:400])
check("winmaint: its Windows ESP, MSR and C: (vfat, ntfs) are never attachable",
      d5 and all(p["maintenance_windows"] and not p["attachable"] for p in d5["partitions"]), json.dumps(d5)[:400])
check("winmaint: the shared disk shows its volume and Maintenance Windows",
      d6 and not d6["available_for_format"] and "volume:data" in d6["in_use_reason"]
      and "Maintenance Windows" in d6["in_use_reason"], json.dumps(d6)[:400])
check("winmaint: a partition recorded in state.json counts without its GPT name; without the record it does not",
      d7 and d7["maintenance_windows"] and not d7["partitions"][0]["attachable"]
      and wm_disks(OUT / "no-state-root")["/dev/nvme7n1"]["available_for_format"], json.dumps(d7)[:400])

def no_disk_writes(*a, **k):
    raise AssertionError("reached partitioning")

real_partition, st._partition_single_gpt = st._partition_single_gpt, no_disk_writes
try:
    st.remove_volume("data", root=wm_root)
    ld = wm_disks()
    d6 = ld.get("/dev/nvme6n1", {})
    p6 = {p["path"]: p for p in d6.get("partitions", [])}
    check("winmaint: after remove data the shared disk is still never available for format",
          d6 and not d6["available_for_format"] and d6["in_use_reason"] == "Maintenance Windows", json.dumps(d6)[:400])
    check("winmaint: after remove data its data partition can be attached again, its Windows partitions cannot",
          p6.get("/dev/nvme6n1p1", {}).get("attachable") is True
          and not any(p6[x]["attachable"] for x in ("/dev/nvme6n1p2", "/dev/nvme6n1p3", "/dev/nvme6n1p4")),
          json.dumps(p6)[:400])
    for name, disk in (("only-windows", "/dev/nvme5n1"), ("shared", "/dev/nvme6n1")):
        expect_error(f"winmaint: create --mode format refuses the {name} disk before any write",
                     lambda disk=disk: st.create_volume(name="x", mode="format", role="shared", devs=[disk],
                                                        encrypt="none", root=str(wm_root), no_mount=True,
                                                        blockdevices=wm_tree), "Maintenance Windows")
    expect_error("winmaint: an array member disk holding Windows partitions is refused",
                 lambda: st.create_volume(name="arr", mode="array", role="compute", devs=["/dev/nvme3n1", "/dev/nvme5n1"],
                                          encrypt="none", root=str(wm_root), no_mount=True, blockdevices=wm_tree),
                 "Maintenance Windows")
    for part in ("/dev/nvme5n1p3", "/dev/nvme5n1p1", "/dev/nvme7n1p1"):
        expect_error(f"winmaint: attach refuses {part}",
                     lambda part=part: st.create_volume(name="win", mode="attach", role="data", devs=[part],
                                                        root=str(wm_root), no_mount=True, blockdevices=wm_tree),
                     "Maintenance Windows")
    nodes, _ = st._ensure_safe_target_disks(["/dev/nvme6n1p1"], mode="attach", cfg={"schema": 1, "volumes": []},
                                            blockdevices=wm_tree)
    check("winmaint: another partition on a Maintenance Windows disk can still be attached",
          [n["path"] for n in nodes] == ["/dev/nvme6n1p1"])
    # A volume that resolves to a Windows partition is never wiped (checked before anything changes).
    live_vfile = OUT / "wm-live/volumes.json"
    os.environ["LOCACHY_VOLUMES_FILE"] = str(live_vfile)
    st.save_config({"schema": 1, "volumes": [st.validate_volume_entry({
        "name": "winc", "topology": "single", "fs": "ntfs", "raid": "", "role": "data", "encrypt": "none",
        "auto_mount": False, "fs_uuid": "0123456789ABCDE1",
        "members": [{"part_uuid": "5n100000-0000-0000-0000-000000000003", "luks_uuid": "", "mapper": "",
                     "keyfile": ""}],
        "mount_options": "", "created_at": "2026-11-24T00:00:00Z"})]})
    expect_error("winmaint: remove --wipe refuses a member that is a Maintenance Windows partition",
                 lambda: st.remove_volume("winc", wipe=True, blockdevices=wm_tree), "Maintenance Windows")
    check("winmaint: the refused wipe left the volume registered",
          [v["name"] for v in st.load_config()["volumes"]] == ["winc"])
    del os.environ["LOCACHY_VOLUMES_FILE"]
finally:
    st._partition_single_gpt = real_partition

wm_file = OUT / "lsblk-winmaint.json"
wm_file.write_text(json.dumps({"blockdevices": wm_tree}), encoding="utf-8")
wm_env = dict(os.environ, LOCACHY_STORAGE_LSBLK_JSON=str(wm_file))
r_wm = subprocess.run(["python3", CLI, "list-disks", "--root", str(wm_root)], env=wm_env, capture_output=True,
                      text=True, timeout=30)
wm_lines = r_wm.stdout.splitlines()
check("winmaint: list-disks text says in-use (Maintenance Windows) and marks the Windows partitions",
      r_wm.returncode == 0
      and any(l.startswith("/dev/nvme5n1\t") and l.endswith("in-use (Maintenance Windows)") for l in wm_lines)
      and any(l.strip().startswith("/dev/nvme5n1p3") and "Maintenance Windows (never attached)" in l for l in wm_lines),
      r_wm.stdout + r_wm.stderr)
r_wm_st = subprocess.run(["python3", CLI, "status", "--json", "--root", str(wm_root)], env=wm_env,
                         capture_output=True, text=True, timeout=30)
wm_st = {d["path"]: d for d in json.loads(r_wm_st.stdout)["available_disks"]} if r_wm_st.returncode == 0 else {}
check("winmaint: status --json lists the disk with its reason",
      wm_st.get("/dev/nvme5n1", {}).get("in_use_reason") == "Maintenance Windows", r_wm_st.stdout[:300] + r_wm_st.stderr)
r_wm_att = subprocess.run(["python3", CLI, "create", "--name", "winc", "--mode", "attach", "--role", "data",
                           "--dev", "/dev/nvme5n1p3", "--root", str(wm_root), "--no-mount"], env=wm_env,
                          capture_output=True, text=True, timeout=30)
check("winmaint: CLI create --mode attach refuses Windows C: with the reason",
      r_wm_att.returncode != 0 and "Maintenance Windows" in r_wm_att.stderr
      and "winc" not in (wm_root / "etc/locachy/volumes.json").read_text(encoding="utf-8"),
      r_wm_att.stdout + r_wm_att.stderr)

# 5.7 Pre-udev boot auto-mount: null lsblk metadata enriched by blkid/sfdisk,
#     sysfs dm lookup for missing /dev/mapper/<name>, and bounded missing-member retry.
preudev_tree = [
    {
        "path": "/dev/nvme1n1", "name": "/dev/nvme1n1", "kname": "nvme1n1", "type": "disk",
        "pkname": None, "partuuid": None, "partlabel": None, "uuid": None, "fstype": None, "label": None,
        "size": 2000000000000, "model": "Data-NVMe", "serial": "SDATA", "tran": "nvme",
        "mountpoints": [None], "ro": False, "rm": False,
        "children": [
            {
                "path": "/dev/nvme1n1p1", "name": "/dev/nvme1n1p1", "kname": "nvme1n1p1", "type": "part",
                "pkname": "nvme1n1", "partuuid": None, "partlabel": None, "uuid": None, "fstype": None,
                "label": None, "size": 2000000000000, "mountpoints": [None],
            }
        ],
    }
]
orig_blkid = st._probe_blkid_export
orig_sfdisk = st._probe_sfdisk_parts
try:
    st._probe_blkid_export = lambda p: (
        {"UUID": "4f55d91e-27a0-4cd7-9a30-7d4582c9d26d", "TYPE": "crypto_LUKS"}
        if p == "/dev/nvme1n1p1" else {}
    )
    st._probe_sfdisk_parts = lambda d: (
        {"/dev/nvme1n1p1": {"partuuid": "65c9f417-e3a3-401e-9365-f1cd7026059d", "partlabel": "locachy-data"}}
        if d == "/dev/nvme1n1" else {}
    )
    enriched = st._enrich_blockdevices_blkid(preudev_tree)
    pnode = enriched[0]["children"][0]
    vol_omen = st.validate_volume_entry({
        "name": "data", "topology": "single", "fs": "btrfs", "raid": "", "role": "shared",
        "encrypt": "luks", "auto_mount": True, "fs_uuid": "5f83c5d4-5408-4882-840a-be89699d8f53",
        "members": [{
            "part_uuid": "65c9f417-e3a3-401e-9365-f1cd7026059d",
            "luks_uuid": "4f55d91e-27a0-4cd7-9a30-7d4582c9d26d",
            "mapper": "locachy-vol-data-0",
            "keyfile": "/etc/locachy/keys/vol-data-0.key",
        }],
        "mount_options": "", "created_at": "2026-11-26T13:42:58Z",
    })
    res_pre = st.resolve_volume_members(vol_omen, blockdevices=enriched)
    check("pre-udev: blkid+sfdisk enrichment fills null lsblk fields and resolves LUKS member",
          pnode["uuid"] == "4f55d91e-27a0-4cd7-9a30-7d4582c9d26d"
          and pnode["partuuid"] == "65c9f417-e3a3-401e-9365-f1cd7026059d"
          and pnode["fstype"] == "crypto_LUKS"
          and res_pre[0]["present"] is True
          and res_pre[0]["device_path"] == "/dev/nvme1n1p1",
          f"pnode={pnode} res={res_pre}")
finally:
    st._probe_blkid_export = orig_blkid
    st._probe_sfdisk_parts = orig_sfdisk

# Sysfs dm/name discovery for pre-udev mapper nodes
fake_sys_block = OUT / "fake-sys-block"
(fake_sys_block / "dm-2" / "dm").mkdir(parents=True)
(fake_sys_block / "dm-2" / "dm" / "name").write_text("locachy-vol-data-0\n", encoding="utf-8")
(fake_sys_block / "dm-2" / "dev").write_text("253:2\n", encoding="utf-8")
orig_sys_block = st.SYS_BLOCK_DIR
try:
    st.SYS_BLOCK_DIR = str(fake_sys_block)
    dm_hit = st._find_dm_for_mapper("locachy-vol-data-0")
    dm_miss = st._find_dm_for_mapper("locachy-vol-other-0")
    check("pre-udev: _find_dm_for_mapper locates active dm-2 device from sysfs",
          dm_hit == ("dm-2", 253, 2) and dm_miss is None, f"hit={dm_hit} miss={dm_miss}")
finally:
    st.SYS_BLOCK_DIR = orig_sys_block

# mount_all bounded retry when a member disk finishes probing on attempt 2
retry_vfile = OUT / "retry-volumes.json"
os.environ["LOCACHY_VOLUMES_FILE"] = str(retry_vfile)
os.environ["LOCACHY_STORAGE_MOUNT_RETRY_S"] = "0.01"
st.save_config({"schema": 1, "volumes": [vol_omen]})
attempts = {"data": 0}
orig_mount_vol = st.mount_volume
def fake_mount_vol(name, **kw):
    attempts[name] = attempts.get(name, 0) + 1
    if attempts[name] == 1:
        raise st.StorageError("volume 'data': missing 1/1 member(s): 4f55d91e")
    return {"name": name, "mountpoint": "/srv/storage/data", "drives_path": "", "already_mounted": False}
try:
    st.mount_volume = fake_mount_vol
    mres = st.mount_all()
    check("pre-udev: mount_all retries transient missing-member error and succeeds on attempt 2",
          attempts["data"] == 2 and mres["mounted"] == ["data"] and not mres["errors"],
          f"attempts={attempts} res={mres}")
finally:
    st.mount_volume = orig_mount_vol
    del os.environ["LOCACHY_VOLUMES_FILE"]
    del os.environ["LOCACHY_STORAGE_MOUNT_RETRY_S"]

sys.exit(0 if fails == 0 else 1)
PYEOF

python3 "$out/driver.py" "$out" "$src" >"$out/unit.log" 2>&1 || :
grep -E '^(PASS|FAIL|SKIP): ' "$out/unit.log" || cat "$out/unit.log"
sub_fails=$(grep -c '^FAIL: ' "$out/unit.log" || :)
fails=$((fails + sub_fails))

# 6. Live ~/Drives bind projection toggle via set-role inside unprivileged user/mount namespace
if unshare --user --map-auto --map-root-user --mount --propagation private true >/dev/null 2>&1; then
    cat >"$out/ns_driver.py" <<'PYEOF'
import json
import os
from pathlib import Path
import subprocess
import sys

OUT = Path(sys.argv[1])
SRC = Path(sys.argv[2])
CLI = str(SRC / "link" / "locachy-storage")

sroot = OUT / "ns-srv"
home = OUT / "ns-home"
drives = home / "Drives"
vfile = OUT / "ns-volumes.json"
rcfile = OUT / "ns-rc.conf"
fixture = OUT / "lsblk.json"
sroot.mkdir(parents=True, exist_ok=True)
home.mkdir(parents=True, exist_ok=True)

mp = sroot / "scratch"
mp.mkdir(parents=True, exist_ok=True)
subprocess.run(["mount", "-t", "tmpfs", "tmpfs", str(mp)], check=True)
(mp / "hello.txt").write_text("projected-ok\n", encoding="utf-8")

vfile.write_text(json.dumps({
    "schema": 1,
    "volumes": [{
        "name": "scratch",
        "topology": "single",
        "fs": "ext4",
        "raid": "",
        "role": "compute",
        "encrypt": "none",
        "auto_mount": True,
        "fs_uuid": "44444444-1111-2222-3333-444444444444",
        "members": [{"part_uuid": "cccc1111-1111-2222-3333-444444444444", "luks_uuid": "", "mapper": "", "keyfile": ""}],
        "mount_options": "",
        "created_at": "2026-11-24T00:00:00Z",
    }],
}), encoding="utf-8")

env = dict(os.environ,
           LOCACHY_VOLUMES_FILE=str(vfile),
           LOCACHY_STORAGE_ROOT=str(sroot),
           LOCACHY_DRIVES_DIR=str(drives),
           LOCACHY_STORAGE_RC=str(rcfile),
           LOCACHY_STORAGE_LSBLK_JSON=str(fixture))

fails = 0
def check(name, ok, detail=""):
    global fails
    print(("PASS" if ok else "FAIL") + ": " + name + ("" if ok or not detail else " -- " + detail), flush=True)
    if not ok:
        fails += 1

def run(*args):
    return subprocess.run(["python3", CLI, *args], env=env, capture_output=True, text=True, timeout=30)

# Toggle compute -> shared: projects into ~/Drives/scratch with nosuid,nodev
r1 = run("set-role", "scratch", "shared")
proj_file = drives / "scratch" / "hello.txt"
mi_lines = Path("/proc/self/mountinfo").read_text(encoding="utf-8").splitlines()
proj_opts = ""
for ln in mi_lines:
    parts = ln.split()
    if len(parts) >= 6 and parts[4] == str(drives / "scratch"):
        proj_opts = parts[5]
check("projection: set-role compute->shared live-projects volume into ~/Drives/<name> with nosuid,nodev",
      r1.returncode == 0
      and proj_file.is_file()
      and proj_file.read_text(encoding="utf-8").strip() == "projected-ok"
      and "nosuid" in proj_opts.split(",")
      and "nodev" in proj_opts.split(","),
      f"rc={r1.returncode} opts={proj_opts!r} out={r1.stdout} err={r1.stderr}")

# Toggle shared -> data: remains projected into ~/Drives/scratch
r2 = run("set-role", "scratch", "data")
check("projection: set-role shared->data keeps ~/Drives/<name> projection",
      r2.returncode == 0 and proj_file.is_file(), r2.stdout + r2.stderr)

# Toggle data -> compute: removes ~/Drives/scratch bind projection cleanly
r3 = run("set-role", "scratch", "compute")
check("projection: set-role data->compute removes ~/Drives/<name> projection cleanly",
      r3.returncode == 0 and not (drives / "scratch").exists() and (mp / "hello.txt").is_file(),
      r3.stdout + r3.stderr)

sys.exit(0 if fails == 0 else 1)
PYEOF
    unshare --user --map-auto --map-root-user --mount --propagation private \
        python3 "$out/ns_driver.py" "$out" "$src" >"$out/ns.log" 2>&1 || :
    grep -E '^(PASS|FAIL|SKIP): ' "$out/ns.log" || cat "$out/ns.log"
    ns_fails=$(grep -c '^FAIL: ' "$out/ns.log" || :)
    fails=$((fails + ns_fails))
else
    note SKIP "projection: unprivileged user/mount namespace unavailable"
fi

echo "storage tests: $fails failed"
[ "$fails" -eq 0 ]
