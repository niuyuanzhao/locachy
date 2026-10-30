"""Prepared repository/local-package transactions for LOCachy.

Check copies sync databases and the complete pacman configuration into an
immutable plan directory. Apply builds and gates every tracked local output
before touching the host, then installs those outputs and repository updates
in one pacman -Su transaction using that exact snapshot (never -y). Once it
has, the snapshot's sync databases become the host's: what is installed now
matches them, and later installs and AUR builds (whose roots use the host's)
resolve against it, not against an older state the mirrors no longer serve.

The real pacman database lock is held while an alternate DBPath has the
actual local database bind-mounted as its local, in a private mount
namespace; afterwards every package of the transaction must have a complete
entry there. This blocks ordinary pacman callers;
the child additionally takes its own alternate-DBPath lock. A killed
coordinator may leave the real lock behind: fail closed, never remove a lock
belonging to another process. This is dependency-consistent packaging, not
filesystem rollback or an atomic guarantee about arbitrary package hooks.
"""
import contextlib
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import tempfile
import time


class TransactionError(RuntimeError):
    pass


def file_hash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for data in iter(lambda: f.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def tree_hash(path):
    root = Path(path)
    if not root.is_dir() or root.is_symlink():
        raise TransactionError(f"not an ordinary directory: {path}")
    h = hashlib.sha256()
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in dirs + files:
            if Path(base, name).is_symlink():
                raise TransactionError(f"symlink in transaction inputs: {Path(base, name)}")
        for name in sorted(files):
            p = Path(base, name)
            if not p.is_file():
                raise TransactionError(f"non-file transaction input: {p}")
            h.update(p.relative_to(root).as_posix().encode() + b"\0")
            with p.open("rb") as f:
                for data in iter(lambda: f.read(1024 * 1024), b""):
                    h.update(data)
            h.update(b"\0")
    return h.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(value, f, sort_keys=True)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        value = json.load(f)
    if not isinstance(value, dict):
        raise TransactionError(f"not a transaction object: {path}")
    return value


def flatten_config(path, stack=()):
    """Preserve repository order and options, pinning every Include's bytes.
    The resulting file has no mutable Include references left in it."""
    path = Path(path).resolve()
    if path in stack:
        raise TransactionError(f"recursive pacman Include: {path}")
    if len(stack) >= 32:
        raise TransactionError("pacman Include nesting exceeds parsing budget")
    output = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            key, sep, value = line.partition("=")
            if sep and key.strip() == "Include":
                pattern = value.split("#", 1)[0].strip()
                if not os.path.isabs(pattern):
                    pattern = str(path.parent / pattern)
                matches = sorted(glob.glob(pattern))
                if not matches:
                    raise TransactionError(f"pacman Include matches no files: {pattern}")
                for include in matches:
                    output.append(flatten_config(include, stack + (path,)))
            else:
                output.append(line if line.endswith("\n") else line + "\n")
    return "".join(output)


TEST_PATHS = ("base", "init", "progs", "link", "tools", "rc.conf", "packages")


def test_source_hash(source):
    """Only the trusted build/gate inputs, never output/cache directories."""
    source = Path(source)
    h = hashlib.sha256()
    for name in TEST_PATHS:
        path = source / name
        if path.is_symlink() or not path.exists():
            raise TransactionError(f"missing or linked test input: {path}")
        digest = tree_hash(path) if path.is_dir() else file_hash(path)
        h.update(name.encode() + b"\0" + digest.encode() + b"\0")
    return h.hexdigest()


def snapshot(state_dir, sync_dir, pacman_config, host_sha256, recipes, candidate_locks=None, test_source=None,
             missing=None):
    """recipes maps names to {baseline_dir, recipe_dir, candidate}. No recipe
    is evaluated here; every input hash is rechecked before preparation.
    missing: declared repository packages the host lacks, installed in the
    same transaction (and so in its build and contract roots)."""
    parent = Path(state_dir) / "transactions"
    parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory = Path(tempfile.mkdtemp(prefix="plan-", dir=parent))
    try:
        # tree_hash rejects symlinks and non-files before copy.
        tree_hash(sync_dir)
        shutil.copytree(sync_dir, directory / "sync")
        text = flatten_config(pacman_config)
        (directory / "pacman.conf").write_text(text, encoding="utf-8")
        missing = sorted(set(missing or []))
        if any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9@._+\-]*", name) for name in missing):
            raise TransactionError("invalid declared package name")
        pins = {}
        for name, entry in recipes.items():
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9@._+\-]*", name):
                raise TransactionError(f"invalid local package name: {name}")
            baseline = Path(entry["baseline_dir"]).resolve()
            recipe = Path(entry.get("recipe_dir", baseline)).resolve()
            pins[name] = dict(entry, baseline_dir=str(baseline), recipe_dir=str(recipe),
                              baseline_sha256=tree_hash(baseline), recipe_sha256=tree_hash(recipe))
        data = {"version": 1, "created": time.strftime("%Y-%m-%d %H:%M:%S"),
                "host_sha256": host_sha256, "sync_sha256": tree_hash(directory / "sync"),
                "config_sha256": file_hash(directory / "pacman.conf"), "recipes": pins,
                "candidate_locks": candidate_locks or {}, "missing": missing}
        if test_source is not None:
            source = Path(test_source).resolve()
            digest = test_source_hash(source)
            target = directory / "test-source"
            target.mkdir(mode=0o700)
            for name in TEST_PATHS:
                item = source / name
                if item.is_dir(): shutil.copytree(item, target / name)
                else: shutil.copyfile(item, target / name)
            if test_source_hash(target) != digest:
                raise TransactionError("test sources changed while checking; check again")
            data["test_source"] = {"original": str(source), "snapshot": str(target), "sha256": digest}
        write_json(directory / "plan.json", data)
        return {"directory": str(directory), "sha256": file_hash(directory / "plan.json")}
    except Exception:
        shutil.rmtree(directory)
        raise


def load_snapshot(reference, expected_host=None):
    directory = Path(reference["directory"])
    if not directory.is_absolute() or directory.is_symlink():
        raise TransactionError("transaction directory is not an absolute pinned directory")
    if file_hash(directory / "plan.json") != reference["sha256"]:
        raise TransactionError("update plan lock changed; check again")
    data = read_json(directory / "plan.json")
    if data.get("version") != 1:
        raise TransactionError("unsupported update transaction version")
    if tree_hash(directory / "sync") != data.get("sync_sha256"):
        raise TransactionError("repository snapshot changed; check again")
    if file_hash(directory / "pacman.conf") != data.get("config_sha256"):
        raise TransactionError("repository configuration changed; check again")
    if expected_host is not None and data.get("host_sha256") != expected_host:
        raise TransactionError("host packages changed since check; check again")
    test_source = data.get("test_source")
    if test_source and (test_source_hash(test_source["original"]) != test_source["sha256"] or
                        test_source_hash(test_source["snapshot"]) != test_source["sha256"]):
        raise TransactionError("trusted test sources changed; check again")
    for name, entry in data.get("recipes", {}).items():
        if tree_hash(entry["baseline_dir"]) != entry.get("baseline_sha256"):
            raise TransactionError(f"{name}: local patch inputs changed; check again")
        if tree_hash(entry["recipe_dir"]) != entry.get("recipe_sha256"):
            raise TransactionError(f"{name}: effective recipe changed; check again")
    return directory, data


def _invoke(run, command, **kwargs):
    """A small injectable subprocess boundary; every nonzero result fails."""
    p = run(command, **kwargs)
    if isinstance(p, bool):
        ok = p
    else:
        ok = p.returncode == 0
    if not ok:
        detail = getattr(p, "stderr", None) or getattr(p, "stdout", None) or ""
        if isinstance(detail, bytes):
            detail = detail.decode(errors="replace")
        raise TransactionError("command failed: " + " ".join(map(str, command)) +
                               ("\n" + detail[-4000:].strip() if detail else ""))
    return p


def package_versions(text):
    packages = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[0] in packages:
            raise TransactionError("invalid or duplicate package in dependency manifest")
        packages[fields[0]] = fields[1]
    if not packages:
        raise TransactionError("empty target dependency manifest")
    return packages


def prepared_result(directory, name, data):
    result = read_json(Path(directory) / "prepared" / name / "result.json")
    if result.get("name") != name or result.get("source_host_sha256") != data["host_sha256"]:
        raise TransactionError(f"{name}: prepared result is for a different host")
    tests = result.get("tests")
    if not isinstance(tests, dict) or tests.get("passed") is not True:
        raise TransactionError(f"{name}: candidate tests did not pass")
    prepared = Path(directory) / "prepared" / name
    for filename, field in (("target-packages.txt", "target_packages_sha256"),
                            ("target-base-packages.txt", "target_base_sha256")):
        path = prepared / filename
        if not path.is_file() or path.is_symlink() or file_hash(path) != result.get(field):
            raise TransactionError(f"{name}: candidate target dependency manifest changed")
        package_versions(path.read_text())
    artifact = Path(result.get("artifact", ""))
    prepared = Path(directory) / "prepared" / name
    if (not artifact.is_absolute() or artifact.is_symlink() or not artifact.is_file() or
            prepared.resolve() not in artifact.resolve().parents):
        raise TransactionError(f"{name}: candidate artifact is outside its prepared directory")
    if file_hash(artifact) != result.get("sha256"):
        raise TransactionError(f"{name}: candidate artifact hash changed")
    candidate = data["recipes"][name].get("candidate")
    if candidate and result.get("version") != candidate.get("package_version"):
        raise TransactionError(f"{name}: built version differs from the pinned candidate")
    if data.get("test_source") and tests.get("source_sha256") != data["test_source"]["sha256"]:
        raise TransactionError(f"{name}: candidate tests used different trusted sources")
    if candidate and (tests.get("id") != candidate.get("gate") or
                      tests.get("artifact_sha256") != result["sha256"] or
                      not re.fullmatch(r"[0-9a-f]{64}", tests.get("test_hash", "")) or
                      (data.get("test_source") and tests.get("test_hash") != data["test_source"]["sha256"])):
        raise TransactionError(f"{name}: test proof does not match its required artifact gate")
    if not isinstance(result.get("record"), dict) or not result.get("version"):
        raise TransactionError(f"{name}: candidate build metadata is incomplete")
    if result.get("recipe_sha256") != data["recipes"][name]["recipe_sha256"]:
        raise TransactionError(f"{name}: candidate was built from different recipe inputs")
    return result


def prepare(reference, build_tool, host_hash, run=subprocess.run):
    directory, data = load_snapshot(reference, host_hash())
    for name in sorted(data["recipes"]):
        # Old successful results cannot survive a failed replacement build.
        result = directory / "prepared" / name / "result.json"
        if result.exists():
            result.unlink()
    for name in sorted(data["recipes"]):
        print(f"===> prepare: build and seal {name} before host changes", flush=True)
        _invoke(run, [build_tool, "--prepare", str(directory), "--local", name])
        load_snapshot(reference, host_hash())
    print("===> prepare: test all local outputs together before host changes", flush=True)
    _invoke(run, [build_tool, "--validate-prepared", str(directory)])
    load_snapshot(reference, host_hash())
    results = {name: prepared_result(directory, name, data) for name in sorted(data["recipes"])}
    verify_combined_results(results)
    return directory, data, results


def verify_combined_results(results):
    """All contracts must describe one target root and exact artifact set."""
    targets = {r["target_base_sha256"] for r in results.values()}
    installed = {r["target_packages_sha256"] for r in results.values()}
    if len(targets) > 1 or len(installed) > 1:
        raise TransactionError("local candidates were tested against different target package sets")
    artifacts = {name: result["sha256"] for name, result in results.items()}
    for name, result in results.items():
        proof = result["tests"].get("artifacts")
        if len(results) > 1 and proof != artifacts:
            raise TransactionError(f"{name}: contract did not test the complete candidate artifact set")
        if proof is not None and proof != artifacts:
            raise TransactionError(f"{name}: co-installed artifact hashes changed")


@contextlib.contextmanager
def real_database_lock(db_path):
    path = Path(db_path) / "db.lck"
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    except FileExistsError as e:
        raise TransactionError(f"pacman database is locked: {path}") from e
    identity = os.fstat(fd)
    try:
        os.write(fd, (str(os.getpid()) + "\n").encode())
        yield
    finally:
        os.close(fd)
        # Do not remove a replacement lock if someone externally interfered.
        try:
            current = path.stat()
            if (current.st_dev, current.st_ino) == (identity.st_dev, identity.st_ino):
                path.unlink()
        except FileNotFoundError:
            pass


def transaction_config(config_text, repo_dir):
    if "[locachy-validated]" in config_text:
        raise TransactionError("reserved locachy-validated repository already exists")
    # These paths belong to the installation root, not an arbitrary Include.
    # The pinned database is mounted at the normal path in a private namespace;
    # RootDir must remain the real root.
    section = None
    kept = []
    for line in config_text.splitlines(keepends=True):
        match = re.match(r"\s*\[([^]]+)\]", line)
        if match:
            section = match.group(1)
        key, sep, value = line.partition("=")
        if section == "options" and sep:
            if key.strip() == "DBPath":
                continue
            if key.strip() == "RootDir" and value.split("#", 1)[0].strip() != "/":
                raise TransactionError("prepared host update cannot change RootDir")
        kept.append(line)
    config_text = "".join(kept)
    sections = list(re.finditer(r"(?m)^\s*\[([^]\n]+)\]\s*$", config_text))
    if not sections or sections[0].group(1) != "options":
        raise TransactionError("pacman configuration must begin with [options]")
    split = sections[1].start() if len(sections) > 1 else len(config_text)
    repo = ("\n[locachy-validated]\nSigLevel = Optional TrustAll\n"
            f"Server = {Path(repo_dir).as_uri()}\n\n")
    return config_text[:split] + repo + config_text[split:]


def preview_manifest(output):
    """Only explicitly tagged --print-format records are package metadata.
    Distribution notices on stdout are displayed, never parsed as packages."""
    if isinstance(output, bytes):
        output = output.decode()
    rows = []
    for line in output.splitlines():
        if line.startswith("LOCACHY-PACKAGE "):
            rows.append(line[len("LOCACHY-PACKAGE "):])
        elif line.strip():
            print(line, flush=True)
    manifest = "\n".join(rows) + "\n"
    package_versions(manifest)  # Empty, duplicate or malformed records fail.
    return manifest


def verify_install_versions(transaction_output, directory, results):
    """Every version pacman is about to install must have been in each local
    candidate's actual test root. Build-only packages may be absent on host;
    different versions may not masquerade as the tested dependency set."""
    lines = transaction_output.decode() if isinstance(transaction_output, bytes) else transaction_output
    selected = package_versions(lines)
    for name, result in results.items():
        tested = package_versions((Path(directory) / "prepared" / name / "target-packages.txt").read_text())
        for package, version in selected.items():
            if tested.get(package) != version:
                raise TransactionError(f"{name}: {package}={version} was not tested in the candidate root")
        if selected.get(name) != result["version"]:
            raise TransactionError(f"{name}: package transaction does not select its tested artifact version")


def with_local(command, real_local, private_local):
    """The pinned database at pacman's normal path, with the real local bound
    into it first. This preserves complete writes and the real outer lock,
    without relying on distribution-specific handling of --dbpath."""
    return ["unshare", "--mount", "--propagation", "private", "sh", "-c",
            'mount --bind "$1" "$2" && mount --rbind "${2%/local}" "${1%/local}" && shift 2 && exec "$@"', "sh",
            str(real_local), str(private_local)] + command


def verify_recorded(transaction_output, real_local):
    """Every package of the transaction has a complete entry, and only one
    version of it, in the real local database."""
    selected = package_versions(transaction_output)
    for name, version in selected.items():
        entry = Path(real_local) / (name + "-" + version)
        if not (entry / "desc").is_file() or not (entry / "files").is_file():
            raise TransactionError(f"pacman did not record {name} {version} in the local database")
        others = [p.name for p in Path(real_local).glob(glob.escape(name) + "-*")
                  if p != entry and p.name.rsplit("-", 2)[0] == name]
        if others:
            raise TransactionError(f"{name}: old local database entries remain: {', '.join(others)}")


def publish_sync(snapshot_sync, host_sync):
    """The snapshot's repository databases as the host's, each replaced
    whole (rename), others left as they are."""
    host_sync = Path(host_sync)
    host_sync.mkdir(parents=True, exist_ok=True)
    for source in sorted(Path(snapshot_sync).iterdir()):
        if not source.is_file() or source.is_symlink():
            continue
        temporary = host_sync / (source.name + ".locachy-new")
        shutil.copyfile(source, temporary)
        os.chmod(temporary, 0o644)
        os.replace(temporary, host_sync / source.name)


def install(reference, directory, data, results, host_hash, db_path="/var/lib/pacman",
            run=subprocess.run, net=lambda c: c):
    """Prepare has already finished. Hold the real lock until the pacman child
    exits; its alternate local DB is the real one bind-mounted, not a copy."""
    if not results:
        raise TransactionError("prepared update has no validated local packages")
    if set(results) != set(data["recipes"]):
        raise TransactionError("package transaction omits a prepared local output")
    state = Path(directory)
    results = {name: prepared_result(directory, name, data) for name in results}
    verify_combined_results(results)
    workspace = Path(tempfile.mkdtemp(prefix="install-", dir=state))
    try:
        repo = workspace / "repo"
        repo.mkdir()
        artifacts = []
        for name in sorted(results):
            result = prepared_result(directory, name, data)
            target = repo / Path(result["artifact"]).name
            # Copy, not writable hardlinks into a previously validated artifact.
            shutil.copyfile(result["artifact"], target)
            artifacts.append(str(target))
        _invoke(run, ["repo-add", str(repo / "locachy-validated.db.tar.gz")] + artifacts)
        private_db = workspace / "db"
        private_db.mkdir()
        real_local = Path(db_path).resolve() / "local"
        (private_db / "local").mkdir()
        shutil.copytree(state / "sync", private_db / "sync")
        # repo-add creates a .db symlink; dereference this one known local output.
        shutil.copyfile(repo / "locachy-validated.db.tar.gz", private_db / "sync/locachy-validated.db")
        config = workspace / "pacman.conf"
        config.write_text(transaction_config((state / "pacman.conf").read_text(), repo), encoding="utf-8")
        targets = ["locachy-validated/" + name + "=" + results[name]["version"] for name in sorted(results)]
        # Declared packages the host lacks: tested in the candidates' roots.
        targets += [name for name in data.get("missing", [])
                    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9@._+\-]*", name)]
        with real_database_lock(db_path):
            load_snapshot(reference, host_hash())
            checked = {name: prepared_result(directory, name, data) for name in results}
            verify_combined_results(checked)
            # The system builds its own initramfs once reconciliation ends;
            # preview and install must resolve the same kernel dependency.
            # Use the standard database location inside the private namespace.
            # CachyOS's notice client concatenates a custom --dbpath after
            # realpath removed its slash; the normal compiled-in path is valid.
            pacman = ["pacman", "--config", str(config), "--assume-installed", "initramfs"]
            preview = _invoke(run, net(with_local(pacman + ["-Sup", "--print-format", "LOCACHY-PACKAGE %n %v"] + targets,
                                                  real_local, private_db / "local")),
                              capture_output=True, text=True)
            if not hasattr(preview, "stdout") or preview.stdout is None:
                raise TransactionError("package manager returned no pinned transaction preview")
            selected_manifest = preview_manifest(preview.stdout)
            verify_install_versions(selected_manifest, directory, results)
            print("===> install: repository updates and validated local packages in one transaction", flush=True)
            _invoke(run, net(with_local(pacman + ["-Su", "--noconfirm", "--ask", "4"] + targets,
                                        real_local, private_db / "local")))
            verify_recorded(selected_manifest, real_local)
            publish_sync(state / "sync", Path(db_path) / "sync")
        return True
    finally:
        shutil.rmtree(workspace)


def preserve_installed(results, record_dir):
    """Save the last installed archive before a same-name candidate can
    replace it. This runs before the package transaction, not after it."""
    root = Path(record_dir)
    for name in results:
        old = root / (name + ".json")
        if not old.is_file():
            continue
        prior = read_json(old)
        artifact = Path(prior.get("artifact", ""))
        if not artifact.is_absolute():
            artifact = Path("/var/cache/locachy-pkgs") / artifact
        previous = root / "last-good" / name
        previous.mkdir(parents=True, exist_ok=True, mode=0o700)
        if artifact.is_file():
            target = previous / artifact.name
            shutil.copyfile(artifact, target)
            if file_hash(target) != file_hash(artifact):
                raise TransactionError(f"{name}: could not preserve the previous artifact")
        shutil.copyfile(old, previous / "record.json")


def publish_records(results, record_dir, actual_host_hash, promote=None):
    """Called only after pacman succeeded; preserve_installed ran before it.
    The referenced prepared artifact remains immutable after installation."""
    root = Path(record_dir)
    root.mkdir(parents=True, exist_ok=True, mode=0o755)
    for name, result in results.items():
        old = root / (name + ".json")
        record = dict(result["record"], version=result["version"], sha256=result["sha256"],
                      artifact=result["artifact"], transaction_host=actual_host_hash,
                      tests=result["tests"], target_packages_sha256=result["target_packages_sha256"])
        write_json(old, record)
        if promote is not None:
            promote(name, result)
