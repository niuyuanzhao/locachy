#!/usr/bin/env python3
"""Offline device-free tests for locachy-battery-model and its manifest.

Usage:
  python3 -I src/tools/test-battery-model.py [SCRATCH_DIR]

Exercises manifest validation, isolated curl/setpriv/ip-netns argv construction,
exact size and SHA-512 verification, O_NOFOLLOW symlink rejection, atomic
staging and failure cleanup, idempotency, and removal without touching the
network or real system directories.
"""
import errno
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
from unittest import mock

sys.dont_write_bytecode = True

SOURCE = Path(__file__).resolve().parents[1]
SCRIPT = SOURCE / "link/locachy-battery-model"
PROD_MANIFEST = SOURCE / "link/battery-adaptive-manifest.json"

FAILS = 0


def check(label, ok, detail=""):
    global FAILS
    if ok:
        print(f"PASS: {label}", flush=True)
    else:
        FAILS += 1
        suffix = f": {detail}" if detail else ""
        print(f"FAIL: {label}{suffix}", flush=True)


def load_module():
    loader = importlib.machinery.SourceFileLoader("locachy_battery_model", str(SCRIPT))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def make_bytes(seed, size):
    out = bytearray()
    block = hashlib.sha512(seed.encode("ascii")).digest()
    while len(out) < size:
        block = hashlib.sha512(block).digest()
        out.extend(block)
    return bytes(out[:size])


def sha512_hex(data):
    return hashlib.sha512(data).hexdigest()


def write_manifest(path, model_spec, preproc_spec, *, schema_version=1, model_id="TEST_ADAPTIVE_20230314"):
    doc = {
        "schema_version": schema_version,
        "model_id": model_id,
        "source": "Synthetic unit test fixture",
        "artifacts": {
            "model": model_spec,
            "preprocessor": preproc_spec,
        },
    }
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


STUB_CURL_SRC = """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

log_path = Path(os.environ["STUB_CURL_LOG"])
fixtures_dir = Path(os.environ["STUB_CURL_FIXTURES"])
mode = os.environ.get("STUB_CURL_MODE", "ok")

argv = sys.argv[1:]
with log_path.open("a", encoding="utf-8") as f:
    f.write(json.dumps(argv) + "\\n")

url = argv[-1] if argv else ""
name = url.rsplit("/", 1)[-1]
fixture = fixtures_dir / name
if not fixture.is_file():
    sys.stderr.write(f"stub-curl: unknown fixture {name}\\n")
    sys.exit(22)

data = fixture.read_bytes()
if mode == "corrupt-preproc" and name.endswith(".pb"):
    buf = bytearray(data)
    buf[len(buf) // 2] ^= 0x01
    data = bytes(buf)
elif mode == "corrupt-model" and name.endswith(".tflite"):
    buf = bytearray(data)
    buf[0] ^= 0x80
    data = bytes(buf)
elif mode == "oversize":
    data = data + (b"X" * 4096)
elif mode == "truncate":
    data = data[:-7]

sys.stdout.buffer.write(data)
sys.stdout.buffer.flush()
"""

STUB_IP_SRC = """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys

log_path = Path(os.environ["STUB_IP_LOG"])
argv = sys.argv[1:]
with log_path.open("a", encoding="utf-8") as f:
    f.write(json.dumps(argv) + "\\n")

if len(argv) < 4 or argv[0] != "netns" or argv[1] != "exec":
    sys.stderr.write(f"stub-ip: unexpected argv {argv!r}\\n")
    sys.exit(2)

res = subprocess.run(argv[3:])
sys.exit(res.returncode)
"""


def read_jsonl(path):
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def clear_file(path):
    if path.exists():
        path.unlink()


def run_cli(args, env):
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), *args],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )


def run_tests(scratch):
    fixtures = scratch / "fixtures"
    fixtures.mkdir(parents=True, exist_ok=True)

    model_name = "test-adaptive-model.tflite"
    preproc_name = "test-adaptive-preprocessor.pb"
    model_bytes = make_bytes("model-fixture-v1", 8192)
    preproc_bytes = make_bytes("preproc-fixture-v1", 2048)
    model_sha = sha512_hex(model_bytes)
    preproc_sha = sha512_hex(preproc_bytes)

    (fixtures / model_name).write_bytes(model_bytes)
    (fixtures / preproc_name).write_bytes(preproc_bytes)

    manifest_file = scratch / "manifest.json"
    model_spec = {
        "filename": model_name,
        "url": f"https://fixtures.invalid/distfiles/{model_name}",
        "size": len(model_bytes),
        "sha512": model_sha,
    }
    preproc_spec = {
        "filename": preproc_name,
        "url": f"https://fixtures.invalid/distfiles/{preproc_name}",
        "size": len(preproc_bytes),
        "sha512": preproc_sha,
    }
    write_manifest(manifest_file, model_spec, preproc_spec)

    stub_curl = scratch / "stub-curl"
    stub_curl.write_text(STUB_CURL_SRC, encoding="utf-8")
    os.chmod(stub_curl, 0o755)

    stub_bin = scratch / "stub-bin"
    stub_bin.mkdir(exist_ok=True)
    stub_ip = stub_bin / "ip"
    stub_ip.write_text(STUB_IP_SRC, encoding="utf-8")
    os.chmod(stub_ip, 0o755)

    curl_log = scratch / "curl.jsonl"
    ip_log = scratch / "ip.jsonl"

    def make_env(history_dir, *, manifest=manifest_file, mode="ok", netns="", allow_file="0", prepend_stub_bin=False):
        env = os.environ.copy()
        env["LOCACHY_BATTERY_MANIFEST"] = str(manifest)
        env["LOCACHY_BATTERY_HISTORY_DIR"] = str(history_dir)
        env["LOCACHY_BATTERY_NETNS"] = netns
        env["LOCACHY_BATTERY_DROP_PRIVS"] = "0"
        env["LOCACHY_BATTERY_CURL"] = str(stub_curl)
        env["LOCACHY_BATTERY_NO_RELOAD"] = "1"
        env["LOCACHY_BATTERY_ALLOW_FILE_URL"] = allow_file
        env["STUB_CURL_LOG"] = str(curl_log)
        env["STUB_CURL_FIXTURES"] = str(fixtures)
        env["STUB_CURL_MODE"] = mode
        env["STUB_IP_LOG"] = str(ip_log)
        if prepend_stub_bin:
            env["PATH"] = str(stub_bin) + os.pathsep + env.get("PATH", "")
        return env

    # 1. Production manifest parses and validates cleanly.
    prod_hist = scratch / "hist-prod"
    res = run_cli(["status", "--json"], make_env(prod_hist, manifest=PROD_MANIFEST))
    prod_data = json.loads(res.stdout) if res.returncode == 0 else {}
    check(
        "production manifest validates and reports missing on empty dir",
        res.returncode == 0
        and prod_data.get("state") == "missing"
        and prod_data.get("model_id") == "ADAPTIVE_CHARGING_20230314"
        and prod_data.get("artifacts", {}).get("model", {}).get("expected_size") == 124324
        and prod_data.get("artifacts", {}).get("preprocessor", {}).get("expected_size") == 31082,
        f"rc={res.returncode} stdout={res.stdout!r} stderr={res.stderr!r}",
    )

    # 2. Initial missing status and verify failure on synthetic manifest.
    hist = scratch / "hist-main"
    mdir = hist / "model"
    env_ok = make_env(hist)
    res_st = run_cli(["status", "--json"], env_ok)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    check(
        "initial status --json reports missing with exit 0",
        res_st.returncode == 0 and st_json.get("state") == "missing",
        f"rc={res_st.returncode} out={res_st.stdout!r}",
    )
    res_human = run_cli(["status"], env_ok)
    check(
        "initial human status is one line starting with missing",
        res_human.returncode == 0
        and len(res_human.stdout.strip().splitlines()) == 1
        and res_human.stdout.startswith("missing:"),
        f"rc={res_human.returncode} out={res_human.stdout!r}",
    )
    res_ver = run_cli(["verify"], env_ok)
    check(
        "initial verify fails on missing artifacts",
        res_ver.returncode == 1 and "verification failed" in res_ver.stderr,
        f"rc={res_ver.returncode} err={res_ver.stderr!r}",
    )

    # 3. Clean fetch succeeds with exact bytes, mode 0644, no temp leftovers, and curl flags.
    clear_file(curl_log)
    res_fetch = run_cli(["fetch"], env_ok)
    calls = read_jsonl(curl_log)
    leftover_tmp = [p.name for p in mdir.iterdir() if p.name.startswith(".fetch-")] if mdir.is_dir() else ["no-dir"]
    check(
        "clean fetch succeeds and installs exact bytes without temp leftovers",
        res_fetch.returncode == 0
        and (mdir / model_name).read_bytes() == model_bytes
        and (mdir / preproc_name).read_bytes() == preproc_bytes
        and (stat.S_IMODE(os.lstat(mdir / model_name).st_mode) == 0o644)
        and (stat.S_IMODE(os.lstat(mdir / preproc_name).st_mode) == 0o644)
        and (mdir / "manifest.installed.json").is_file()
        and not leftover_tmp
        and len(calls) == 2,
        f"rc={res_fetch.returncode} err={res_fetch.stderr!r} calls={len(calls)} tmp={leftover_tmp}",
    )
    required_flags = {"--disable", "--fail", "--silent", "--show-error", "--location", "--tlsv1.2", "-o", "-"}
    flags_ok = all(
        required_flags.issubset(set(argv))
        and "--proto" in argv and argv[argv.index("--proto") + 1] == "=https"
        and "--proto-redir" in argv and argv[argv.index("--proto-redir") + 1] == "=https"
        and "--max-filesize" in argv
        for argv in calls
    )
    check("fetch passes hardened HTTPS curl flags", flags_ok, f"calls={calls!r}")

    res_st = run_cli(["status", "--json"], env_ok)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    res_ver = run_cli(["verify"], env_ok)
    check(
        "installed model reports status=ready and verify exits 0",
        res_st.returncode == 0
        and st_json.get("state") == "ready"
        and st_json["artifacts"]["model"]["sha512"] == model_sha
        and st_json["artifacts"]["preprocessor"]["sha512"] == preproc_sha
        and res_ver.returncode == 0,
        f"st_rc={res_st.returncode} state={st_json.get('state')} ver_rc={res_ver.returncode}",
    )

    # 4. Idempotent fetch does not touch curl when already verified.
    clear_file(curl_log)
    res_idem = run_cli(["fetch"], env_ok)
    calls = read_jsonl(curl_log)
    check(
        "fetch is idempotent when already verified and skips curl",
        res_idem.returncode == 0
        and "already installed and verified" in res_idem.stdout
        and len(calls) == 0,
        f"rc={res_idem.returncode} out={res_idem.stdout!r} calls={calls!r}",
    )

    # 5. Tampered installed file detected by status=invalid and verify exit 1.
    tampered = bytearray(model_bytes)
    tampered[17] ^= 0xFF
    (mdir / model_name).write_bytes(bytes(tampered))
    res_st = run_cli(["status", "--json"], env_ok)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    res_ver = run_cli(["verify"], env_ok)
    check(
        "tampered installed file reports status=invalid and verify fails",
        res_st.returncode == 0
        and st_json.get("state") == "invalid"
        and st_json["artifacts"]["model"]["state"] == "invalid"
        and "sha512" in st_json["artifacts"]["model"]["reason"]
        and res_ver.returncode == 1,
        f"st={st_json} ver_rc={res_ver.returncode} ver_err={res_ver.stderr!r}",
    )

    # 6. Partial re-fetch preserves valid sibling on failure, then repairs only damaged file.
    (mdir / model_name).write_bytes(model_bytes)
    bad_pre = bytearray(preproc_bytes)
    bad_pre[3] ^= 0x01
    (mdir / preproc_name).write_bytes(bytes(bad_pre))
    clear_file(curl_log)
    res_bad = run_cli(["fetch"], make_env(hist, mode="corrupt-preproc"))
    leftover_tmp = [p.name for p in mdir.iterdir() if p.name.startswith(".fetch-")]
    check(
        "failed re-fetch preserves existing valid model and leaves no temp files",
        res_bad.returncode == 1
        and "sha512 mismatch" in res_bad.stderr
        and (mdir / model_name).read_bytes() == model_bytes
        and not leftover_tmp,
        f"rc={res_bad.returncode} err={res_bad.stderr!r} tmp={leftover_tmp}",
    )
    clear_file(curl_log)
    res_repair = run_cli(["fetch"], env_ok)
    repair_calls = read_jsonl(curl_log)
    check(
        "successful re-fetch downloads only the invalid artifact",
        res_repair.returncode == 0
        and len(repair_calls) == 1
        and repair_calls[0][-1].endswith(preproc_name)
        and (mdir / preproc_name).read_bytes() == preproc_bytes,
        f"rc={res_repair.returncode} calls={repair_calls!r}",
    )

    # 7. Incomplete state when one file is missing.
    (mdir / preproc_name).unlink()
    res_st = run_cli(["status", "--json"], env_ok)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    res_ver = run_cli(["verify"], env_ok)
    check(
        "single missing file reports status=incomplete and verify fails",
        res_st.returncode == 0 and st_json.get("state") == "incomplete" and res_ver.returncode == 1,
        f"st={st_json.get('state')} ver_rc={res_ver.returncode}",
    )

    # 8. Symlinked artifact pointing to valid bytes is rejected (O_NOFOLLOW).
    (mdir / preproc_name).write_bytes(preproc_bytes)
    (mdir / model_name).unlink()
    os.symlink(fixtures / model_name, mdir / model_name)
    res_st = run_cli(["status", "--json"], env_ok)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    res_ver = run_cli(["verify"], env_ok)
    check(
        "symlink artifact is rejected by status and verify",
        res_st.returncode == 0
        and st_json.get("state") == "invalid"
        and "symlink" in st_json["artifacts"]["model"]["reason"]
        and res_ver.returncode == 1,
        f"st={st_json} ver_rc={res_ver.returncode}",
    )

    # 9. Symlinked model directory is rejected by status, verify, and fetch.
    sym_hist = scratch / "hist-symdir"
    sym_hist.mkdir(parents=True, exist_ok=True)
    real_dir = scratch / "real-model-dir"
    real_dir.mkdir(parents=True, exist_ok=True)
    (real_dir / model_name).write_bytes(model_bytes)
    (real_dir / preproc_name).write_bytes(preproc_bytes)
    os.symlink(real_dir, sym_hist / "model")
    env_sym = make_env(sym_hist)
    res_st = run_cli(["status", "--json"], env_sym)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    res_ver = run_cli(["verify"], env_sym)
    res_f = run_cli(["fetch"], env_sym)
    check(
        "symlinked model directory is rejected by status, verify, and fetch",
        res_st.returncode == 0
        and st_json.get("state") == "invalid"
        and res_ver.returncode == 1
        and res_f.returncode == 1,
        f"st={st_json.get('state')} ver_rc={res_ver.returncode} fetch_rc={res_f.returncode}",
    )

    # 10. Corrupted byte on fresh fetch fails SHA-512 and leaves no partial or installed files.
    hist_corrupt = scratch / "hist-corrupt"
    res_c = run_cli(["fetch"], make_env(hist_corrupt, mode="corrupt-preproc"))
    cdir = hist_corrupt / "model"
    entries = sorted(p.name for p in cdir.iterdir()) if cdir.is_dir() else []
    check(
        "corrupted byte fails SHA-512 and leaves zero partial or installed files",
        res_c.returncode == 1 and "sha512 mismatch" in res_c.stderr and entries == [],
        f"rc={res_c.returncode} err={res_c.stderr!r} entries={entries!r}",
    )

    # 11. Oversized payload fails and leaves no partial or installed files.
    hist_over = scratch / "hist-over"
    res_o = run_cli(["fetch"], make_env(hist_over, mode="oversize"))
    odir = hist_over / "model"
    entries = sorted(p.name for p in odir.iterdir()) if odir.is_dir() else []
    check(
        "oversized payload aborts and leaves zero partial or installed files",
        res_o.returncode == 1 and "exceeded expected size" in res_o.stderr and entries == [],
        f"rc={res_o.returncode} err={res_o.stderr!r} entries={entries!r}",
    )

    # 12. Truncated payload fails size check and leaves no partial or installed files.
    hist_trunc = scratch / "hist-trunc"
    res_t = run_cli(["fetch"], make_env(hist_trunc, mode="truncate"))
    tdir = hist_trunc / "model"
    entries = sorted(p.name for p in tdir.iterdir()) if tdir.is_dir() else []
    check(
        "truncated payload fails size check and leaves zero partial or installed files",
        res_t.returncode == 1 and "size mismatch" in res_t.stderr and entries == [],
        f"rc={res_t.returncode} err={res_t.stderr!r} entries={entries!r}",
    )

    # 13. remove cleans artifacts, metadata, stale .fetch-* files, and empty model dir.
    hist_rm = scratch / "hist-remove"
    env_rm = make_env(hist_rm)
    res_f = run_cli(["fetch"], env_rm)
    rm_mdir = hist_rm / "model"
    (rm_mdir / ".fetch-stale-123.tmp").write_bytes(b"stale")
    res_rm = run_cli(["remove"], env_rm)
    res_st = run_cli(["status", "--json"], env_rm)
    st_json = json.loads(res_st.stdout) if res_st.returncode == 0 else {}
    check(
        "remove deletes artifacts, metadata, stale temp files, and empty directory",
        res_f.returncode == 0
        and res_rm.returncode == 0
        and not rm_mdir.exists()
        and res_st.returncode == 0
        and st_json.get("state") == "missing",
        f"f_rc={res_f.returncode} rm_rc={res_rm.returncode} exists={rm_mdir.exists()} st={st_json.get('state')}",
    )

    # 14. Non-HTTPS URL rejected without invoking curl.
    bad_http_manifest = scratch / "manifest-http.json"
    bad_model_spec = dict(model_spec, url=f"http://fixtures.invalid/distfiles/{model_name}")
    write_manifest(bad_http_manifest, bad_model_spec, preproc_spec)
    clear_file(curl_log)
    res_http = run_cli(["fetch"], make_env(scratch / "hist-http", manifest=bad_http_manifest))
    check(
        "http:// manifest URL is rejected without invoking curl",
        res_http.returncode == 1
        and "https://" in res_http.stderr
        and len(read_jsonl(curl_log)) == 0,
        f"rc={res_http.returncode} err={res_http.stderr!r}",
    )

    # 15. file:// URL rejected by default, accepted only with LOCACHY_BATTERY_ALLOW_FILE_URL=1.
    file_manifest = scratch / "manifest-file.json"
    file_model_spec = dict(model_spec, url=(fixtures / model_name).as_uri())
    file_preproc_spec = dict(preproc_spec, url=(fixtures / preproc_name).as_uri())
    write_manifest(file_manifest, file_model_spec, file_preproc_spec)
    clear_file(curl_log)
    res_file_denied = run_cli(["fetch"], make_env(scratch / "hist-file-denied", manifest=file_manifest, allow_file="0"))
    check(
        "file:// URL is rejected when LOCACHY_BATTERY_ALLOW_FILE_URL is not enabled",
        res_file_denied.returncode == 1 and len(read_jsonl(curl_log)) == 0,
        f"rc={res_file_denied.returncode} err={res_file_denied.stderr!r}",
    )
    clear_file(curl_log)
    res_file_ok = run_cli(["fetch"], make_env(scratch / "hist-file-ok", manifest=file_manifest, allow_file="1"))
    file_calls = read_jsonl(curl_log)
    check(
        "file:// URL succeeds when LOCACHY_BATTERY_ALLOW_FILE_URL=1",
        res_file_ok.returncode == 0
        and len(file_calls) == 2
        and all(argv[argv.index("--proto") + 1] == "=file" for argv in file_calls),
        f"rc={res_file_ok.returncode} err={res_file_ok.stderr!r} calls={file_calls!r}",
    )

    # 16. End-to-end ip netns exec argv construction via stub ip wrapper.
    clear_file(ip_log)
    clear_file(curl_log)
    hist_ns = scratch / "hist-netns"
    res_ns = run_cli(["fetch"], make_env(hist_ns, netns="locachy-test-ns", prepend_stub_bin=True))
    ip_calls = read_jsonl(ip_log)
    check(
        "fetch wraps curl with ip netns exec <ns> when LOCACHY_BATTERY_NETNS is non-empty",
        res_ns.returncode == 0
        and len(ip_calls) == 2
        and all(c[:4] == ["netns", "exec", "locachy-test-ns", str(stub_curl)] for c in ip_calls),
        f"rc={res_ns.returncode} err={res_ns.stderr!r} ip_calls={ip_calls!r}",
    )

    # 17. Direct module unit checks for argv construction, manifest validation, and reload signaling.
    mod = load_module()
    argv_root_ns = mod.build_download_argv(
        model_spec["url"],
        model_spec["size"],
        curl_bin="curl",
        netns="locachy-net",
        drop_privs=True,
        allow_file_url=False,
        euid=0,
        already_in_netns=False,
    )
    check(
        "root download argv places ip netns exec outermost and setpriv nobody before curl",
        argv_root_ns[:12] == [
            "ip", "netns", "exec", "locachy-net",
            "setpriv", "--reuid=65534", "--regid=65534", "--clear-groups", "--no-new-privs", "--",
            "curl", "--disable",
        ]
        and argv_root_ns[-2:] == ["--", model_spec["url"]],
        f"argv={argv_root_ns!r}",
    )

    argv_nonroot_ns = mod.build_download_argv(
        model_spec["url"],
        model_spec["size"],
        curl_bin="curl",
        netns="locachy-net",
        drop_privs=True,
        allow_file_url=False,
        euid=1000,
        already_in_netns=False,
    )
    check(
        "non-root download argv keeps ip netns exec and omits setpriv",
        argv_nonroot_ns[:5] == ["ip", "netns", "exec", "locachy-net", "curl"]
        and "setpriv" not in argv_nonroot_ns,
        f"argv={argv_nonroot_ns!r}",
    )

    argv_in_ns = mod.build_download_argv(
        model_spec["url"],
        model_spec["size"],
        curl_bin="curl",
        netns="locachy-net",
        drop_privs=False,
        allow_file_url=False,
        euid=0,
        already_in_netns=True,
    )
    check(
        "already-in-netns argv skips ip netns exec",
        argv_in_ns[0] == "curl" and "ip" not in argv_in_ns,
        f"argv={argv_in_ns!r}",
    )

    bad_ns_rejected = False
    try:
        mod.build_download_argv(
            model_spec["url"],
            model_spec["size"],
            curl_bin="curl",
            netns="../evil",
            drop_privs=False,
            allow_file_url=False,
            euid=1000,
            already_in_netns=False,
        )
    except mod.Failure:
        bad_ns_rejected = True
    check("invalid netns name with path traversal is rejected", bad_ns_rejected)

    bad_manifests = [
        ("dot-prefixed filename", dict(model_spec, filename=".hidden.tflite"), preproc_spec),
        ("path separator filename", dict(model_spec, filename="sub/model.tflite"), preproc_spec),
        ("dotdot filename", dict(model_spec, filename="a..b.tflite"), preproc_spec),
        ("boolean size", dict(model_spec, size=True), preproc_spec),
        ("zero size", dict(model_spec, size=0), preproc_spec),
        ("uppercase sha512", dict(model_spec, sha512=model_sha.upper()), preproc_spec),
        ("short sha512", dict(model_spec, sha512="deadbeef"), preproc_spec),
        ("duplicate filenames", model_spec, dict(preproc_spec, filename=model_name)),
    ]
    for desc, ms, ps in bad_manifests:
        rejected = False
        try:
            mod.validate_manifest(
                {"schema_version": 1, "model_id": "M1", "source": "t", "artifacts": {"model": ms, "preprocessor": ps}}
            )
        except mod.Failure:
            rejected = True
        check(f"manifest validation rejects {desc}", rejected)

    with mock.patch.dict(os.environ, {"LOCACHY_BATTERY_NO_RELOAD": "0"}), \
            mock.patch.object(mod.os, "geteuid", return_value=0), \
            mock.patch.object(mod.os, "kill") as kill_mock:
        mod.notify_reload()
        check(
            "notify_reload signals PID 1 with SIGHUP when root and enabled",
            kill_mock.call_args_list == [mock.call(1, signal.SIGHUP)],
            f"calls={kill_mock.call_args_list!r}",
        )

    with mock.patch.dict(os.environ, {"LOCACHY_BATTERY_NO_RELOAD": "1"}), \
            mock.patch.object(mod.os, "geteuid", return_value=0), \
            mock.patch.object(mod.os, "kill") as kill_mock:
        mod.notify_reload()
        check("notify_reload skips SIGHUP when LOCACHY_BATTERY_NO_RELOAD=1", not kill_mock.called)

    for err_no, name in ((errno.ESRCH, "ESRCH"), (errno.EPERM, "EPERM")):
        with mock.patch.dict(os.environ, {"LOCACHY_BATTERY_NO_RELOAD": "0"}), \
                mock.patch.object(mod.os, "geteuid", return_value=0), \
                mock.patch.object(mod.os, "kill", side_effect=OSError(err_no, name)):
            try:
                mod.notify_reload()
                ignored = True
            except OSError:
                ignored = False
            check(f"notify_reload ignores {name} from kill(1, SIGHUP)", ignored)


def main():
    if len(sys.argv) > 2:
        print("usage: test-battery-model.py [SCRATCH_DIR]", file=sys.stderr)
        return 2
    if len(sys.argv) == 2:
        scratch = Path(sys.argv[1]).resolve()
        scratch.mkdir(parents=True, exist_ok=True)
        run_tests(scratch)
    else:
        with tempfile.TemporaryDirectory(prefix="test-battery-model-") as tmp:
            run_tests(Path(tmp).resolve())
    if FAILS:
        print(f"SUMMARY: {FAILS} failure(s)", file=sys.stderr, flush=True)
        return 1
    print("ALL PASSED", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
