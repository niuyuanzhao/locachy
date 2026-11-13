#!/usr/bin/env python3
"""Content-addressed successful installation stages, never a disk-layout journal.

Old installations without this file repeat software stages. Failed work loses
its completion record before it starts. Only success records are reusable; the
install-id and partition/ESP checks remain the installer's responsibility.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time
import uuid

SCHEMA = 1
# Account creation and hardware enrollment are separate, once-only operations:
# a source update must not change passwords or consume another TPM keyslot.
DEPENDENTS = {
    "sources": (),  # other stages hash their relevant source inputs; a copy alone is not their input
    "packages": ("system", "configuration", "boot", "publish"),
    "system": ("configuration", "boot", "publish"),
    "configuration": ("boot", "publish"),
    "boot": ("publish",),
    "publish": (),
    "account": (),
    "tpm": (),
    "secure-boot-keys": (),
    "secure-boot-enroll": (),
}
PACKAGE_INPUTS = ("packages", "link/modules.json", "link/declare-packages", "link/declared-packages-baseline.json",
                  "link/install-pkg-tools", "link/locachy-build", "link/locachy-pkg", "link/locachy-repos",
                  "link/locachy_upstream.py", "link/locachy_transaction.py", "link/locachy_stack.py",
                  "link/local-package-gate", "pkgbuilds", "aur-patches", "make-root.sh")
SYSTEM_INPUTS = ("base", "init", "progs", "edit", "link", "po", "make.sh", "rc.conf")
SOURCE_INPUTS = SYSTEM_INPUTS + ("tools", "containers", "pkgbuilds", "aur-patches", "machines", "packages", "make-root.sh")
BUILD_ENV = ("WITH_IME", "WITH_FEATURES", "WITH_EXTRA_PACKAGES", "WITH_REPOS", "LOCACHY_INSTALLER")


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def files_digest(root, names):
    """Hash names and bytes, not timestamps, git commits or cache outputs."""
    root = Path(root)
    h = hashlib.sha256()
    for name in sorted(set(names)):
        source = root / name
        paths = sorted(source.rglob("*")) if source.is_dir() else [source]
        for path in paths:
            relative = path.relative_to(root)
            if "__pycache__" in relative.parts or path.suffix == ".pyc":
                continue
            if path.is_symlink():
                # Source trees can carry intentional relative links (for
                # example generated protocol aliases). Hash their target, but
                # never follow a link outside the checkout.
                target = (path.parent / os.readlink(path)).resolve()
                if not target.is_relative_to(root.resolve()):
                    raise ValueError(f"installation input symlink leaves source: {path}")
                h.update(str(relative).encode() + b"\0symlink\0" + os.readlink(path).encode() + b"\0")
                continue
            if not path.exists():
                h.update(str(relative).encode() + b"\0missing\0")
            elif path.is_file():
                h.update(str(relative).encode() + b"\0")
                with path.open("rb") as f:
                    for chunk in iter(lambda: f.read(128 * 1024), b""):
                        h.update(chunk)
                h.update(b"\0")
    return h.hexdigest()


def installed_digest(root):
    """The target ABI/toolchain, by package identity (not pacman's timestamps)."""
    entries = []
    for path in sorted((Path(root) / "var/lib/pacman/local").glob("*/desc")):
        fields, current = {}, None
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("%") and line.endswith("%"):
                current = line
                fields[current] = []
            elif line and current:
                fields[current].append(line)
        entries.append([fields.get("%NAME%", []), fields.get("%VERSION%", [])])
    return digest(entries)


def build_input(source, root, stage, level, cpu):
    names = {"sources": SOURCE_INPUTS, "packages": PACKAGE_INPUTS, "system": SYSTEM_INPUTS}[stage]
    result = {"files": files_digest(source, names), "level": level, "cpu": cpu,
              "selection": {name: os.environ.get(name, "") for name in BUILD_ENV}}
    if stage != "sources":
        result["installed"] = installed_digest(root)
    if stage == "packages":
        result["manifest"] = files_digest(root, ("etc/locachy/packages",))
    return digest(result)


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink() or path.parent.is_symlink():
        raise ValueError(f"installation state must not be a symlink: {path}")
    fd, name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(value, f, indent=1, sort_keys=True)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class Stages:
    def __init__(self, directory):
        self.path = Path(directory) / "stages.json"
        self.stages = {}
        if self.path.exists():
            if self.path.is_symlink() or not stat.S_ISREG(self.path.stat().st_mode):
                raise ValueError("installation stage record is not a regular file")
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if data.get("schema") != SCHEMA or not isinstance(data.get("stages"), dict):
                raise ValueError("unsupported installation stage record")
            self.stages = data["stages"]

    def needed(self, stage, fingerprint):
        entry = self.stages.get(stage, {})
        return entry.get("input") != fingerprint or entry.get("success") is not True

    def begin(self, stage):
        if stage not in DEPENDENTS:
            raise ValueError("unknown installation stage: " + stage)
        for name in (stage,) + DEPENDENTS[stage]:
            self.stages.pop(name, None)
        self.save()

    def complete(self, stage, fingerprint):
        if stage not in DEPENDENTS:
            raise ValueError("unknown installation stage: " + stage)
        self.stages[stage] = {"input": fingerprint, "success": True, "generation": uuid.uuid4().hex,
                              "completed": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        self.save()

    def run(self, stage, inputs, action, *, present=True):
        fingerprint = digest(inputs)
        if present and not self.needed(stage, fingerprint):
            print(f"Installer stage {stage}: unchanged successful inputs, reused", flush=True)
            return False
        print(f"Installer stage {stage}: new, changed or unfinished inputs, running", flush=True)
        self.begin(stage)
        action()
        self.complete(stage, fingerprint)
        return True

    def save(self):
        write_json(self.path, {"schema": SCHEMA, "stages": self.stages})


def rebuild_patches(source, root):
    """A changed installer overlay is not satisfied by an old same-version AUR package."""
    source, root = Path(source), Path(root)
    path = root / ".locachy-installer/aur-patches.json"
    if path.is_symlink():
        raise ValueError("AUR patch record must not be a symlink")
    before = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    if not isinstance(before, dict):
        raise ValueError("invalid AUR patch record")
    current = {p.stem: files_digest(source, (str(p.relative_to(source)),))
               for p in sorted((source / "aur-patches").glob("*.sed"))}
    changed = []
    for name in sorted(set(before) | set(current)):
        if before.get(name) == current.get(name):
            continue
        installed = subprocess.run(["chroot", str(root), "pacman", "-Q", name], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL).returncode == 0
        if installed:
            changed.append(name)
    if changed:
        print("Installer AUR patches changed: rebuilding " + " ".join(changed), flush=True)
        subprocess.run(["chroot", str(root), "/usr/local/bin/locachy-build", "-i", "-r"] + changed, check=True)
    write_json(path, current)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("fingerprint", "needed", "begin", "complete", "rebuild-patches"))
    parser.add_argument("directory")
    parser.add_argument("stage")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    if args.command == "fingerprint":
        if len(args.extra) != 3:
            parser.error("fingerprint SOURCE STAGE TARGET LEVEL CPU")
        print(build_input(args.directory, args.extra[0], args.stage, args.extra[1], args.extra[2]))
        return 0
    if args.command == "rebuild-patches":
        if len(args.extra) != 1 or args.stage != "packages":
            parser.error("rebuild-patches SOURCE packages TARGET")
        rebuild_patches(args.directory, args.extra[0])
        return 0
    state = Stages(args.directory)
    if args.command == "needed":
        if len(args.extra) != 1:
            parser.error("needed DIRECTORY STAGE FINGERPRINT")
        # A known reusable success has its own code: parse/read errors must
        # never look like a successful stage that can be skipped.
        return 0 if state.needed(args.stage, args.extra[0]) else 10
    if args.command == "begin":
        state.begin(args.stage)
    else:
        if len(args.extra) != 1:
            parser.error("complete DIRECTORY STAGE FINGERPRINT")
        state.complete(args.stage, args.extra[0])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit("installation state: " + str(error))
