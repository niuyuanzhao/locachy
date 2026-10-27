#!/usr/bin/env python3
"""Pinned stable distro recipes plus a small, package-specific local overlay.

This module never executes PKGBUILD or edits the source checkout. Recipe code
is opaque data until makepkg runs it as the isolated build user. Candidate
creation is separate from promotion: a compiler success is not a passed gate.
"""
import hashlib
import json
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import urllib.parse
import urllib.request

SCHEMA = 1
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_TREE_BYTES = 32 * 1024 * 1024
NETWORK_TIMEOUT = 25
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9@._+\-]*")
COMMIT_RE = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
HASH_RE = re.compile(r"[0-9a-f]{64}")
VERSION_RE = re.compile(r"(?:(\d+):)?([A-Za-z0-9_.+~]+)-(\d+(?:\.\d+)*)")
ALGORITHMS = ("md5", "sha1", "sha224", "sha256", "sha384", "sha512", "b2")


class UpstreamError(RuntimeError):
    pass


DEPENDENCY_ARRAY = re.compile(r"^(?:depends|makedepends|checkdepends)=\(([^)]*)\)", re.M)


def recipe_dependencies(path):
    """The packages a recipe builds and runs against: the literal names in the
    depends, makedepends and checkdepends arrays of its PKGBUILD, or of the
    locked distro recipe an effective one sources, read, never run; and
    base-devel, which makepkg assumes."""
    names = {"base-devel"}
    for filename in ("PKGBUILD", ".locachy-upstream.PKGBUILD"):
        try:
            text = Path(path, filename).read_text(encoding="utf-8")
        except FileNotFoundError:
            continue
        for body in DEPENDENCY_ARRAY.findall(text):
            for line in body.splitlines():
                for word in line.split("#", 1)[0].split():
                    name = re.split(r"[<>=]", word.strip("'\""), maxsplit=1)[0]
                    if NAME_RE.fullmatch(name):
                        names.add(name)
    return names


def installed_packages(db_path="/var/lib/pacman/local"):
    """name -> (version, dependencies, provides) from the local pacman database."""
    packages = {}
    for desc in Path(db_path).glob("*/desc"):
        fields, key = {}, None
        for line in desc.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("%") and line.endswith("%"):
                key = line
                fields[key] = []
            elif line and key:
                fields[key].append(line)
        name = fields.get("%NAME%", [""])[0]
        if name:
            packages[name] = (fields.get("%VERSION%", [""])[0],
                              [re.split(r"[<>=:]", d, maxsplit=1)[0] for d in fields.get("%DEPENDS%", [])],
                              [re.split(r"[<>=]", p, maxsplit=1)[0] for p in fields.get("%PROVIDES%", [])])
    return packages


def build_fingerprint(path, local_names=(), db_path="/var/lib/pacman/local"):
    """What a local package's build depends on: the installed versions of its
    recipe's dependencies and all they depend on. Installing an unrelated
    package leaves it as it was; an update of a library or tool in that
    closure changes it. Local packages are never part of it: installing one
    must not invalidate its own build."""
    packages = installed_packages(db_path)
    provides = {}
    for name, (_, _, provided) in packages.items():
        for item in provided:
            provides.setdefault(item, []).append(name)

    def resolve(dependency):
        if dependency in packages:
            return dependency
        found = sorted(provides.get(dependency, []))
        return found[0] if found else None

    seen, stack = set(), [resolve(d) for d in sorted(recipe_dependencies(path))]
    while stack:
        name = stack.pop()
        if name is None or name in seen or name in local_names:
            continue
        seen.add(name)
        stack.extend(resolve(d) for d in packages[name][1])
    text = "".join(f"{n} {packages[n][0]}\n" for n in sorted(seen))
    return hashlib.sha256(text.encode()).hexdigest()


def _name(name):
    if not NAME_RE.fullmatch(name):
        raise UpstreamError("invalid local package name")


def _json_bytes(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _relative(value):
    p = PurePosixPath(value)
    if p.is_absolute() or not value or any(x in ("", ".", "..") for x in value.split("/")):
        raise UpstreamError(f"unsafe recipe path: {value}")
    return p


def _files(path):
    """Sorted regular files only; no following symlinks or special files."""
    path = Path(path)
    if path.is_symlink() or not path.is_dir():
        raise UpstreamError(f"not a recipe directory: {path}")
    total = 0
    result = {}
    for base, dirs, files in os.walk(path, followlinks=False):
        dirs.sort()
        for name in dirs:
            if Path(base, name).is_symlink():
                raise UpstreamError("recipe directory symlink is not allowed")
        for name in sorted(files):
            full = Path(base, name)
            s = full.lstat()
            if not stat.S_ISREG(s.st_mode):
                raise UpstreamError("recipe symlink or special file is not allowed")
            if s.st_size > MAX_FILE_BYTES:
                raise UpstreamError("recipe file exceeds the input budget")
            data = full.read_bytes()
            total += len(data) + len(full.relative_to(path).as_posix().encode()) + 128
            if total > MAX_TREE_BYTES:
                raise UpstreamError("recipe tree exceeds the input budget")
            result[full.relative_to(path).as_posix()] = data
    return dict(sorted(result.items()))


def tree_hash(path):
    h = hashlib.sha256()
    for name, data in _files(path).items():
        h.update(name.encode() + b"\0")
        h.update(data)
        h.update(b"\0")
    return h.hexdigest()


def local_input_hash(baseline_dir):
    return tree_hash(baseline_dir)


def read_srcinfo(data):
    """Read repeated .SRCINFO fields without shell evaluation."""
    try:
        text = data.decode("utf-8") if isinstance(data, bytes) else data
    except UnicodeError as e:
        raise UpstreamError("invalid .SRCINFO encoding") from e
    fields = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if " = " not in line:
            raise UpstreamError("invalid .SRCINFO line")
        key, value = line.split(" = ", 1)
        if not re.fullmatch(r"[A-Za-z0-9_]+", key):
            raise UpstreamError("invalid .SRCINFO key")
        fields.setdefault(key, []).append(value)
    return fields


def _selected_srcinfo(data, package):
    """The selected split output inherits base fields, not its siblings'."""
    text = data.decode("utf-8") if isinstance(data, bytes) else data
    sections = re.split(r"(?m)^\s*pkgname = ([^\n]+)\n?", text)
    fields = read_srcinfo(sections[0])
    matches = [body for name, body in zip(sections[1::2], sections[2::2]) if name == package]
    if len(matches) != 1:
        raise UpstreamError("selected stable package is missing or repeated")
    fields.update(read_srcinfo(matches[0]))
    fields["pkgname"] = [package]
    return fields


def _single(fields, key, default=None):
    values = fields.get(key, [default] if default is not None else [])
    if len(values) != 1 or values[0] is None:
        raise UpstreamError(f"expected one {key} in .SRCINFO")
    return values[0]


def _config(name, baseline_dir):
    _name(name)
    try:
        cfg = json.loads(Path(baseline_dir, "upstream.json").read_text())
    except (OSError, ValueError) as e:
        raise UpstreamError("upstream tracking configuration is unavailable") from e
    overlays = {
        "labwc-locachy": ("labwc", "labwc", ["labwc"],
                         ["clipboard-guard.patch", "locachy-clipboard.c", "locachy-clipboard.h"],
                         "labwc-clipboard-freeze-v1"),
        "libinput": ("libinput", "libinput", ["libinput", "libinput-tools"],
                     ["library.patch", "circular-scroll.patch"], "libinput-circular-scroll-v1"),
        "libinput-tools": ("libinput-tools", "libinput", ["libinput", "libinput-tools"],
                          ["cli.patch"], "libinput-tools-cli-v1"),
        "libmupdf": ("libmupdf", "mupdf", ["libmupdf", "mupdf", "mupdf-gl", "mupdf-tools", "python-mupdf"],
                     ["bundled-cmark-gfm.patch"], "libmupdf-markdown-v1"),
    }
    if cfg.get("schema") != SCHEMA or cfg.get("name") != name or name not in overlays:
        raise UpstreamError("unsupported upstream overlay")
    package, pkgbase, packages, inputs, gate = overlays[name]
    if cfg.get("package") != package or cfg.get("pkgbase", package) != pkgbase or \
            cfg.get("packages", [package]) != packages:
        raise UpstreamError("unsupported upstream package selection")
    if cfg.get("provider") != "arch-gitlab" or cfg.get("source_directory") != pkgbase:
        raise UpstreamError("unsupported source provider or adapter")
    expected_api = "https://gitlab.archlinux.org/api/v4/projects/archlinux%2Fpackaging%2Fpackages%2F" + pkgbase
    expected_repo = "https://gitlab.archlinux.org/archlinux/packaging/packages/" + pkgbase + ".git"
    if cfg.get("api") != expected_api or cfg.get("repository") != expected_repo:
        raise UpstreamError("unrecognized stable recipe authority")
    if not isinstance(cfg.get("patch_revision"), int) or cfg["patch_revision"] < 1:
        raise UpstreamError("invalid local patch revision")
    if cfg.get("adapter") != "adapter.sh" or cfg.get("inputs") != inputs:
        raise UpstreamError("unsupported package adapter inputs")
    if cfg.get("gate") != gate:
        raise UpstreamError("missing required package contract gate")
    return dict(cfg, pkgbase=pkgbase, packages=packages)


def stable_source_version(version):
    """An official Arch stable version, never inferred from a rebuild's pkgrel."""
    m = VERSION_RE.fullmatch(version)
    if not m:
        raise UpstreamError("unsupported stable source version")
    epoch, pkgver, pkgrel = m.groups()
    return {"epoch": epoch or "0", "pkgver": pkgver, "pkgrel": pkgrel,
            "tag": f"{pkgver}-{pkgrel}"}


def fetch_bytes(url, *, max_bytes=MAX_FILE_BYTES, timeout=NETWORK_TIMEOUT):
    """HTTPS, bounded reads, no silent authority-changing redirects."""
    request = urllib.request.Request(url, headers={"User-Agent": "LOCachy-upstream/1"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            if urllib.parse.urlsplit(response.url).scheme != "https" or \
                    urllib.parse.urlsplit(response.url).netloc != "gitlab.archlinux.org":
                raise UpstreamError("upstream request redirected outside its authority")
            data = response.read(max_bytes + 1)
    except (OSError, ValueError) as e:
        raise UpstreamError(f"stable upstream did not answer: {e}") from e
    if len(data) > max_bytes:
        raise UpstreamError("upstream response exceeded the input budget")
    return data


def _fetch(fetch, url, max_bytes=MAX_FILE_BYTES):
    data = fetch(url, max_bytes=max_bytes, timeout=NETWORK_TIMEOUT)
    if not isinstance(data, bytes) or len(data) > max_bytes:
        raise UpstreamError("invalid or oversized upstream response")
    return data


def _fetch_json(fetch, url):
    try:
        return json.loads(_fetch(fetch, url))
    except (ValueError, UnicodeError) as e:
        raise UpstreamError("stable authority returned invalid metadata") from e


def _private_directory(path):
    path = Path(path)
    if path.is_symlink():
        raise UpstreamError("state directory cannot be a symlink")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    st = path.stat()
    if st.st_uid != os.geteuid() or st.st_mode & 0o022:
        raise UpstreamError("state directory must be owned by the updater and not writable by others")
    return path


def _state(name, state_dir):
    _name(name)
    root = _private_directory(state_dir)
    upstream = _private_directory(root / "upstream")
    return _private_directory(upstream / name)


def _write(path, data):
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with open(path, "xb") as f:
        f.write(data)
    path.chmod(0o600)


def _shell_string(value):
    if "\n" in value or "\r" in value or "\0" in value:
        raise UpstreamError("invalid shell metadata")
    return "'" + value.replace("'", "'\\''") + "'"


def _effective_wrapper(cfg, metadata, extras):
    lines = ["# Generated from a locked stable recipe; evaluated only by isolated makepkg.",
             "pkgname=" + _shell_string(cfg["name"]), "pkgbase=" + _shell_string(cfg["name"]),
             "pkgver=" + _shell_string(metadata["pkgver"]),
             "pkgrel=" + _shell_string(metadata["pkgrel"]),
             "epoch=" + str(int(metadata["epoch"]) + 1),
             "pkgdesc=" + _shell_string(metadata["description"]),
             "_locachy_upstream_pkgver=" + _shell_string(metadata["pkgver"]),
             "_locachy_upstream_pkgrel=" + _shell_string(metadata["pkgrel"]),
             "_locachy_upstream_epoch=" + _shell_string(metadata["epoch"]),
             "_locachy_patch_revision=" + str(cfg["patch_revision"]),
             "_locachy_extra_sources=(" + " ".join(_shell_string(x) for x in extras) + ")"]
    for algorithm in ALGORITHMS:
        make_hash = hashlib.blake2b if algorithm == "b2" else lambda data, a=algorithm: hashlib.new(a, data)
        sums = [make_hash(data).hexdigest() for data in extras.values()]
        lines.append(f"_locachy_extra_{algorithm}sums=(" + " ".join(_shell_string(x) for x in sums) + ")")
    lines.append('source "$startdir/adapter.sh"')
    return ("\n".join(lines) + "\n").encode()


def _remote_tree(cfg, commit, fetch):
    files = {}
    total = 0
    page = 1
    # Repository API pages are bounded by the total bytes and strictly grow:
    # repeated/empty pages cannot turn an authority error into an infinite loop.
    seen = set()
    while True:
        url = cfg["api"] + "/repository/tree?" + urllib.parse.urlencode({
            "ref": commit, "recursive": "true", "per_page": 100, "page": page})
        entries = _fetch_json(fetch, url)
        if not isinstance(entries, list):
            raise UpstreamError("invalid upstream tree listing")
        if not entries:
            break
        fingerprint = _sha(_json_bytes(entries))
        if fingerprint in seen:
            raise UpstreamError("upstream tree pagination did not advance")
        seen.add(fingerprint)
        for entry in entries:
            name = entry.get("path", "")
            _relative(name)
            mode = entry.get("mode")
            kind = entry.get("type")
            if kind == "tree" and mode == "040000":
                continue
            if kind == "blob" and mode == "120000":
                target = _fetch(fetch, cfg["api"] + f"/repository/files/{urllib.parse.quote(name, safe='')}/raw?ref={commit}", 1024)
                try:
                    target_text = target.decode("utf-8").strip()
                except UnicodeDecodeError as e:
                    raise UpstreamError("upstream symlink target is not text") from e
                if target_text.startswith("/") or not target_text or "\n" in target_text or "\0" in target_text:
                    raise UpstreamError("upstream symlink target is unsafe")
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target_text))
                _relative(resolved)
                # Resolve only a regular target from this immutable tree. The
                # link becomes a regular copy, so builder input has no links.
                target_entry = next((x for x in entries if x.get("path") == resolved), None)
                if not target_entry or target_entry.get("mode") not in ("100644", "100755"):
                    raise UpstreamError("upstream symlink does not target a regular recipe file")
                target_url = cfg["api"] + f"/repository/files/{urllib.parse.quote(resolved, safe='')}/raw?ref={commit}"
                raw = _fetch(fetch, target_url)
                total += len(raw) + len(name.encode()) + 128
                if total > MAX_TREE_BYTES:
                    raise UpstreamError("upstream recipe tree exceeds the input budget")
                files[name] = raw
                continue
            if kind != "blob" or mode not in ("100644", "100755"):
                raise UpstreamError("upstream submodules and special files are unsupported")
            if name in files:
                raise UpstreamError("duplicate upstream file")
            encoded = urllib.parse.quote(name, safe="")
            raw = _fetch(fetch, cfg["api"] + f"/repository/files/{encoded}/raw?ref={commit}")
            total += len(raw) + len(name.encode()) + 128
            if total > MAX_TREE_BYTES:
                raise UpstreamError("upstream recipe tree exceeds the input budget")
            files[name] = raw
        if len(entries) < 100:
            break
        page += 1
        if len(files) * 128 > MAX_TREE_BYTES:
            raise UpstreamError("upstream recipe has too many file names for its input budget")
    if "PKGBUILD" not in files or ".SRCINFO" not in files:
        raise UpstreamError("stable recipe lacks PKGBUILD or .SRCINFO")
    return dict(sorted(files.items()))


def prepare_candidate(name, baseline_dir, repo_name, repo_version, state_dir, *, stable_version, fetch=None, run=None):
    """Return the immutable candidate lock.json Path; do not promote/install.

    stable_version comes from the official extra database in the fixed Check
    snapshot. repo_name/repo_version describe only the target's chosen package;
    they never select or rewrite the source recipe. fetch is injectable.
    run is accepted for host integration symmetry, but deliberately never used:
    no subprocess or recipe code executes while checking upstream.
    """
    del run
    cfg = _config(name, baseline_dir)
    inputs = _files(baseline_dir)
    local_hash = tree_hash(baseline_dir)
    requested = stable_source_version(stable_version)
    fetch = fetch or fetch_bytes
    tag_url = cfg["api"] + "/repository/tags/" + urllib.parse.quote(requested["tag"], safe="")
    tag = _fetch_json(fetch, tag_url)
    if not isinstance(tag, dict) or tag.get("name") != requested["tag"]:
        raise UpstreamError("stable packaging tag does not match official extra version")
    commit = tag.get("commit", {}).get("id", "")
    if not COMMIT_RE.fullmatch(commit):
        raise UpstreamError("stable packaging tag lacks an immutable commit")
    remote = _remote_tree(cfg, commit, fetch)
    info = read_srcinfo(remote[".SRCINFO"])
    if _single(info, "pkgbase") != cfg["pkgbase"] or info.get("pkgname") != cfg["packages"]:
        raise UpstreamError("split or renamed stable recipe requires adapter review")
    selected = _selected_srcinfo(remote[".SRCINFO"], cfg["package"])
    actual = {key: _single(selected, key, "0" if key == "epoch" else None)
              for key in ("pkgver", "pkgrel", "epoch")}
    if actual != {key: requested[key] for key in actual}:
        raise UpstreamError("stable .SRCINFO version does not match the official extra package")
    if selected.get("arch") not in (["x86_64"], ["any"]):
        raise UpstreamError("stable recipe does not target x86_64")
    if not actual["epoch"].isdigit() or int(actual["epoch"]) >= 2**31 - 1:
        raise UpstreamError("unsupported stable epoch")
    extras = {path: inputs[path] for path in cfg["inputs"] if path in inputs}
    if len(extras) != len(cfg["inputs"]) or cfg["adapter"] not in inputs:
        raise UpstreamError("local overlay input is missing")
    reserved = {cfg["adapter"], *extras, ".locachy-upstream.PKGBUILD"}
    if any(x in remote for x in reserved):
        raise UpstreamError("upstream file conflicts with local adapter inputs")
    metadata = dict(actual, description=_single(selected, "pkgdesc", cfg["name"]))
    package_version = f"{int(actual['epoch']) + 1}:{actual['pkgver']}-{actual['pkgrel']}"
    root = _state(name, state_dir)
    candidates = _private_directory(root / "candidates")
    temp = Path(tempfile.mkdtemp(prefix=".new-", dir=candidates))
    try:
        upstream = temp / "upstream"
        recipe = temp / "recipe"
        for path, data in remote.items():
            _write(upstream / path, data)
            if path not in ("PKGBUILD", ".SRCINFO"):
                _write(recipe / path, data)
        _write(recipe / ".locachy-upstream.PKGBUILD", remote["PKGBUILD"])
        _write(recipe / cfg["adapter"], inputs[cfg["adapter"]])
        for path, data in extras.items():
            _write(recipe / path, data)
        _write(recipe / "PKGBUILD", _effective_wrapper(cfg, metadata, extras))
        lock = {"schema": SCHEMA, "name": name, "upstream_package": cfg["package"],
                "upstream_pkgbase": cfg["pkgbase"],
                "repository": repo_name, "repository_version": repo_version,
                "source_repository": "extra",
                "source_version": ((actual["epoch"] + ":") if actual["epoch"] != "0" else "")
                                  + f"{actual['pkgver']}-{actual['pkgrel']}",
                "upstream_tag": requested["tag"], "packaging_commit": commit,
                "source_authority": cfg["repository"], "architecture": "x86_64",
                "package_version": package_version, "patch_revision": cfg["patch_revision"],
                "gate": cfg["gate"], "local_input_hash": local_hash,
                "upstream_tree_hash": tree_hash(upstream), "effective_recipe_hash": tree_hash(recipe),
                "input_hashes": {k: _sha(v) for k, v in extras.items()}, "recipe": "recipe"}
        identity = _sha(_json_bytes(lock))
        lock["candidate_id"] = identity
        _write(temp / "lock.json", _json_bytes(lock))
        destination = candidates / identity
        if destination.exists():
            validate_candidate(destination / "lock.json", name, baseline_dir, state_dir)
            shutil.rmtree(temp)
        else:
            temp.rename(destination)
        return destination / "lock.json"
    except BaseException:
        if temp.exists():
            shutil.rmtree(temp)
        raise


def _protected_tree(path):
    for base, dirs, files in os.walk(path, followlinks=False):
        for full in [Path(base)] + [Path(base, n) for n in dirs + files]:
            st = full.lstat()
            if stat.S_ISLNK(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o022:
                raise UpstreamError("candidate tree is not protected")


def validate_candidate(lock_path, name, baseline_dir, state_dir):
    """Validate fixed provenance and current local input; return lock metadata.
    A stale/modified candidate fails, never silently resolves a newer one.
    """
    cfg = _config(name, baseline_dir)
    root = _state(name, state_dir)
    candidates = _private_directory(root / "candidates")
    path = Path(lock_path)
    if path.parent.is_symlink() or path.is_symlink() or path.name != "lock.json":
        raise UpstreamError("invalid candidate lock path")
    resolved = path.resolve()
    if resolved.parent.parent != candidates.resolve():
        raise UpstreamError("candidate lock is outside the package state")
    try:
        if path.stat().st_size > MAX_FILE_BYTES or path.stat().st_uid != os.geteuid() or path.stat().st_mode & 0o022:
            raise UpstreamError("candidate lock is not protected")
        lock = json.loads(path.read_bytes())
    except (OSError, ValueError) as e:
        raise UpstreamError("candidate lock is unreadable") from e
    if not isinstance(lock, dict) or lock.get("schema") != SCHEMA or lock.get("name") != name:
        raise UpstreamError("invalid candidate identity")
    identity = lock.get("candidate_id", "")
    if not HASH_RE.fullmatch(identity) or identity != path.parent.name:
        raise UpstreamError("candidate identifier mismatch")
    body = dict(lock)
    del body["candidate_id"]
    if _sha(_json_bytes(body)) != identity:
        raise UpstreamError("candidate lock was changed")
    if lock.get("source_authority") != cfg["repository"] or not COMMIT_RE.fullmatch(lock.get("packaging_commit", "")):
        raise UpstreamError("candidate authority mismatch")
    if lock.get("upstream_package") != cfg["package"] or \
            lock.get("upstream_pkgbase", cfg["package"]) != cfg["pkgbase"]:
        raise UpstreamError("candidate package selection mismatch")
    if lock.get("source_repository") != "extra":
        raise UpstreamError("candidate is not from the official stable source")
    source = stable_source_version(lock.get("source_version", ""))
    if lock.get("upstream_tag") != source["tag"]:
        raise UpstreamError("candidate stable source tag mismatch")
    if lock.get("local_input_hash") != local_input_hash(baseline_dir):
        raise UpstreamError("local recipe or patch changed since Check")
    if lock.get("gate") != cfg["gate"] or lock.get("patch_revision") != cfg["patch_revision"]:
        raise UpstreamError("candidate gate or patch revision changed")
    if lock.get("recipe") != "recipe" or tree_hash(path.parent / "recipe") != lock.get("effective_recipe_hash"):
        raise UpstreamError("effective candidate recipe changed")
    _protected_tree(path.parent / "recipe")
    _protected_tree(path.parent / "upstream")
    if tree_hash(path.parent / "upstream") != lock.get("upstream_tree_hash"):
        raise UpstreamError("stable upstream recipe tree changed")
    return lock


def promote_candidate(lock_path, name, baseline_dir, state_dir, *, artifact_sha256, gate):
    """Call only after the exact artifact's required gate and installation.
    Store the lock, artifact identity and gate proof atomically; no auto-build.
    """
    lock = validate_candidate(lock_path, name, baseline_dir, state_dir)
    if not HASH_RE.fullmatch(artifact_sha256):
        raise UpstreamError("promotion lacks an exact artifact hash")
    if not isinstance(gate, dict) or gate.get("id") != lock["gate"] or gate.get("passed") is not True or \
            gate.get("artifact_sha256") != artifact_sha256 or not HASH_RE.fullmatch(gate.get("test_hash", "")):
        raise UpstreamError("promotion requires the matching artifact's passed contract gate")
    root = _state(name, state_dir)
    active = {"schema": SCHEMA, "candidate_id": lock["candidate_id"],
              "artifact_sha256": artifact_sha256, "gate": gate}
    temp = root / (".active-" + str(os.getpid()))
    _write(temp, _json_bytes(active))
    os.replace(temp, root / "active.json")
    return Path(lock_path).parent / "recipe"


def effective_recipe(name, baseline_dir, state_dir):
    """The last promoted, still-valid recipe, or overlay inputs if none.
    Overlay inputs contain no executable recipe until a candidate is prepared.
    A broken/stale promotion fails closed instead of rebuilding an old version.
    """
    root = _state(name, state_dir)
    active_path = root / "active.json"
    if not active_path.exists():
        return Path(baseline_dir)
    if active_path.is_symlink():
        raise UpstreamError("active recipe record is a symlink")
    st = active_path.stat()
    if st.st_uid != os.geteuid() or st.st_mode & 0o022 or st.st_size > MAX_FILE_BYTES:
        raise UpstreamError("active recipe record is not protected")
    try:
        active = json.loads(active_path.read_bytes())
    except (OSError, ValueError) as e:
        raise UpstreamError("active recipe record is unreadable") from e
    identity = active.get("candidate_id", "") if isinstance(active, dict) else ""
    if not HASH_RE.fullmatch(identity):
        raise UpstreamError("invalid active recipe identity")
    lock_path = root / "candidates" / identity / "lock.json"
    lock = validate_candidate(lock_path, name, baseline_dir, state_dir)
    artifact = active.get("artifact_sha256", "")
    proof = active.get("gate", {})
    if not HASH_RE.fullmatch(artifact) or proof.get("id") != lock["gate"] or proof.get("passed") is not True or \
            proof.get("artifact_sha256") != artifact or not HASH_RE.fullmatch(proof.get("test_hash", "")):
        raise UpstreamError("active recipe lacks a passed artifact gate")
    return lock_path.parent / "recipe"
