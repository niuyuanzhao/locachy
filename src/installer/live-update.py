#!/usr/bin/env python3
"""Build the live interface from the same source snapshot as its backend.

The live entry calls this after fetching a complete checkout. Builds are staged
beside one another and published only after success; the old process is never
signalled. It switches itself with exec, passing its state through a sealed fd.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def revision(source):
    result = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                            capture_output=True, text=True, timeout=10)
    return result.stdout.strip() if result.returncode == 0 else ""


def input_hash(source):
    digest = hashlib.sha256()
    inputs = [source / name for name in ("base", "po", "installer/make-live.sh", "installer/live-update.py",
              "progs/installer.c", "progs/installer-session.inc", "progs/term.c", "progs/rescue.c",
              "progs/keytable.c", "progs/clipboard.c", "link/64-locachy-cjk-prefer.conf")]
    for item in inputs:
        for path in sorted(item.rglob("*")) if item.is_dir() else (item,):
            if not path.is_file() or "__pycache__" in path.parts or path.name.startswith("._"):
                continue
            digest.update(str(path.relative_to(source)).encode() + b"\0")
            digest.update(path.read_bytes())
            digest.update(b"\0")
    # A source-identical program must be relinked after the live ABI changes.
    packages = subprocess.run(["pacman", "-Q"], check=True, capture_output=True, text=True)
    digest.update(packages.stdout.encode())
    return digest.hexdigest()


def build(source, root=Path("/"), state=Path("/run/locachy-installer/live")):
    fingerprint = input_hash(source)
    stage = state / fingerprint
    metadata = stage / "live.json"
    if not metadata.is_file():
        state.mkdir(mode=0o700, parents=True, exist_ok=True)
        temporary = Path(tempfile.mkdtemp(prefix="build-", dir=state))
        try:
            print("Installer: building the live programs from " + (revision(source)[:12] or "the bundled source"),
                  file=sys.stderr, flush=True)
            subprocess.run(["sh", str(source / "installer/make-live.sh"), str(source), str(temporary)],
                           check=True, stdout=sys.stderr, stderr=sys.stderr, timeout=600)
            for name in ("installer", "term", "rescue"):
                if not (temporary / "usr/local/bin" / name).is_file():
                    raise RuntimeError("the live build did not produce " + name)
            data = {"format": 1, "input_hash": fingerprint, "revision": revision(source),
                    "source": str(source), "frontend": str(stage / "usr/local/bin/installer")}
            (temporary / "live.json").write_text(json.dumps(data) + "\n", encoding="utf-8")
            temporary.rename(stage)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    data = json.loads(metadata.read_text(encoding="utf-8"))
    if data.get("input_hash") != fingerprint or not Path(data["frontend"]).is_file():
        raise RuntimeError("the staged live programs are incomplete")
    # Compiled tools and catalogs are published through a same-directory rename.
    # Do not replace the clipboard daemon: its protocol is shared, and it may
    # hold the text a person is copying between this interface and its terminal.
    for directory in ("usr/local/bin", "usr/local/lib/locachy", "usr/local/share", "usr/lib/locale",
                      "etc/fonts/conf.d"):
        for path in sorted((stage / directory).rglob("*")):
            if not path.is_file() or path.name == "clipboard":
                continue
            target = root / path.relative_to(stage)
            target.parent.mkdir(parents=True, exist_ok=True)
            fd, name = tempfile.mkstemp(prefix="." + target.name + ".", dir=target.parent)
            try:
                with os.fdopen(fd, "wb") as output:
                    output.write(path.read_bytes())
                    output.flush()
                    os.fsync(output.fileno())
                os.chmod(name, path.stat().st_mode & 0o777)
                os.replace(name, target)
            finally:
                if os.path.exists(name):
                    os.unlink(name)
    # The input hash identifies the binary, while revision identifies the whole
    # repository. A backend-only update need not rebuild the live interface.
    return dict(data, revision=revision(source), source=str(source))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("ui", "refresh"))
    parser.add_argument("--update-repo")
    parser.add_argument("--update-branch")
    parser.add_argument("--apps-repo")
    parser.add_argument("--apps-branch")
    args = parser.parse_args()
    source = args.source.resolve()
    how = os.environ.get("LOCACHY_SOURCE", "")
    if args.mode == "refresh" and (how.startswith("fetched before:") or
                                  how.startswith("bundled:") and how != "bundled: updates are off"):
        raise RuntimeError("the source could not be refreshed: " + how)
    data = build(source)
    if args.mode == "refresh":
        print(json.dumps(data))
        return
    os.environ["LOCACHY_INSTALL_SNAPSHOT_ROOT"] = str(source.parent if source.name == "src" else source)
    os.environ["LOCACHY_INSTALL_SNAPSHOT_COMMIT"] = data["revision"]
    os.environ["LOCACHY_INSTALLER_INPUT_HASH"] = data["input_hash"]
    os.environ["LOCACHY_INSTALLER_REVISION"] = data["revision"]
    record = Path("/run/locachy-install/resume.json")
    # The new frontend starts in its normal state; installer.c can consume the
    # non-secret resume-info record and offer its stopped page. Never put the
    # record or a secret in argv.
    argv = [data["frontend"]]
    os.execv(argv[0], argv)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print("Installer refresh failed: " + str(error), file=sys.stderr)
        sys.exit(1)
