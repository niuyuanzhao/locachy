#!/usr/bin/env python3
"""locachy-winiso against a local HTTPS server standing in for Microsoft: the
download page with its table of SHA-256 values, the session permit, the two
answers of the download service (language list, link) and the ISO's address
with ranges; a job queue double stands in for the system job service. Judged
by PASS/FAIL lines.

  python3 -I tools/test-winiso.py [--live]

Offline by default. --live (or LOCACHY_WINISO_LIVE=1) also runs one live check
against Microsoft after the mock suite. Needs Linux, curl and openssl."""
import argparse
import getpass
import hashlib
import http.server
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlsplit

SOURCE = Path(__file__).resolve().parents[1]
MiB = 1 << 20
fails = 0
PAGE_PATHS = ("/en-us/software-download/windows11",)
def _build_forbidden():
    words = ["locachy", "@"]
    try:
        user = getpass.getuser()
    except Exception:
        user = ""
    if user:
        words.append(user.lower())
    return tuple(words)


# Words that must not reach Microsoft in a request (the project, the person, an e-mail).
FORBIDDEN = _build_forbidden()
# (Language, LocalizedLanguage): the names Microsoft's language list uses.
LANGUAGE_LIST = [
    ("Arabic", "Arabic"), ("Brazilian Portuguese", "Brazilian Portuguese"), ("Bulgarian", "Bulgarian"),
    ("Chinese (Simplified)", "Chinese Simplified"), ("Chinese (Traditional)", "Chinese Traditional"),
    ("Croatian", "Croatian"), ("Czech", "Czech"), ("Danish", "Danish"), ("Dutch", "Dutch"),
    ("English", "English (United States)"), ("English (United Kingdom)", "English International"),
    ("Estonian", "Estonian"), ("Finnish", "Finnish"), ("French", "French"), ("French Canadian", "French Canadian"),
    ("German", "German"), ("Greek", "Greek"), ("Hebrew", "Hebrew"), ("Hungarian", "Hungarian"),
    ("Italian", "Italian"), ("Japanese", "Japanese"), ("Korean", "Korean"), ("Latvian", "Latvian"),
    ("Lithuanian", "Lithuanian"), ("Norwegian", "Norwegian"), ("Polish", "Polish"), ("Portuguese", "Portuguese"),
    ("Romanian", "Romanian"), ("Russian", "Russian"), ("Serbian Latin", "Serbian Latin"), ("Slovak", "Slovak"),
    ("Slovenian", "Slovenian"), ("Spanish", "Spanish"), ("Spanish (Mexico)", "Spanish (Mexico)"),
    ("Swedish", "Swedish"), ("Thai", "Thai"), ("Turkish", "Turkish"), ("Ukrainian", "Ukrainian"),
]


def check(label, ok, detail=""):
    global fails
    if ok:
        print(f"PASS: {label}", flush=True)
    else:
        fails += 1
        print(f"FAIL: {label}{': ' + detail if detail else ''}", flush=True)


def payload(seed, size=3 * MiB):
    out = bytearray()
    block = hashlib.sha256(seed.encode()).digest()
    while len(out) < size:
        block = hashlib.sha256(block).digest()
        out += block * 64
    return bytes(out[:size])


def name_of(localized):
    return "Win11_Test_" + re.sub("[^A-Za-z]", "", localized) + "_x64.iso"


class World:
    """What the fake Microsoft serves, and what it was asked."""

    def __init__(self):
        self.isos = {}          # ISO file name -> bytes
        self.table = {}         # row of the page's table -> published SHA-256
        self.drop_once = {}     # ISO file name -> bytes served before the transfer is cut
        self.refuse = None      # None; "sentinel", "sentinel-nonjson", "api-error", "blocked"
        self.range_status = 206  # reply to exactly Range: bytes=0-0
        self.range_header = None  # override Content-Range on 206 for malformed-response tests
        self.link_options = None  # optional callable(world, loc) -> list of ProductDownloadOptions dicts
        self.sku_extra = {}     # optional extra fields merged into language_list JSON
        self.session = None     # the session the permit set; every later API request must carry it
        self.base = ""
        self.page_url = ""
        self.sku_of = {loc: str(27100 + i) for i, (_, loc) in enumerate(LANGUAGE_LIST)}
        self.lang_of = {sku: loc for loc, sku in self.sku_of.items()}
        self.log = []           # one dict per request: kind, bytes, and what it asked for
        self.violations = []
        self.lock = threading.Lock()

    def record(self, kind, nbytes=0, **extra):
        with self.lock:
            self.log.append(dict(kind=kind, bytes=nbytes, **extra))

    def violate(self, what):
        with self.lock:
            self.violations.append(what)

    def count(self, kind, since=0):
        return sum(1 for e in self.log[since:] if e["kind"] == kind)

    def page(self):
        rows = "".join(f"<tr><td>{row} 64-bit</td><td>{digest.upper()}</td></tr>" for row, digest in self.table.items())
        return ("<html><body><h2>Verify your download</h2><p>Hash values for the ISO files for Each Language</p>"
                "<table class=\"table table-bordered\"><tbody><tr><th>Country Locale</th><th>Hash Code</th></tr>"
                f"{rows}</tbody></table><option value=\"3813\">Windows 11 (multi-edition ISO for x64 devices)</option>"
                "</body></html>").encode()

    def language_list(self):
        body = {"ValidationContainer": {"Errors": []}, "Skus": [
            {"Id": self.sku_of[loc], "Language": lang, "LocalizedLanguage": loc,
             "ProductDisplayName": "Windows 11 Client - Build 26300"} for lang, loc in LANGUAGE_LIST]}
        body.update(self.sku_extra)
        return json.dumps(body).encode()


def make_handler(world):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, body, ctype="application/json", headers=()):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in headers:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def outbound_check(self, query):
            """What Microsoft may see: a generic client, nothing of the project, the host or the person."""
            sent = self.path + "\n" + "\n".join(f"{k}: {v}" for k, v in self.headers.items())
            if self.headers.get("User-Agent") != "winiso/1":
                world.violate(f"User-Agent {self.headers.get('User-Agent')!r}")
            for word in FORBIDDEN + (socket.gethostname().lower(),):
                if (word == "@" or len(word) >= 3) and word in sent.lower():
                    world.violate(f"{word!r} in a request")
            if "sessionID" in query and world.session is not None and query["sessionID"] != [world.session]:
                world.violate("a session id other than the permit's")

        def do_GET(self):
            url = urlsplit(self.path)
            path, query = url.path, parse_qs(url.query)
            rng = self.headers.get("Range", "")
            self.outbound_check(query)
            if path in PAGE_PATHS:
                body = world.page()
                world.record("page", len(body))
                return self.reply(200, body, "text/html", [("Set-Cookie", "ms_page=1; Path=/")])
            if path == "/tags":
                if query.get("org_id") != ["y6jn8c31"]:
                    world.violate("the permit's org id")
                world.session = query.get("session_id", [""])[0]
                world.record("tags")
                return self.reply(200, b"", "text/plain",
                                  [("Set-Cookie", f"ms_tag={world.session}; Path=/")])
            if path == "/mdt.js":
                if query.get("instanceId") != ["560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175"]:
                    world.violate("ov-df mdt.js instanceId")
                if query.get("PageId") != ["si"] or query.get("session_id") != [world.session]:
                    world.violate("ov-df mdt.js session_id/PageId")
                world.record("mdt")
                js = b'var u="?w=CAFEBABE01";var rticks="+1700000000";'
                return self.reply(200, js, "application/javascript")
            if path == "/":
                if (query.get("CustomerId") != ["560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175"]
                        or query.get("PageId") != ["si"]
                        or query.get("session_id") != [world.session]
                        or query.get("w") != ["CAFEBABE01"]
                        or query.get("rticks") != ["1700000000"]
                        or not re.fullmatch(r"\d{10,16}", query.get("mdt", [""])[0])):
                    world.violate(f"ov-df reply query {query!r}")
                world.record("ovdf")
                return self.reply(200, b"", "text/plain",
                                  [("Set-Cookie", f"ms_ov={world.session}; Path=/")])
            if path.endswith("/getskuinformationbyproductedition"):
                ed = (query.get("productEditionId") or query.get("ProductEditionId") or [""])[0]
                world.record("sku", edition=ed)
                if world.refuse == "blocked":
                    return self.reply(403, b"<html><body>Access Denied</body></html>", "text/html")
                return self.reply(200, world.language_list(),
                                  headers=[("Set-Cookie", f"ms_sku={world.session}; Path=/")])
            if path.endswith("/GetProductDownloadLinksBySku"):
                sku, referer = query.get("SKU", [""])[0], self.headers.get("Referer", "")
                cookie = self.headers.get("Cookie", "")
                world.record("link", sku=sku, referer=referer)
                if referer != world.page_url:
                    world.violate("the link request's referer is not the download page")
                if world.session and f"ms_ov={world.session}" not in cookie:
                    world.violate("the link request did not carry the session cookies")
                if world.refuse == "sentinel":
                    return self.reply(200, json.dumps({"Errors": [{
                        "Key": "ErrorSettings.SentinelReject", "Value": "Sentinel marked this request as rejected.",
                        "Type": 8}]}).encode())
                if world.refuse == "sentinel-nonjson":
                    return self.reply(200, b"<html>Sentinel rejected this request</html>", "text/html")
                if world.refuse == "api-error":
                    return self.reply(200, json.dumps({"Errors": [{
                        "Key": "ErrorSettings.Generic", "Value": "Temporary catalog error.",
                        "Type": 2}]}).encode())
                loc = world.lang_of.get(sku)
                if loc is None:
                    return self.reply(404, b"", "text/plain")
                if world.link_options is not None:
                    opts = world.link_options(world, loc)
                    return self.reply(200, json.dumps({"ProductDownloadOptions": opts}).encode())
                uri = f"{world.base}/dbazure/{name_of(loc)}?t=token&P1=1"
                return self.reply(200, json.dumps({"ProductDownloadOptions": [{"DownloadType": 1, "Uri": uri}]}).encode())
            name = path.rsplit("/", 1)[-1]
            data = world.isos.get(name) if path.startswith("/dbazure/") else None
            if data is None:
                return self.reply(404, b"", "text/plain")
            if rng == "bytes=0-0":
                code = world.range_status
                world.record("iso", 1 if code == 206 else 0, range=rng, status=code)
                if code == 206:
                    header = world.range_header if world.range_header is not None else f"bytes 0-0/{len(data)}"
                    return self.reply(code, data[:1], "application/octet-stream", [("Content-Range", header)])
                if code == 200:
                    # Advertise a full response, but send no body: curl must stop at its size limit.
                    self.send_response(code)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    return
                if code == 302:
                    return self.reply(code, b"", "text/plain", [("Location", self.path)])
                return self.reply(code, b"", "text/plain")
            start = 0
            m = re.fullmatch(r"bytes=(\d+)-", rng)
            if m:
                start = int(m.group(1))
                if start >= len(data):
                    world.record("iso", 0, range=rng)
                    return self.reply(416, b"", "text/plain", [("Content-Range", f"bytes */{len(data)}")])
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
            else:
                self.send_response(200)
            self.send_header("Content-Length", str(len(data) - start))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            body = data[start:]
            with world.lock:
                cut = world.drop_once.pop(name, None)
            if cut is not None:
                body = body[:cut]
            self.wfile.write(body)
            self.wfile.flush()
            world.record("iso", len(body), range=rng)
            if cut is not None:
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)

    return Handler


def run_live():
    # Ignore mock endpoint overrides; use the program's real Microsoft URLs.
    env = {k: v for k, v in os.environ.items() if not k.startswith("LOCACHY_WINISO_")}
    r = subprocess.run([sys.executable, "-I", str(SOURCE / "link/locachy-winiso"), "check", "--lang", "en-US"],
                       env=env, capture_output=True, text=True, errors="replace")
    if r.returncode == 0:
        outcome = "PASS: live Windows ISO check"
    elif r.returncode == 4:
        if "Microsoft refused the automated download" in r.stderr:
            outcome = "FAIL: live Windows ISO check: Microsoft Sentinel refusal"
        elif any(note in r.stderr for note in (
                "lists no single SHA-256", "names no product edition", "gave no usable tokens",
                "not a usable answer", "no usable answer", "has no single entry", "gave no single x64 link",
                "gave a malformed link URL", "Microsoft's link is not on", "does not name an ISO")):
            outcome = "FAIL: live Windows ISO check: Microsoft response format or parsing error"
        else:
            outcome = "FAIL: live Windows ISO check: Microsoft refusal or upstream error"
    else:
        outcome = "FAIL: live Windows ISO check: local or usage error"
    print(f"{outcome} (exit {r.returncode})", flush=True)
    print(f"live stdout: {r.stdout!r}\nlive stderr: {r.stderr!r}", flush=True)
    return int(r.returncode != 0)


def main():
    parser = argparse.ArgumentParser(description="Test locachy-winiso against an offline mock server")
    parser.add_argument("--live", action="store_true", help="also run one check against Microsoft")
    args = parser.parse_args()
    if sys.platform != "linux" or not shutil.which("curl") or not shutil.which("openssl"):
        print("SKIP: test-winiso.py needs Linux, curl and openssl")
        return 77
    with tempfile.TemporaryDirectory(prefix="locachy-winiso-test.") as tmp:
        result = run(Path(tmp))
    if args.live or os.environ.get("LOCACHY_WINISO_LIVE") == "1":
        result |= run_live()
    return result


def run(tmp):
    cert, key = tmp / "cert.pem", tmp / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert),
                    "-days", "1", "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, capture_output=True)
    world = World()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_handler(world))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(cert), str(key))
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"https://127.0.0.1:{server.server_address[1]}"
    world.base, world.page_url = base, base + PAGE_PATHS[0]

    for localized, row in (("English (United States)", "English"), ("German", "German"),
                           ("Chinese Simplified", "Chinese Simplified"), ("Japanese", "Japanese"),
                           ("Korean", "Korean"), ("Polish", "Polish"), ("French", "French"),
                           ("Italian", "Italian")):
        world.isos[name_of(localized)] = data = payload(localized)
        world.table[row] = hashlib.sha256(data).hexdigest()
    world.table["German"] = hashlib.sha256(b"another build").hexdigest()  # de-DE's table names other bytes

    # The program beside a job queue double (locachy-winiso loads locachy_jobs.py beside it).
    prog = tmp / "prog"
    prog.mkdir()
    shutil.copy(SOURCE / "link/locachy-winiso", prog / "locachy-winiso")
    (prog / "locachy_jobs.py").write_text('''import json, os
def execution_mode():
    return os.environ.get("TEST_JOBS_MODE", "offline").split("-")[0]
def submit(kind, args=(), secret=None, wait=True, startup_wait=0):
    if os.environ.get("TEST_JOBS_MODE") == "queue-refuse":
        raise RuntimeError("job service: unknown kind")
    with open(os.environ["TEST_JOBS_LOG"], "a") as f:
        f.write(json.dumps([kind, list(args)]) + "\\n")
    return 0
''', encoding="utf-8")

    isos, rc, jobs_log = tmp / "isos", tmp / "rc.conf", tmp / "jobs.log"
    virtio = tmp / "libvirt/virtio-win.iso"
    rc.write_text("", encoding="utf-8")
    env = dict(os.environ, LOCACHY_ISO_DIR=str(isos), LOCACHY_VIRTIO_WIN_ISO=str(virtio), LOCACHY_RC_CONF=str(rc),
               LOCACHY_WINISO_MS_BASE=base, LOCACHY_WINISO_TAGS_BASE=base,
               LOCACHY_WINISO_OVDF_BASE=base, LOCACHY_WINISO_LINK_PREFIX=base + "/dbazure/",
               CURL_CA_BUNDLE=str(cert), no_proxy="*", TEST_JOBS_LOG=str(jobs_log), TEST_JOBS_MODE="offline")

    def winiso(*args, **extra):
        return subprocess.run([sys.executable, "-I", str(prog / "locachy-winiso"), *args],
                              env=dict(env, **extra), capture_output=True, text=True, timeout=120)

    def final_of(version, tag):
        return isos / f"windows-{version}-{tag}.iso"

    # --- path ---
    r = winiso("path", "virtio-win")
    check("path virtio-win without the package's ISO: status 3, nothing printed", r.returncode == 3 and r.stdout == "",
          f"{r.returncode} {r.stdout!r} {r.stderr!r}")
    virtio.parent.mkdir()
    virtio.write_bytes(b"virtio")
    r = winiso("path", "virtio-win")
    check("path virtio-win with the package's ISO: status 0, its one absolute path",
          r.returncode == 0 and r.stdout.splitlines() == [str(virtio.resolve())], f"{r.returncode} {r.stdout!r}")
    r = winiso("path", "windows")
    check("path windows before any fetch: status 3", r.returncode == 3 and r.stdout == "", f"{r.returncode} {r.stdout!r}")
    r = winiso("fetch", "windows", "--lang", "eo")
    check("fetch --lang in a language Microsoft has no ISO in: status 2", r.returncode == 2, f"{r.returncode} {r.stderr!r}")

    # --- check: no ISO directory, no system job, one bounded range request ---
    chain = ["page", "tags", "mdt", "ovdf", "sku", "link"]
    probe_tmp = tmp / "check-tmp"
    probe_tmp.mkdir()

    def probe(*args, **extra):
        return winiso("check", *args, TMPDIR=str(probe_tmp), TEST_JOBS_MODE="queue", **extra)

    def request_kinds(mark):
        return [e["kind"] for e in world.log[mark:]]

    english = "English (United States)"
    summary = f"ok: windows-11 en-US {name_of(english)} sha256={world.table['English']} range="
    mark = len(world.log)
    r = probe()
    check("check: default en-US, one full chain and one byte, no ISO directory or system job",
          r.returncode == 0 and r.stdout == summary + "206\n" and not r.stderr
          and request_kinds(mark) == chain + ["iso"]
          and world.log[-1] == dict(kind="iso", bytes=1, range="bytes=0-0", status=206)
          and not isos.exists() and not jobs_log.exists() and not list(probe_tmp.iterdir()),
          f"{r.returncode} {r.stdout!r} {r.stderr!r} {world.log[mark:]}")

    # A verified ISO and a stale fetch cookie jar must not change check's flow or be modified.
    readonly = tmp / "readonly-isos"
    readonly_dl = readonly / ".partial/dl"
    readonly_dl.mkdir(parents=True)
    installed = readonly / "windows-11-en-US.iso"
    installed.write_bytes(b"placeholder")
    record = installed.with_suffix(".iso.json")
    record.write_text(json.dumps({"size": installed.stat().st_size, "sha256": world.table["English"]}))
    stale = readonly_dl / ".session.stale.cookies"
    stale.write_bytes(b"keep")
    entries = [readonly, *sorted(readonly.rglob("*"))]
    dirs = [p for p in entries if p.is_dir()]
    for p in dirs:
        p.chmod(0o555)
    before = [p.stat() for p in entries]
    rc.write_text('language="zh_CN.UTF-8"\n', encoding="utf-8")
    try:
        mark = len(world.log)
        r = probe("--lang", "en_US.UTF-8", LOCACHY_ISO_DIR=str(readonly))
        after = [p.stat() for p in entries if p.exists()]
        check("check: --lang overrides rc.conf; an existing read-only ISO tree is unchanged",
              r.returncode == 0 and r.stdout == summary + "206\n" and request_kinds(mark) == chain + ["iso"]
              and world.log[mark + 5]["sku"] == world.sku_of[english]
              and entries == [readonly, *sorted(readonly.rglob("*"))] and before == after,
              f"{r.returncode} {r.stdout!r} {r.stderr!r} {world.log[mark:]}")
    finally:
        for p in dirs:
            p.chmod(0o755)
    mark = len(world.log)
    r = probe()
    check("check: uses rc.conf's language when --lang is omitted",
          r.returncode == 0 and r.stdout.startswith("ok: windows-11 zh-CN ")
          and request_kinds(mark) == chain + ["iso"] and world.log[mark + 5]["sku"] == world.sku_of["Chinese Simplified"],
          f"{r.returncode} {r.stdout!r} {r.stderr!r} {world.log[mark:]}")
    rc.write_text('language="eo"\n', encoding="utf-8")
    mark = len(world.log)
    r = probe()
    check("check: unsupported rc.conf language falls back to en-US, with a notice",
          r.returncode == 0 and r.stdout.endswith(summary + "206\n") and "rc.conf's language eo" in r.stdout
          and request_kinds(mark) == chain + ["iso"], f"{r.returncode} {r.stdout!r} {r.stderr!r}")
    rc.write_text("", encoding="utf-8")
    mark = len(world.log)
    r = probe("--lang", "eo")
    check("check: unsupported explicit language is usage error 2, without a request",
          r.returncode == 2 and not world.log[mark:], f"{r.returncode} {r.stderr!r}")

    world.refuse = "sentinel"
    mark = len(world.log)
    r = probe("--lang", "en-US")
    check("check: Sentinel refusal returns 4 without retry or range request; temporary jar cleaned",
          r.returncode == 4 and not r.stdout and "Sentinel marked this request as rejected" in r.stderr
          and request_kinds(mark) == chain and not list(probe_tmp.iterdir()),
          f"{r.returncode} {r.stderr!r} {world.log[mark:]}")
    world.refuse = None
    for status in (403, 404, 302):
        world.range_status = status
        mark = len(world.log)
        r = probe("--lang", "en-US")
        check(f"check: range HTTP {status} returns 4 without retry",
              r.returncode == 4 and not r.stdout and f"HTTP {status}" in r.stderr
              and request_kinds(mark) == chain + ["iso"] and world.log[-1]["range"] == "bytes=0-0",
              f"{r.returncode} {r.stderr!r} {world.log[mark:]}")
    world.range_status = 200
    mark = len(world.log)
    r = probe("--lang", "en-US")
    check("check: rejects range HTTP 200 without downloading an ISO",
          r.returncode == 4 and not r.stdout and "HTTP 200" in r.stderr
          and request_kinds(mark) == chain + ["iso"],
          f"{r.returncode} {r.stdout!r} {r.stderr!r} {world.log[mark:]}")
    world.range_status = 206
    world.range_header = "bytes 0-2/999"
    mark = len(world.log)
    r = probe("--lang", "en-US")
    check("check: rejects malformed HTTP 206 range metadata",
          r.returncode == 4 and not r.stdout and "HTTP 206" in r.stderr
          and request_kinds(mark) == chain + ["iso"],
          f"{r.returncode} {r.stdout!r} {r.stderr!r} {world.log[mark:]}")
    world.range_header = None
    world.link_options = lambda w, loc: [{"DownloadType": 1, "Uri": f"{w.base}/{name_of(loc)}"}]
    mark = len(world.log)
    r = probe("--lang", "en-US")
    check("check: refuses a link outside the download prefix without probing it",
          r.returncode == 4 and "Microsoft's link is not on" in r.stderr and request_kinds(mark) == chain,
          f"{r.returncode} {r.stderr!r} {world.log[mark:]}")
    world.link_options = None
    world.sku_extra = {"Skus": 1}
    mark = len(world.log)
    r = probe("--lang", "en-US")
    check("check: malformed SKU list returns upstream error 4 without another request",
          r.returncode == 4 and "missing or invalid Skus" in r.stderr and request_kinds(mark) == chain[:-1],
          f"{r.returncode} {r.stderr!r} {world.log[mark:]}")
    world.sku_extra = {}
    check("check: all requests used winiso/1 and no private identifiers; no ISO state or jobs created",
          not world.violations and not isos.exists() and not jobs_log.exists() and not list(probe_tmp.iterdir()),
          "; ".join(sorted(set(world.violations))))

    # --- Microsoft refuses (Sentinel in the link answer): the chain stops there, nothing is retried ---
    world.refuse = "sentinel"
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "en-US")
    check("Sentinel refusal: status 4, the give-a-path message, nothing tried again, nothing downloaded",
          r.returncode == 4 and "Microsoft refused the automated download" in r.stderr
          and "give its path instead" in r.stderr and "Nothing is tried again" in r.stderr
          and world.count("link", mark) == 1 and world.count("iso", mark) == 0 and not (isos / "windows.iso").exists(),
          f"{r.returncode} {r.stderr!r} {world.log[mark:]}")
    check("Sentinel refusal: the chain was page, tags, mdt, ovdf, sku, link, once each",
          [world.count(k, mark) for k in ("page", "tags", "mdt", "ovdf", "sku", "link")] == [1, 1, 1, 1, 1, 1],
          repr(world.log[mark:]))
    world.refuse = "sentinel-nonjson"
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "en-US")
    check("Sentinel refusal (non-JSON text fallback): status 4, the give-a-path message, nothing downloaded",
          r.returncode == 4 and "Microsoft refused the automated download" in r.stderr
          and world.count("link", mark) == 1 and world.count("iso", mark) == 0,
          f"{r.returncode} {r.stderr!r}")
    world.refuse = "api-error"
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "en-US")
    check("non-Sentinel JSON Errors: status 4 with API error (not Sentinel refusal), nothing downloaded",
          r.returncode == 4 and "Temporary catalog error." in r.stderr
          and "Microsoft refused the automated download" not in r.stderr
          and world.count("iso", mark) == 0,
          f"{r.returncode} {r.stderr!r}")
    world.refuse = "blocked"
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "en-US")
    check("non-success answer (HTTP 403): status 4 at once, no link request, no download",
          r.returncode == 4 and "answered without success" in r.stderr and world.count("link", mark) == 0
          and world.count("iso", mark) == 0, f"{r.returncode} {r.stderr!r}")
    world.refuse = None

    # --- no SHA-256 for the language on the page: nothing after the page ---
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "th-TH")
    check("no SHA-256 on Microsoft's page for the language: status 4, nothing requested after the page",
          r.returncode == 4 and "lists no single SHA-256" in r.stderr
          and [world.count(k, mark) for k in ("tags", "mdt", "ovdf", "sku", "link", "iso")] == [0, 0, 0, 0, 0, 0],
          f"{r.returncode} {r.stderr!r}")

    # --- a good download: one chain, verified ---
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "en-US")
    final = final_of("11", "en-US")
    record = json.loads((isos / "windows-11-en-US.iso.json").read_text()) if (isos / "windows-11-en-US.iso.json").exists() else {}
    sku_asked = [e["sku"] for e in world.log[mark:] if e["kind"] == "link"]
    check("good download: status 0, the ISO kept under its stable name, its path printed last",
          r.returncode == 0 and r.stdout.splitlines()[-1:] == [str(final)]
          and final.read_bytes() == world.isos[name_of("English (United States)")], f"{r.returncode} {r.stdout!r} {r.stderr!r}")
    check("good download: one chain (page, tags, mdt, ovdf, sku, link, iso), each once, no stray cookie jar",
          [world.count(k, mark) for k in ("page", "tags", "mdt", "ovdf", "sku", "link", "iso")] == [1, 1, 1, 1, 1, 1, 1]
          and not list((isos / ".partial/dl").glob(".session.*")), repr(world.log[mark:]))
    check("good download: the link was asked for the SKU of English (United States) only",
          sku_asked == [world.sku_of["English (United States)"]], repr(sku_asked))
    check("good download: the record holds Microsoft's published SHA-256 and the size",
          record.get("sha256") == world.table["English"] and record.get("size") == final.stat().st_size
          and record.get("published_row") == "English 64-bit", repr(record))
    r = winiso("path", "windows")
    check("path windows after the fetch: that ISO", r.returncode == 0 and r.stdout.splitlines() == [str(final.resolve())],
          f"{r.returncode} {r.stdout!r}")
    r = winiso("path", "windows", "--lang", "en_US.UTF-8")
    check("path windows --lang: that ISO", r.returncode == 0 and r.stdout.splitlines() == [str(final.resolve())],
          f"{r.returncode} {r.stdout!r}")

    # --- the same ISO again: nothing requested ---
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "en-US")
    check("fetch of an ISO already verified against the published SHA-256: the page read once, nothing else requested",
          r.returncode == 0 and "that ISO already" in r.stdout
          and [world.count(k, mark) for k in ("page", "tags", "mdt", "ovdf", "sku", "link", "iso")] == [1, 0, 0, 0, 0, 0, 0],
          f"{r.returncode} {r.stdout!r} {world.log[mark:]}")

    # --- checksum mismatch: refused and deleted ---
    r = winiso("fetch", "windows", "--lang", "de-DE")
    de = final_of("11", "de-DE")
    check("checksum mismatch: status 5, the download deleted, nothing kept",
          r.returncode == 5 and "not the" in r.stderr and not de.exists()
          and not any(p.name.startswith("windows-11-de-DE") for p in (isos / ".partial").iterdir())
          and not any(p.name.startswith("windows-11-de-DE") for p in (isos / ".partial/dl").iterdir()),
          f"{r.returncode} {r.stderr!r}")
    r = winiso("path", "windows")
    check("checksum mismatch: path windows still gives the last verified ISO",
          r.returncode == 0 and r.stdout.splitlines() == [str(final.resolve())], f"{r.returncode} {r.stdout!r}")

    # --- a transfer that stops: status 4 at once (no retry in the run), the partial kept; the next fetch continues ---
    rc.write_text('language="zh_CN.UTF-8"\n', encoding="utf-8")
    zh = name_of("Chinese Simplified")
    world.drop_once[zh] = MiB
    mark = len(world.log)
    r = winiso("fetch", "windows")
    zh_final = final_of("11", "zh-CN")
    part = isos / ".partial/dl" / "windows-11-zh-CN.iso.part"
    check("a transfer that stops: status 4, the partial kept in .partial/dl, one ISO request only (no retry in the same run)",
          r.returncode == 4 and "the download stopped" in r.stderr and world.count("iso", mark) == 1
          and part.exists() and part.stat().st_size == MiB and not zh_final.exists(),
          f"{r.returncode} {r.stderr!r} {world.log[mark:]}")
    mark = len(world.log)
    r = winiso("fetch", "windows")
    isos_asked = [e["range"] for e in world.log[mark:] if e["kind"] == "iso"]
    check("resume across fetches: rc.conf's language zh_CN.UTF-8, the next fetch continues at the partial's end and verifies",
          r.returncode == 0 and isos_asked == [f"bytes={MiB}-"] and zh_final.read_bytes() == world.isos[zh]
          and [world.count(k, mark) for k in ("page", "tags", "mdt", "ovdf", "sku", "link")] == [1, 1, 1, 1, 1, 1],
          f"{r.returncode} {isos_asked} {r.stderr!r}")
    rc.write_text("", encoding="utf-8")

    # --- a partial file of an earlier fetch continues ---
    ja = name_of("Japanese")
    part = isos / ".partial/dl/windows-11-ja-JP.iso.part"
    part.write_bytes(world.isos[ja][:MiB + MiB // 2])
    (isos / ".partial/windows-11-ja-JP.iso.part.json").write_text(
        json.dumps({"sha256": world.table["Japanese"], "source": ja}) + "\n")
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "ja-JP")
    got = [e["range"] for e in world.log[mark:] if e["kind"] == "iso"]
    check("resume of an earlier partial file: continues from its end and verifies",
          r.returncode == 0 and got == [f"bytes={MiB + MiB // 2}-"]
          and final_of("11", "ja-JP").read_bytes() == world.isos[ja], f"{r.returncode} {got} {r.stderr!r}")

    # --- a partial file of another build is not continued ---
    ko = name_of("Korean")
    (isos / ".partial/dl/windows-11-ko-KR.iso.part").write_bytes(b"x" * MiB)
    (isos / ".partial/windows-11-ko-KR.iso.part.json").write_text(
        json.dumps({"sha256": "0" * 64, "source": ko}) + "\n")
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "ko-KR")
    got = [e["range"] for e in world.log[mark:] if e["kind"] == "iso"]
    check("a partial file of another build is not continued: from the start, verified",
          r.returncode == 0 and got == [""] and final_of("11", "ko-KR").read_bytes() == world.isos[ko],
          f"{r.returncode} {got} {r.stderr!r}")

    # --- a complete partial file (HTTP 416 to its range) is verified and kept ---
    pl = name_of("Polish")
    pl_part = isos / ".partial/dl/windows-11-pl-PL.iso.part"
    pl_part.write_bytes(world.isos[pl])
    pl_ino = os.lstat(pl_part).st_ino
    (isos / ".partial/windows-11-pl-PL.iso.part.json").write_text(
        json.dumps({"sha256": world.table["Polish"], "source": pl}) + "\n")
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "pl-PL")
    got = [e["range"] for e in world.log[mark:] if e["kind"] == "iso"]
    pl_final = final_of("11", "pl-PL")
    pl_st = os.lstat(pl_final) if pl_final.exists() else None
    check("a complete partial file (HTTP 416 to its range) is verified and kept via fd hard-link (same inode, nlink=1, dl/ cleared)",
          r.returncode == 0 and got == [f"bytes={len(world.isos[pl])}-"]
          and pl_final.read_bytes() == world.isos[pl]
          and pl_st is not None and pl_st.st_ino == pl_ino and pl_st.st_nlink == 1 and not pl_part.exists(),
          f"{r.returncode} {got} ino={pl_st.st_ino if pl_st else None}/{pl_ino} {r.stderr!r}")
    r = winiso("path", "windows")
    check("path windows gives the ISO the last fetch verified",
          r.returncode == 0 and r.stdout.splitlines() == [str(pl_final.resolve())], f"{r.stdout!r}")

    # --- directory layout and symlink / hardlink / fd-link safety ---
    check("partial dir layout: .partial is 0711, .partial/dl is 0700, .lock lives in .partial (0600)",
          stat.S_IMODE(os.lstat(isos / ".partial").st_mode) == 0o711
          and stat.S_IMODE(os.lstat(isos / ".partial/dl").st_mode) == 0o700
          and stat.S_IMODE(os.lstat(isos / ".partial/.lock").st_mode) == 0o600
          and not (isos / ".lock").exists(),
          f"partial={oct(stat.S_IMODE(os.lstat(isos / '.partial').st_mode))} "
          f"dl={oct(stat.S_IMODE(os.lstat(isos / '.partial/dl').st_mode))}")

    # Planted symlink at .partial/dl/<name>.part must be refused without touching target.
    fr = name_of("French")
    victim = tmp / "victim.txt"
    victim.write_bytes(world.isos[fr])
    fr_part = isos / ".partial/dl/windows-11-fr-FR.iso.part"
    os.symlink(victim, fr_part)
    (isos / ".partial/windows-11-fr-FR.iso.part.json").write_text(
        json.dumps({"sha256": world.table["French"], "source": fr}) + "\n")
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "fr-FR")
    check("planted symlink at .partial/dl/<name>.part is refused; target untouched, no ISO downloaded",
          r.returncode == 1 and "refused" in r.stderr and not final_of("11", "fr-FR").exists()
          and victim.read_bytes() == world.isos[fr] and world.count("iso", mark) == 0,
          f"{r.returncode} {r.stderr!r}")
    fr_part.unlink(missing_ok=True)
    (isos / ".partial/windows-11-fr-FR.iso.part.json").unlink(missing_ok=True)

    # Planted hardlink (st_nlink > 1) at .partial/dl/<name>.part must be refused.
    it = name_of("Italian")
    it_other = tmp / "it-hardlink.bin"
    it_other.write_bytes(world.isos[it])
    it_part = isos / ".partial/dl/windows-11-it-IT.iso.part"
    os.link(it_other, it_part)
    (isos / ".partial/windows-11-it-IT.iso.part.json").write_text(
        json.dumps({"sha256": world.table["Italian"], "source": it}) + "\n")
    r = winiso("fetch", "windows", "--lang", "it-IT")
    check("planted hardlink (nlink=2) at .partial/dl/<name>.part is refused",
          r.returncode == 1 and "refused" in r.stderr and not final_of("11", "it-IT").exists(),
          f"{r.returncode} {r.stderr!r}")
    it_part.unlink(missing_ok=True)
    (isos / ".partial/windows-11-it-IT.iso.part.json").unlink(missing_ok=True)

    # Direct unit checks of open_partial_ro and link_verified_fd: symlink refused with O_NOFOLLOW,
    # hardlink refused, and fd-link installs the hashed inode even if dl/<name>.part is swapped after open.
    loader = importlib.machinery.SourceFileLoader("locachy_winiso_mod", str(SOURCE / "link/locachy-winiso"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    mod.ISO_DIR = isos
    dl_dir = isos / ".partial/dl"
    sym_p = dl_dir / "sym-check.part"
    os.symlink(victim, sym_p)
    try:
        mod.open_partial_ro(sym_p)
        sym_refused = False
    except mod.Failure as e:
        sym_refused = "symbolic link" in str(e)
    finally:
        sym_p.unlink(missing_ok=True)
    check("open_partial_ro refuses a symlink with O_NOFOLLOW (does not follow)", sym_refused)

    # 1. Normal fd-link: final gets the exact hashed inode, nlink == 1, mode 0644, dl/ entry removed.
    good_p = dl_dir / "fdlink-ok.part"
    good_p.write_bytes(b"verified-bytes-0123456789")
    good_ino = os.lstat(good_p).st_ino
    fd, _st = mod.open_partial_ro(good_p)
    try:
        mod.seal_partial(fd)
        got_h = mod.sha256_fd(fd)
        good_final = isos / "fdlink-ok-final.iso"
        mod.link_verified_fd(fd, good_p, good_final)
    finally:
        os.close(fd)
    g_st = os.lstat(good_final)
    check("link_verified_fd installs the exact opened/hashed inode (same ino, nlink=1, 0644, dl/ entry removed)",
          got_h == hashlib.sha256(b"verified-bytes-0123456789").hexdigest()
          and good_final.read_bytes() == b"verified-bytes-0123456789"
          and g_st.st_ino == good_ino and g_st.st_nlink == 1 and not good_p.exists()
          and stat.S_IMODE(g_st.st_mode) == 0o644,
          f"ino={g_st.st_ino}/{good_ino} nlink={g_st.st_nlink} mode={oct(stat.S_IMODE(g_st.st_mode))}")
    good_final.unlink(missing_ok=True)

    # 2. Unlink-and-swap after open: Linux refuses linkat on an unlinked fd (ENOENT),
    # so the swapped replacement is never installed.
    swap_p = dl_dir / "swap-check.part"
    swap_p.write_bytes(b"verified-bytes-0123456789")
    fd, _st = mod.open_partial_ro(swap_p)
    swapped_final = isos / "swap-check-final.iso"
    try:
        swap_p.unlink()
        swap_p.write_bytes(b"attacker-swapped-bytes")
        try:
            mod.link_verified_fd(fd, swap_p, swapped_final)
            swap_refused = False
        except mod.Failure as e:
            swap_refused = "unlinked before installation" in str(e)
    finally:
        os.close(fd)
        swap_p.unlink(missing_ok=True)
    check("link_verified_fd refuses if dl/<name>.part was unlinked/swapped after open (attacker file never installed)",
          swap_refused and not swapped_final.exists())

    # 3. Rename-aside-and-swap after open: the hashed inode still has a link in dl/,
    # so nlink == 2 after unlinking part -> refused, nothing published.
    ren_p = dl_dir / "ren-check.part"
    ren_aside = dl_dir / "ren-aside.part"
    ren_p.write_bytes(b"verified-bytes-0123456789")
    fd, _st = mod.open_partial_ro(ren_p)
    ren_final = isos / "ren-check-final.iso"
    try:
        ren_p.rename(ren_aside)
        ren_p.write_bytes(b"attacker-swapped-bytes")
        try:
            mod.link_verified_fd(fd, ren_p, ren_final)
            ren_refused = False
        except mod.Failure as e:
            ren_refused = "extra link" in str(e)
    finally:
        os.close(fd)
        ren_p.unlink(missing_ok=True)
        ren_aside.unlink(missing_ok=True)
    check("link_verified_fd refuses if dl/<name>.part was renamed aside after open (extra link caught, nothing published)",
          ren_refused and not ren_final.exists())

    # --- download_link selection, JSON sentinel non-error field, stale cookie jars, ISO_NAME ---
    # 1. Valid JSON containing "sentinel" outside Errors must not trigger a false Sentinel refusal;
    #    and DownloadType == 1 wins even when its URL path does not contain "x64".
    world.isos["Win11_French_NoArch.iso"] = world.isos[name_of("French")]
    world.sku_extra = {"Note": "sentinel benign metadata"}
    world.link_options = lambda w, loc: [
        {"DownloadType": 2, "Uri": f"{w.base}/dbazure/{name_of(loc)}?t=other"},
        {"DownloadType": 1, "Uri": f"{w.base}/dbazure/Win11_French_NoArch.iso?note=sentinel"},
    ]
    r = winiso("fetch", "windows", "--lang", "fr-FR")
    check("DownloadType == 1 wins over other options; benign 'sentinel' outside Errors does not refuse",
          r.returncode == 0 and final_of("11", "fr-FR").read_bytes() == world.isos[name_of("French")],
          f"{r.returncode} {r.stderr!r}")
    world.sku_extra = {}
    # 2. Fallback when no DownloadType == 1 exists: match x64 case-insensitively in URL path, not query string.
    world.isos["Win11_Italian_X64.iso"] = world.isos[name_of("Italian")]
    world.link_options = lambda w, loc: [
        {"DownloadType": 2, "Uri": f"{w.base}/dbazure/Win11_Italian_arm64.iso?arch=x64"},
        {"DownloadType": 2, "Uri": f"{w.base}/dbazure/Win11_Italian_X64.iso?t=1"},
    ]
    r = winiso("fetch", "windows", "--lang", "it-IT")
    check("fallback without DownloadType == 1 matches x64 case-insensitively in URL path only (ignores query)",
          r.returncode == 0 and final_of("11", "it-IT").read_bytes() == world.isos[name_of("Italian")],
          f"{r.returncode} {r.stderr!r}")
    # 3. Ambiguous candidates (two DownloadType == 1 options, or two path-x64 fallbacks) are refused.
    world.link_options = lambda w, loc: [
        {"DownloadType": 1, "Uri": f"{w.base}/dbazure/Win11_a_x64.iso?t=1"},
        {"DownloadType": 1, "Uri": f"{w.base}/dbazure/Win11_b_x64.iso?t=2"},
    ]
    r = winiso("fetch", "windows", "--lang", "de-DE")
    check("multiple DownloadType == 1 candidates refused with status 4",
          r.returncode == 4 and "no single x64 link" in r.stderr, f"{r.returncode} {r.stderr!r}")
    world.link_options = lambda w, loc: [
        {"DownloadType": 2, "Uri": f"{w.base}/dbazure/Win11_only_query.iso?arch=x64"},
    ]
    r = winiso("fetch", "windows", "--lang", "de-DE")
    check("query-only x64 match without DownloadType == 1 is refused with status 4",
          r.returncode == 4 and "no single x64 link" in r.stderr, f"{r.returncode} {r.stderr!r}")
    world.link_options = None

    # 4. Stale cookie jars in .partial/dl/ are cleaned at fetch start; symlinks and other files are left alone.
    stale_jar = dl_dir / ".session.99999.deadbeef.cookies"
    stale_jar.write_text("# stale\n", encoding="utf-8")
    jar_sym_target = tmp / "jar-symlink-target.txt"
    jar_sym_target.write_text("keep", encoding="utf-8")
    stale_jar_sym = dl_dir / ".session.99999.symlink1.cookies"
    os.symlink(jar_sym_target, stale_jar_sym)
    unrelated = dl_dir / "keep-me.part"
    unrelated.write_bytes(b"keep")
    r = winiso("fetch", "windows", "--lang", "en-US")
    check("stale .session.*.cookies regular file removed at fetch start; symlink and other files untouched",
          r.returncode == 0 and not stale_jar.exists() and stale_jar_sym.is_symlink()
          and jar_sym_target.read_text(encoding="utf-8") == "keep" and unrelated.exists(),
          f"rc={r.returncode} stale={stale_jar.exists()} sym={stale_jar_sym.is_symlink()} keep={unrelated.exists()}")
    stale_jar_sym.unlink(missing_ok=True)
    unrelated.unlink(missing_ok=True)

    # 5. ISO_NAME accepts windows-11-... only, never windows-10-...
    check("ISO_NAME accepts windows-11-en-US.iso and rejects windows-10-en-US.iso",
          bool(mod.ISO_NAME.fullmatch("windows-11-en-US.iso"))
          and not bool(mod.ISO_NAME.fullmatch("windows-10-en-US.iso")))

    # --- online: the fixed system job, nothing requested here ---
    mark = len(world.log)
    r = winiso("fetch", "windows", "--lang", "de", TEST_JOBS_MODE="queue")
    jobs = jobs_log.read_text().splitlines() if jobs_log.exists() else []
    check("online, fetch is the system job winiso-fetch with the decided language; nothing requested here",
          r.returncode == 0 and jobs == [json.dumps(["winiso-fetch", ["--lang", "de-DE"]])]
          and not world.log[mark:], f"{r.returncode} {jobs} {r.stderr!r}")
    r = winiso("fetch", "windows", TEST_JOBS_MODE="queue-refuse")
    check("a refusal of the job service is a failure, never a fallback",
          r.returncode == 1 and "refused by the job service" in r.stderr and not world.log[mark:],
          f"{r.returncode} {r.stderr!r}")

    # --- static: the link step needs no quickemu; it credits its reference; curl drops to nobody ---
    text = (SOURCE / "link/locachy-winiso").read_text(encoding="utf-8")
    modules = json.loads((SOURCE / "link/modules.json").read_text(encoding="utf-8"))
    check("no quickget command or quickemu package is run or required (credits only)",
          not re.search(r"LOCACHY_QUICKGET|[\"']quickget[\"']|shutil\.which|quickemu\s+package|package\s+quickemu",
                        text, re.IGNORECASE)
          and "quickemu" not in json.dumps(modules), "a quickget/quickemu dependency remains")
    check("the link step credits Fido, Mido, dockur/windows and quickget as references",
          all(ref in text for ref in ("github.com/pbatard/Fido", "github.com/ElliotKillick/Mido",
                                     "github.com/dockur/windows, src/mido.sh, MIT", "github.com/quickemu-project/quickemu"))
          and "follow" in text, "credit or note missing")
    as_nobody_block = re.search(r"def as_nobody\(.*?\n(?=def )", text, re.S)
    fetch_text_block = re.search(r"def fetch_text\(.*?\n(?=def )", text, re.S)
    curl_dl_block = re.search(r"def curl_download\(.*?\n(?=def )", text, re.S)
    check("static: fetch_text passes --connect-timeout 20 to curl",
          fetch_text_block is not None and '"--connect-timeout", "20"' in fetch_text_block.group(0),
          "fetch_text missing --connect-timeout 20")
    check("static: as_nobody puts setpriv inside in_net when real_root() and curl_download uses as_nobody",
          as_nobody_block is not None and curl_dl_block is not None
          and "if real_root():" in as_nobody_block.group(0)
          and '"setpriv"' in as_nobody_block.group(0)
          and "--no-new-privs" in as_nobody_block.group(0)
          and "return in_net(cmd)" in as_nobody_block.group(0)
          and "as_nobody(cmd)" in curl_dl_block.group(0),
          "as_nobody/curl_download privilege-drop structure mismatch")
    # Also verify argument ordering with mocked real_root() + netns check.
    orig_rr, orig_in_net = mod.real_root, mod.in_net
    try:
        mod.real_root = lambda: True
        mod.in_net = lambda c: ["ip", "netns", "exec", "locachy-net", *c]
        built = mod.as_nobody(["curl", "--output", "/x"])
    finally:
        mod.real_root, mod.in_net = orig_rr, orig_in_net
    check("as_nobody ordering: ip netns exec locachy-net outermost, then setpriv nobody, then curl",
          built == ["ip", "netns", "exec", "locachy-net", "setpriv", "--reuid=65534", "--regid=65534",
                    "--clear-groups", "--no-new-privs", "--", "curl", "--output", "/x"], repr(built))

    # --- what left this host: one generic client, nothing of the project, the host or the person ---
    check("every outbound request: User-Agent winiso/1; no project name, host name or e-mail; one session per run; "
          "the link's referer is the download page",
          not world.violations, "; ".join(sorted(set(world.violations))))

    server.shutdown()
    if fails:
        print(f"FAILED ({fails} failures)")
        return 1
    print("ALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
