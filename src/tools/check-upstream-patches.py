#!/usr/bin/env python3
"""Audit and build LOCachy's carried upstream patches against live Arch/AUR.

Two modes:
  --audit                  Check every pkgbuilds/* adapter and source patch,
                           installer/archiso-readonly-seed.patch, and every
                           aur-patches/*.sed against live upstream metadata
                           without compiling.
  --build-package TARGET   Inside an Arch Linux container, lock, build with
                           makepkg, install, and smoke-test one target:
                           libinput-pair, labwc-locachy, libmupdf, or libpaper.
"""

import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

sys.dont_write_bytecode = True

SOURCE = Path(__file__).resolve().parents[1]
ARCH_PKG_API = "https://archlinux.org/packages/{repo}/{arch}/{pkg}/json/"
AUR_RPC_API = "https://aur.archlinux.org/rpc/v5/info?arg[]={pkg}"
VKS_URL = "https://keys.openpgp.org/vks/v1/by-fingerprint/{fpr}"
ARCHISO_REPO = "https://gitlab.archlinux.org/archlinux/archiso.git"
USER_AGENT = "locachy-upstream-check/1"

BUILD_TARGETS = {
    "libinput-pair": [
        ("libinput", ["bash", str(SOURCE / "tools/test-libinput.sh"), "{out}"]),
        ("libinput-tools", ["bash", str(SOURCE / "tools/test-libinput-tools.sh"), "{out}"]),
    ],
    "labwc-locachy": [
        ("labwc-locachy", ["bash", str(SOURCE / "tools/test-labwc-locachy.sh"), "{out}"]),
    ],
    "libmupdf": [
        ("libmupdf", ["bash", str(SOURCE / "tools/test-libmupdf.sh"), "{out}", "--update-smoke"]),
    ],
    "libpaper": [
        ("libpaper", ["bash", str(SOURCE / "tools/test-libpaper.sh"), "{out}"]),
    ],
}


def load_upstream_module():
    spec = importlib.util.spec_from_file_location("locachy_upstream", SOURCE / "link/locachy_upstream.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fetch_json(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_bytes(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def run_cmd(cmd, cwd=None, env=None, input_bytes=None, timeout=600):
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=env,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )


def format_arch_version(meta):
    epoch = int(meta.get("epoch") or 0)
    ver = f"{meta['pkgver']}-{meta['pkgrel']}"
    return f"{epoch}:{ver}" if epoch else ver


def arch_package_meta(pkg, repo="extra", arch="x86_64"):
    return fetch_json(ARCH_PKG_API.format(
        repo=urllib.parse.quote(repo),
        arch=urllib.parse.quote(arch),
        pkg=urllib.parse.quote(pkg),
    ))


def source_candidate_pkgbuild(recipe_dir):
    """Source a locked candidate PKGBUILD in a subshell and return key metadata."""
    script = r'''
extract_function_variable() {
    local decl
    decl=$(declare -f "$1" | grep -E "^[[:space:]]+$2=\(") || return
    eval "${decl/#$2/$4}"
}
package_info() { :; }
startdir=$PWD
srcdir=$PWD/src
pkgdir=$PWD/pkg
source ./PKGBUILD
printf 'LOCACHY_META_PKGVER=%s\n' "${pkgver:-}"
printf 'LOCACHY_META_PKGREL=%s\n' "${pkgrel:-}"
printf 'LOCACHY_META_EPOCH=%s\n' "${epoch:-0}"
printf 'LOCACHY_META_SOURCE0=%s\n' "${source[0]:-}"
'''
    proc = run_cmd(["bash", "-euc", script], cwd=recipe_dir, timeout=60)
    out = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"PKGBUILD/adapter failed (exit {proc.returncode}):\n{out[-1500:]}")
    meta = {}
    for line in out.splitlines():
        if line.startswith("LOCACHY_META_") and "=" in line:
            k, v = line.split("=", 1)
            meta[k[len("LOCACHY_META_"):]] = v
    return meta


def parse_source_patches_from_adapter(adapter_path):
    """Return ordered list of source-tree patch filenames invoked in prepare()."""
    text = adapter_path.read_text(encoding="utf-8")
    patches = []
    for match in re.finditer(r'patch\b[^\n]*?-i\s+"?\$srcdir/([^"\s]+)"?', text):
        patches.append(match.group(1))
    return patches


def parse_patch_target_dir(adapter_path, pkgver, default_dir):
    text = adapter_path.read_text(encoding="utf-8")
    match = re.search(r'patch\b[^\n]*?-d\s+"?\$srcdir/([^"\s]+)"?', text)
    if not match:
        return default_dir
    return match.group(1).replace("$pkgver", pkgver).replace("${pkgver}", pkgver)


def fetch_upstream_source_tree(source0, dest_root, target_subdir, cache):
    """Fetch primary upstream source (git tag or release tarball) into dest_root."""
    # Strip optional makepkg "dest::url" prefix.
    raw = source0.split("::", 1)[1] if "::" in source0 else source0
    if raw in cache:
        cached = cache[raw]
        target = dest_root / target_subdir
        shutil.copytree(cached, target, symlinks=True)
        return target

    if raw.startswith("git+"):
        url_body = raw[4:]
        frag = ""
        if "#" in url_body:
            url_body, frag = url_body.split("#", 1)
        if "?" in url_body:
            url_body = url_body.split("?", 1)[0]
        ref = None
        if frag.startswith("tag="):
            ref = frag[4:]
        elif frag.startswith("branch="):
            ref = frag[7:]
        elif frag.startswith("commit="):
            ref = frag[7:]
        clone_dir = dest_root / target_subdir
        cmd = ["git", "clone", "--depth", "1", "--quiet"]
        if ref and not frag.startswith("commit="):
            cmd += ["--branch", ref]
        cmd += [url_body, str(clone_dir)]
        proc = run_cmd(cmd, timeout=180)
        if proc.returncode != 0:
            raise RuntimeError(f"git clone {url_body} ({ref}) failed:\n{proc.stdout.decode('utf-8', 'replace')[-1000:]}")
        if frag.startswith("commit="):
            co = run_cmd(["git", "-C", str(clone_dir), "checkout", "--quiet", ref], timeout=60)
            if co.returncode != 0:
                raise RuntimeError(f"git checkout {ref} failed")
        cache_copy = dest_root.parent / f"cache-{len(cache)}"
        shutil.copytree(clone_dir, cache_copy, symlinks=True)
        cache[raw] = cache_copy
        return clone_dir

    if raw.startswith(("https://", "http://")):
        archive = dest_root / Path(urllib.parse.urlparse(raw).path).name
        archive.write_bytes(fetch_bytes(raw, timeout=120))
        proc = run_cmd(["tar", "-xf", str(archive), "-C", str(dest_root)], timeout=120)
        if proc.returncode != 0:
            raise RuntimeError(f"tar extract {archive.name} failed:\n{proc.stdout.decode('utf-8', 'replace')[-1000:]}")
        target = dest_root / target_subdir
        if not target.is_dir():
            raise RuntimeError(f"expected extracted directory {target_subdir} not found in {dest_root}")
        cache_copy = dest_root.parent / f"cache-{len(cache)}"
        shutil.copytree(target, cache_copy, symlinks=True)
        cache[raw] = cache_copy
        return target

    raise RuntimeError(f"unsupported upstream source[0] format: {source0}")


def audit_pkgbuild(u, name, baseline_dir, work_root, src_cache):
    cfg = json.loads((baseline_dir / "upstream.json").read_text(encoding="utf-8"))
    upstream_pkg = cfg["package"]
    meta = arch_package_meta(upstream_pkg)
    version = format_arch_version(meta)
    updated = (meta.get("last_update") or "")[:10]

    state_dir = work_root / f"state-{name}"
    lock_path = u.prepare_candidate(name, baseline_dir, "extra", version, state_dir, stable_version=version)
    u.validate_candidate(lock_path, name, baseline_dir, state_dir)
    recipe_dir = lock_path.parent / "recipe"
    sourced = source_candidate_pkgbuild(recipe_dir)

    adapter_path = baseline_dir / cfg["adapter"]
    source_patches = parse_source_patches_from_adapter(adapter_path)
    notes = ["adapter/PKGBUILD OK"]

    if source_patches:
        src_root = work_root / f"src-{name}"
        src_root.mkdir(parents=True, exist_ok=True)
        target_subdir = parse_patch_target_dir(adapter_path, sourced.get("PKGVER", ""), cfg["source_directory"])
        tree = fetch_upstream_source_tree(sourced.get("SOURCE0", ""), src_root, target_subdir, src_cache)
        for pf in source_patches:
            proc = run_cmd(
                ["patch", "--batch", "--forward", "--fuzz=0", "-d", str(tree), "-Np1", "-i", str(baseline_dir / pf)],
                timeout=60,
            )
            if proc.returncode != 0:
                out = proc.stdout.decode("utf-8", errors="replace")
                raise RuntimeError(f"source patch {pf} failed:\n{out[-1200:]}")
        notes.append(f"{len(source_patches)} source patch(es) OK")

    return {
        "kind": "pkgbuild",
        "target": f"pkgbuilds/{name}",
        "upstream": f"extra/{upstream_pkg} {version}",
        "updated": updated,
        "status": "PASS",
        "detail": ", ".join(notes),
    }


def audit_archiso(work_root):
    patch_file = SOURCE / "installer/archiso-readonly-seed.patch"
    meta = arch_package_meta("archiso", repo="extra", arch="any")
    version = format_arch_version(meta)
    pkgver = meta["pkgver"]
    updated = (meta.get("last_update") or "")[:10]

    clone_dir = work_root / "archiso-src"
    proc = run_cmd(["git", "clone", "--depth", "1", "--quiet", "--branch", f"v{pkgver}", ARCHISO_REPO, str(clone_dir)], timeout=120)
    if proc.returncode != 0:
        shutil.rmtree(clone_dir, ignore_errors=True)
        proc = run_cmd(["git", "clone", "--depth", "1", "--quiet", ARCHISO_REPO, str(clone_dir)], timeout=120)
        if proc.returncode != 0:
            raise RuntimeError(f"git clone archiso failed:\n{proc.stdout.decode('utf-8', 'replace')[-1000:]}")

    mkarchiso = clone_dir / "archiso/mkarchiso"
    stage = work_root / "archiso-stage"
    stage.mkdir(parents=True, exist_ok=True)
    shutil.copy2(mkarchiso, stage / "mkarchiso")
    res = run_cmd(["patch", "--batch", "--fuzz=0", "-p1", "-d", str(stage)], input_bytes=patch_file.read_bytes(), timeout=30)
    if res.returncode != 0:
        raise RuntimeError(f"archiso-readonly-seed.patch failed:\n{res.stdout.decode('utf-8', 'replace')[-1200:]}")

    return {
        "kind": "installer",
        "target": "installer/archiso-readonly-seed.patch",
        "upstream": f"extra/archiso {version}",
        "updated": updated,
        "status": "PASS",
        "detail": "mkarchiso patch OK (fuzz=0)",
    }


def audit_aur_sed(sed_path, work_root, aur_cache):
    pkg = sed_path.stem
    info = fetch_json(AUR_RPC_API.format(pkg=urllib.parse.quote(pkg)))
    results = info.get("results") or []
    if not results:
        raise RuntimeError(f"AUR package {pkg} not found in RPC v5")
    entry = results[0]
    pkgbase = entry["PackageBase"]
    version = entry["Version"]
    updated = datetime.fromtimestamp(int(entry["LastModified"]), timezone.utc).strftime("%Y-%m-%d")

    if pkgbase not in aur_cache:
        clone_dir = work_root / f"aur-{pkgbase}"
        proc = run_cmd(["git", "clone", "--depth", "1", "--quiet", f"https://aur.archlinux.org/{pkgbase}.git", str(clone_dir)], timeout=120)
        if proc.returncode != 0:
            raise RuntimeError(f"git clone AUR {pkgbase} failed:\n{proc.stdout.decode('utf-8', 'replace')[-1000:]}")
        aur_cache[pkgbase] = clone_dir

    orig = (aur_cache[pkgbase] / "PKGBUILD").read_bytes()
    patched_path = work_root / f"{pkg}.PKGBUILD"
    proc = run_cmd(["sed", "-f", str(sed_path), str(aur_cache[pkgbase] / "PKGBUILD")], timeout=30)
    if proc.returncode != 0:
        raise RuntimeError(f"sed -f {sed_path.name} failed:\n{proc.stdout.decode('utf-8', 'replace')[-1000:]}")
    if proc.stdout == orig:
        raise RuntimeError(
            f"{sed_path.name} produced no change on AUR {pkgbase} {version} "
            "(upstream PKGBUILD drifted or already incorporated this fix)"
        )
    patched_path.write_bytes(proc.stdout)
    syn = run_cmd(["bash", "-n", str(patched_path)], timeout=30)
    if syn.returncode != 0:
        raise RuntimeError(f"patched PKGBUILD syntax error:\n{syn.stdout.decode('utf-8', 'replace')[-1000:]}")

    return {
        "kind": "aur-sed",
        "target": f"aur-patches/{sed_path.name}",
        "upstream": f"aur/{pkgbase} {version}",
        "updated": updated,
        "status": "PASS",
        "detail": "sed applied with diff + bash -n OK",
    }


def render_markdown(title, rows, errors):
    lines = [f"## {title}", "", "| Target | Upstream | Updated | Status | Detail |", "| --- | --- | --- | --- | --- |"]
    for row in rows:
        icon = "PASS" if row["status"] == "PASS" else "**FAIL**"
        detail = row["detail"].replace("\n", " ").replace("|", "\\|")
        lines.append(f"| `{row['target']}` | `{row['upstream']}` | {row['updated']} | {icon} | {detail} |")
    if errors:
        lines.append("")
        lines.append("### Failure details")
        for target, err in errors:
            lines.append("")
            lines.append(f"#### `{target}`")
            lines.append("```text")
            lines.append(err.strip())
            lines.append("```")
    lines.append("")
    return "\n".join(lines)


def write_summary(md, paths):
    sys.stdout.write(md)
    for p in paths:
        if not p:
            continue
        path = Path(p)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as fh:
            fh.write(md)


def cmd_audit(args):
    u = load_upstream_module()
    rows = []
    errors = []
    with tempfile.TemporaryDirectory(prefix="locachy-upstream-audit-") as tmp:
        work_root = Path(tmp)
        src_cache = {}
        aur_cache = {}

        pkgbuilds_dir = SOURCE / "pkgbuilds"
        for entry in sorted(pkgbuilds_dir.iterdir()):
            if not (entry.is_dir() and (entry / "upstream.json").is_file()):
                continue
            try:
                rows.append(audit_pkgbuild(u, entry.name, entry, work_root, src_cache))
            except Exception as exc:
                msg = str(exc)
                rows.append({
                    "kind": "pkgbuild",
                    "target": f"pkgbuilds/{entry.name}",
                    "upstream": "extra",
                    "updated": "-",
                    "status": "FAIL",
                    "detail": msg.splitlines()[0][:140],
                })
                errors.append((f"pkgbuilds/{entry.name}", msg))

        try:
            rows.append(audit_archiso(work_root))
        except Exception as exc:
            msg = str(exc)
            rows.append({
                "kind": "installer",
                "target": "installer/archiso-readonly-seed.patch",
                "upstream": "extra/archiso",
                "updated": "-",
                "status": "FAIL",
                "detail": msg.splitlines()[0][:140],
            })
            errors.append(("installer/archiso-readonly-seed.patch", msg))

        aur_dir = SOURCE / "aur-patches"
        for sed_path in sorted(aur_dir.glob("*.sed")):
            try:
                rows.append(audit_aur_sed(sed_path, work_root, aur_cache))
            except Exception as exc:
                msg = str(exc)
                rows.append({
                    "kind": "aur-sed",
                    "target": f"aur-patches/{sed_path.name}",
                    "upstream": f"aur/{sed_path.stem}",
                    "updated": "-",
                    "status": "FAIL",
                    "detail": msg.splitlines()[0][:140],
                })
                errors.append((f"aur-patches/{sed_path.name}", msg))

    md = render_markdown("Daily Upstream Patch Audit", rows, errors)
    write_summary(md, [args.summary, args.report])
    return 1 if errors else 0


def pacman_extra_version(pkg):
    proc = run_cmd(["pacman", "-Si", f"extra/{pkg}"], timeout=60)
    out = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"pacman -Si extra/{pkg} failed:\n{out}")
    for line in out.splitlines():
        if line.startswith("Version"):
            return line.split(":", 1)[1].strip()
    raise RuntimeError(f"could not parse Version from pacman -Si extra/{pkg}")


def parse_srcinfo_fields(text):
    fields = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or " = " not in line:
            continue
        k, v = line.split(" = ", 1)
        fields.setdefault(k, []).append(v)
    return fields


def import_pgp_keys(builder_user, keys):
    if not keys:
        return
    for fpr in sorted(set(keys)):
        imported = False
        try:
            key_bytes = fetch_bytes(VKS_URL.format(fpr=urllib.parse.quote(fpr)), timeout=30)
            proc = run_cmd(["runuser", "-u", builder_user, "--", "gpg", "--batch", "--import"], input_bytes=key_bytes, timeout=60)
            if proc.returncode == 0:
                imported = True
        except Exception:
            pass
        if not imported:
            for server in ("hkps://keyserver.ubuntu.com", "hkps://keys.openpgp.org", "hkps://pgp.surf.nl"):
                proc = run_cmd(
                    ["runuser", "-u", builder_user, "--", "gpg", "--batch", "--keyserver", server, "--recv-keys", fpr],
                    timeout=90,
                )
                if proc.returncode == 0:
                    imported = True
                    break
        if not imported:
            raise RuntimeError(f"failed to import upstream PGP key {fpr} into builder keyring")


def ensure_builder_user(name="builder"):
    proc = run_cmd(["id", "-u", name], timeout=15)
    if proc.returncode != 0:
        res = run_cmd(["useradd", "-m", "-s", "/bin/bash", name], timeout=30)
        if res.returncode != 0:
            raise RuntimeError(f"useradd {name} failed:\n{res.stdout.decode('utf-8', 'replace')}")


def build_and_smoke_one(u, name, smoke_cmd_template, state_root, out_root):
    baseline_dir = SOURCE / "pkgbuilds" / name
    cfg = json.loads((baseline_dir / "upstream.json").read_text(encoding="utf-8"))
    upstream_pkg = cfg["package"]
    version = pacman_extra_version(upstream_pkg)

    state_dir = state_root / name
    lock_path = u.prepare_candidate(name, baseline_dir, "extra", version, state_dir, stable_version=version)
    lock = u.validate_candidate(lock_path, name, baseline_dir, state_dir)
    recipe_src = lock_path.parent / "recipe"

    ensure_builder_user("builder")
    build_dir = Path("/build") / name
    pkgdest = Path("/build/pkgs") / name
    shutil.rmtree(build_dir, ignore_errors=True)
    shutil.rmtree(pkgdest, ignore_errors=True)
    pkgdest.mkdir(parents=True, exist_ok=True)
    shutil.copytree(recipe_src, build_dir, symlinks=True)
    run_cmd(["chown", "-R", "builder:builder", str(build_dir), str(pkgdest)], timeout=60)

    srcinfo_proc = run_cmd(
        ["runuser", "-u", "builder", "--", "makepkg", "--printsrcinfo"],
        cwd=build_dir,
        timeout=60,
    )
    srcinfo_text = srcinfo_proc.stdout.decode("utf-8", errors="replace")
    if srcinfo_proc.returncode != 0:
        raise RuntimeError(f"makepkg --printsrcinfo failed for {name}:\n{srcinfo_text[-1500:]}")
    fields = parse_srcinfo_fields(srcinfo_text)

    self_names = set(fields.get("pkgname", [])) | {
        p.split("=", 1)[0] for p in fields.get("provides", [])
    }
    raw_deps = []
    for key in ("depends", "makedepends", "checkdepends", "depends_x86_64", "makedepends_x86_64", "checkdepends_x86_64"):
        raw_deps.extend(fields.get(key, []))

    needed = []
    for dep in sorted(set(raw_deps)):
        bare = re.split(r"[<>=]", dep, maxsplit=1)[0]
        if bare in self_names:
            continue
        if run_cmd(["pacman", "-T", dep], timeout=30).returncode == 0:
            continue
        needed.append(dep)

    if needed:
        inst = run_cmd(["pacman", "-S", "--needed", "--noconfirm", "--asdeps", *needed], timeout=600)
        if inst.returncode != 0:
            raise RuntimeError(f"installing build deps for {name} failed:\n{inst.stdout.decode('utf-8', 'replace')[-1500:]}")

    import_pgp_keys("builder", fields.get("validpgpkeys", []))

    mkconf = Path("/tmp/ci-makepkg.conf")
    nproc = os.cpu_count() or 2
    mkconf.write_text(
        "source_makepkg_config /etc/makepkg.conf\n"
        f'MAKEFLAGS="-j{nproc}"\n'
        "OPTIONS+=(!debug)\n",
        encoding="utf-8",
    )

    print(f"=== building {name} ({lock['package_version']}) ===", flush=True)
    build_proc = subprocess.run(
        [
            "runuser", "-u", "builder", "--",
            "env", "LANG=C.UTF-8", f"PKGDEST={pkgdest}",
            "makepkg", "--config", str(mkconf), "--force", "--noconfirm",
        ],
        cwd=str(build_dir),
        timeout=2400,
    )
    if build_proc.returncode != 0:
        raise RuntimeError(f"makepkg failed for {name} (exit {build_proc.returncode})")

    pkgs = [
        p for p in sorted(pkgdest.glob("*.pkg.tar.*"))
        if "-debug-" not in p.name and not p.name.endswith(".sig")
    ]
    if not pkgs:
        raise RuntimeError(f"no package archive produced in {pkgdest}")

    print(f"=== installing {[p.name for p in pkgs]} ===", flush=True)
    up = run_cmd(["pacman", "-U", "--noconfirm", *[str(p) for p in pkgs]], timeout=300)
    if up.returncode != 0:
        raise RuntimeError(f"pacman -U failed for {name}:\n{up.stdout.decode('utf-8', 'replace')[-1500:]}")

    smoke_out = out_root / f"smoke-{name}"
    smoke_out.mkdir(parents=True, exist_ok=True)
    smoke_cmd = [arg.format(out=str(smoke_out)) for arg in smoke_cmd_template]
    print(f"=== smoke test: {' '.join(smoke_cmd)} ===", flush=True)
    smoke = run_cmd(smoke_cmd, timeout=300)
    smoke_text = smoke.stdout.decode("utf-8", errors="replace")
    sys.stdout.write(smoke_text)
    if smoke.returncode != 0:
        raise RuntimeError(f"smoke test failed for {name} (exit {smoke.returncode}):\n{smoke_text[-1500:]}")

    return {
        "kind": "build",
        "target": f"pkgbuilds/{name}",
        "upstream": f"extra/{upstream_pkg} {version}",
        "updated": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
        "status": "PASS",
        "detail": f"built {lock['package_version']} + smoke PASS",
    }


def cmd_build(args):
    if args.build_package not in BUILD_TARGETS:
        raise SystemExit(f"unknown target {args.build_package!r}; choose from {', '.join(BUILD_TARGETS)}")
    u = load_upstream_module()
    rows = []
    errors = []
    out_root = Path(args.out_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    state_root = Path("/build/state")
    state_root.mkdir(parents=True, exist_ok=True)

    for pkg_name, smoke_cmd in BUILD_TARGETS[args.build_package]:
        try:
            rows.append(build_and_smoke_one(u, pkg_name, smoke_cmd, state_root, out_root))
        except Exception as exc:
            msg = str(exc)
            rows.append({
                "kind": "build",
                "target": f"pkgbuilds/{pkg_name}",
                "upstream": "extra",
                "updated": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
                "status": "FAIL",
                "detail": msg.splitlines()[0][:140],
            })
            errors.append((f"pkgbuilds/{pkg_name}", msg))
            break  # libinput-tools depends on libinput succeeding first

    md = render_markdown(f"Arch Container Build: {args.build_package}", rows, errors)
    write_summary(md, [args.summary, args.report])
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description="Audit and build LOCachy carried upstream patches.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--audit", action="store_true", help="Audit all carried patches against live Arch/AUR (default).")
    mode.add_argument("--build-package", metavar="TARGET", help="Build and smoke-test one target inside Arch Linux.")
    parser.add_argument("--summary", help="Append Markdown summary to this file (e.g. $GITHUB_STEP_SUMMARY).")
    parser.add_argument("--report", help="Also write Markdown report to this file.")
    parser.add_argument("--out-dir", default="/tmp/locachy-ci-out", help="Output directory for build smoke artifacts.")
    args = parser.parse_args()
    if args.build_package:
        return cmd_build(args)
    return cmd_audit(args)


if __name__ == "__main__":
    raise SystemExit(main())
