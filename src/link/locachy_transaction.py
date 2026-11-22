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
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from threading import RLock
import fcntl
import glob
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import signal
import shutil
import stat
import subprocess
import tempfile
import time


class TransactionError(RuntimeError):
    pass


# A shared worker cannot use the jobserver path (e.g. an AUR dependency).
# Only a pre-compilation deferral uses this status, never a makepkg failure.
SERIAL_DEPS = 76


# The host package domain: the jobs runner holds it for every job that changes
# the host, locachy-pkg and locachy-repos take or inherit it, the pacman guard
# holds it for a transaction LOCachy did not start. A fixed path: never one
# given at run time.
DOMAIN_LOCK = "/run/locachy/pkg.lock"


class DomainBusy(RuntimeError):
    """Another package or repository operation holds the host domain."""


def inherited_domain_fd(path=DOMAIN_LOCK):
    """The descriptor of the domain lock a caller passed down (the jobs
    runner, an execv of the same tool), matched by the lock file's (st_dev,
    st_ino), never by a name; -1 when none was inherited."""
    try:
        want = os.stat(path)
    except FileNotFoundError:
        return -1
    for name in os.listdir("/proc/self/fd"):
        try:
            fd = int(name)
            if fd <= 2 or fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC:
                continue
            got = os.fstat(fd)
        except (OSError, ValueError):
            continue
        if stat.S_ISREG(got.st_mode) and (got.st_dev, got.st_ino) == (want.st_dev, want.st_ino):
            return fd
    return -1


def hold_domain(path=DOMAIN_LOCK, inside_job=False):
    """Hold the host package domain without waiting: the inherited open file
    description when the caller holds it, else a new one. Inside a system job
    only the inherited one: a job never takes the domain itself (its runner
    holds it for every job that changes the host), so none there means
    refusal, and no lock file is made. Returns the descriptor (inheritable: an
    execv of the same tool keeps it), or raises DomainBusy."""
    fd = inherited_domain_fd(path)
    if fd < 0 and inside_job:
        raise DomainBusy("this system job does not hold the host package domain; nothing changed")
    if fd < 0:
        os.makedirs(os.path.dirname(path), mode=0o755, exist_ok=True)
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
            raise DomainBusy("the package domain lock is not an owned regular file")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as e:
        os.close(fd)
        raise DomainBusy("another package or repository operation is running; "
                         "wait for it to finish (home: J)") from e
    except DomainBusy:
        os.close(fd)
        raise
    os.set_inheritable(fd, True)
    return fd


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


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


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


PACKAGE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9@._+\-]*")
AUR_SMOKE_GATE = "aur-coinstall-smoke-v1"


def aur_entries(data):
    aur = data.get("aur", {})
    if not isinstance(aur, dict):
        raise TransactionError("invalid AUR update metadata")
    return aur


def prepared_names(data):
    """All packages built before the host transaction: local recipes plus AUR rebuilds."""
    return sorted(set(data.get("recipes", {})) | set(aur_entries(data)))


def repo_artifact_name(path, sha256):
    """Give a validated artifact a hash-qualified filename inside the temporary
    repository so pacman never reuses a stale same-version archive from the host
    cache during a rebuild."""
    name = Path(path).name
    tag = f".locachy-{sha256[:12]}"
    marker = ".pkg.tar."
    if marker in name:
        prefix, suffix = name.split(marker, 1)
        return f"{prefix}{tag}{marker}{suffix}"
    return f"{name}{tag}"


def snapshot(state_dir, sync_dir, pacman_config, host_sha256, recipes, candidate_locks=None, test_source=None,
             missing=None, aur=None):
    """recipes maps local names to {baseline_dir, recipe_dir, candidate}.
    aur maps host AUR package names to {version, target_version, reason, provides, patch_path?}.
    No recipe is evaluated here; every input hash is rechecked before preparation.
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
        if any(not PACKAGE_NAME.fullmatch(name) for name in missing):
            raise TransactionError("invalid declared package name")
        pins = {}
        for name, entry in recipes.items():
            if not PACKAGE_NAME.fullmatch(name):
                raise TransactionError(f"invalid local package name: {name}")
            baseline = Path(entry["baseline_dir"]).resolve()
            recipe = Path(entry.get("recipe_dir", baseline)).resolve()
            pins[name] = dict(entry, baseline_dir=str(baseline), recipe_dir=str(recipe),
                              baseline_sha256=tree_hash(baseline), recipe_sha256=tree_hash(recipe))
        aur_pins = {}
        for name, entry in sorted((aur or {}).items()):
            if not PACKAGE_NAME.fullmatch(name):
                raise TransactionError(f"invalid AUR package name: {name}")
            if name in pins:
                raise TransactionError(f"package cannot be both local and AUR: {name}")
            if not isinstance(entry, dict):
                raise TransactionError(f"{name}: invalid AUR entry")
            provides = sorted({str(p) for p in entry.get("provides", ()) if p})
            if any(not re.fullmatch(r"[A-Za-z0-9@._+:=\-]+", p) for p in provides):
                raise TransactionError(f"{name}: invalid AUR provides entry")
            pinned = {
                "installed_version": str(entry.get("installed_version", "")),
                "target_version": str(entry.get("target_version", "")),
                "reason": str(entry.get("reason", "")),
                "provides": provides,
            }
            patch_path = entry.get("patch_path")
            if patch_path:
                raw_patch = Path(patch_path)
                if raw_patch.is_symlink():
                    raise TransactionError(f"{name}: invalid AUR patch file: {patch_path}")
                patch = raw_patch.resolve()
                if patch.is_symlink() or not patch.is_file():
                    raise TransactionError(f"{name}: invalid AUR patch file: {patch_path}")
                pinned["patch_path"] = str(patch)
                pinned["patch_sha256"] = file_hash(patch)
            aur_pins[name] = pinned
        data = {"version": 1, "created": time.strftime("%Y-%m-%d %H:%M:%S"),
                "host_sha256": host_sha256, "sync_sha256": tree_hash(directory / "sync"),
                "config_sha256": file_hash(directory / "pacman.conf"), "recipes": pins,
                "aur": aur_pins, "candidate_locks": candidate_locks or {}, "missing": missing}
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
    for name, entry in aur_entries(data).items():
        if not PACKAGE_NAME.fullmatch(name):
            raise TransactionError(f"invalid AUR package name: {name}")
        if name in data.get("recipes", {}):
            raise TransactionError(f"package cannot be both local and AUR: {name}")
        patch_path = entry.get("patch_path")
        if patch_path:
            patch = Path(patch_path)
            if patch.is_symlink() or not patch.is_file() or file_hash(patch) != entry.get("patch_sha256"):
                raise TransactionError(f"{name}: AUR patch input changed; check again")
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


@contextlib.contextmanager
def timed_stage(label):
    """Separate package work from the setup, contracts, and host hooks."""
    started = time.monotonic()
    status = "failed"
    print(f"===> {label}", flush=True)
    try:
        yield
        status = "done"
    finally:
        print(f"===> {label}: {status} ({time.monotonic() - started:.1f} s)", flush=True)


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
    if (not artifact.is_absolute() or artifact.is_symlink() or not artifact.is_file() or
            prepared.resolve() not in artifact.resolve().parents):
        raise TransactionError(f"{name}: candidate artifact is outside its prepared directory")
    if file_hash(artifact) != result.get("sha256"):
        raise TransactionError(f"{name}: candidate artifact hash changed")
    aur = aur_entries(data)
    if name in aur:
        if result.get("kind") != "aur":
            raise TransactionError(f"{name}: AUR result metadata is invalid")
        if not result.get("version"):
            raise TransactionError(f"{name}: AUR build version is missing")
        if data.get("test_source") and tests.get("source_sha256") != data["test_source"]["sha256"]:
            raise TransactionError(f"{name}: AUR tests used different trusted sources")
        if (tests.get("id") != AUR_SMOKE_GATE or
                tests.get("artifact_sha256") != result["sha256"] or
                not re.fullmatch(r"[0-9a-f]{64}", tests.get("test_hash", "")) or
                (data.get("test_source") and tests.get("test_hash") != data["test_source"]["sha256"])):
            raise TransactionError(f"{name}: test proof does not match its required AUR gate")
        expected_patch = aur[name].get("patch_sha256")
        if expected_patch != result.get("patch_sha256"):
            raise TransactionError(f"{name}: AUR patch hash differs from the pinned plan")
        extras = result.get("extra_artifacts", [])
        if not isinstance(extras, list):
            raise TransactionError(f"{name}: invalid extra AUR artifacts metadata")
        for extra in extras:
            if not isinstance(extra, dict) or not PACKAGE_NAME.fullmatch(extra.get("name", "")):
                raise TransactionError(f"{name}: invalid extra AUR artifact entry")
            extra_path = Path(extra.get("artifact", ""))
            if (not extra_path.is_absolute() or extra_path.is_symlink() or not extra_path.is_file() or
                    prepared.resolve() not in extra_path.resolve().parents):
                raise TransactionError(f"{name}: extra AUR artifact is outside its prepared directory")
            if file_hash(extra_path) != extra.get("sha256") or not extra.get("version"):
                raise TransactionError(f"{name}: extra AUR artifact hash or version is invalid")
        return result
    if name not in data.get("recipes", {}):
        raise TransactionError(f"{name}: unknown prepared package")
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


def reusable_results(directory, data):
    """The prepared results this plan directory already holds, when every one is
    valid for exactly this snapshot (prepared_result: host, recipe, target package
    set, trusted test sources, artifact hash, passed contract) and they were tested
    together (verify_combined_results); else None. A retried apply of the same
    plan, or results built for it on another machine at the same path, then need
    no second build."""
    names = prepared_names(data)
    if not names:
        return None
    try:
        results = {name: prepared_result(directory, name, data) for name in names}
        verify_combined_results(results)
    except (TransactionError, OSError, ValueError, KeyError, TypeError):
        return None
    return results


def build_resources(workspace):
    """Return the CPU ceiling and available RAM and disk in bytes."""
    limit = len(os.sched_getaffinity(0))
    try:
        available = next(int(line.split()[1]) * 1024
                         for line in Path('/proc/meminfo').read_text().splitlines()
                         if line.startswith('MemAvailable:'))
        group = next(line[3:] for line in Path('/proc/self/cgroup').read_text().splitlines()
                     if line.startswith('0::/'))
        mount = Path('/sys/fs/cgroup')
        relative = Path(group.lstrip('/'))
        current = mount / relative
        # Only visible cgroup v2 ancestors can constrain this estimate.
        if '..' in relative.parts or not current.is_dir():
            return 1, 0, 0
        while True:
            if (current / 'cpu.max').is_file():
                quota, period = (current / 'cpu.max').read_text().split()
                if quota != 'max':
                    limit = min(limit, int(quota) // int(period))
            if (current / 'memory.max').is_file():
                cap = (current / 'memory.max').read_text().strip()
                if cap != 'max':
                    used = int((current / 'memory.current').read_text())
                    available = min(available, int(cap) - used)
            if current == mount:
                break
            current = current.parent
        free = shutil.disk_usage(workspace).free
    except (StopIteration, OSError, ValueError):
        return 1, 0, 0
    return max(1, limit), available, free


def build_slot_budget(workspace):
    """Estimate slots from the effective CPUs, available RAM and workspace."""
    cpus, available, free = build_resources(workspace)
    gib = 1024 ** 3
    return max(1, min(cpus, available // gib, free // (2 * gib)))


def build_resource_headroom(workspace):
    """Do not start an additional root when RAM or disk is tight."""
    _, available, free = build_resources(workspace)
    gib = 1024 ** 3
    return available >= 2 * gib and free >= 4 * gib


@contextlib.contextmanager
def build_jobserver(workspace, slots):
    """Own and seed the batch's GNU jobserver FIFO, including implicit slots."""
    info = workspace.lstat()
    if (os.geteuid() != 0 or info.st_uid != 0 or not stat.S_ISDIR(info.st_mode) or
            info.st_mode & 0o077):
        raise TransactionError('build jobserver needs a private root-owned workspace')
    try:
        builder_gid = pwd.getpwnam('locachy-builder').pw_gid
    except KeyError:
        raise TransactionError('build jobserver needs the locachy-builder account') from None
    path = workspace / 'build-jobserver.fifo'
    with contextlib.ExitStack() as cleanup:
        os.mkfifo(path, 0o600)
        cleanup.callback(path.unlink, missing_ok=True)
        # Linux permits a read/write FIFO open without another participant.
        # Keep both ends alive; children open the path independently so their
        # blocking I/O cannot change the coordinator's nonblocking descriptor.
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
        cleanup.callback(os.close, fd)
        # Root owns the inode; only the build group can open the bind-mounted
        # FIFO inside the worker. The host workspace itself remains private.
        os.fchown(fd, 0, builder_gid)
        os.fchmod(fd, 0o660)
        # A partial read does not free its pipe buffer until the entire page
        # is drained. Leave a page of headroom for nonblocking token returns.
        capacity = slots + os.sysconf('SC_PAGESIZE')
        if capacity > fcntl.fcntl(fd, fcntl.F_GETPIPE_SZ):
            fcntl.fcntl(fd, fcntl.F_SETPIPE_SZ, capacity)
        if os.write(fd, b'+' * slots) != slots:
            raise TransactionError('could not seed the build jobserver FIFO')
        yield path, fd


def run_build_workers(names, slots, active_limit, fd, launch, cancelled, has_headroom=lambda: True):
    """Reserve one global token per active package worker."""
    pending = iter(names)
    name = next(pending, None)
    codes = {}
    # The executor need not create more threads than the FIFO can admit.
    workers = min(slots, active_limit)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        active = {}
        while True:
            for future in tuple(active):
                if future.done():
                    finished, token = active.pop(future)
                    try:
                        code = future.result()
                        if code:
                            codes[finished] = code
                    finally:
                        # The coordinator observes the outcome before this slot
                        # can be reused to dispatch a package.
                        os.write(fd, token)
            if (name is not None and len(active) < workers and not cancelled() and
                    all(code == SERIAL_DEPS for code in codes.values()) and
                    (not active or has_headroom())):
                try:
                    token = os.read(fd, 1)
                except BlockingIOError:
                    if not active:
                        raise TransactionError('build jobserver has no tokens with no active workers') from None
                    # A child can return an extra token without exiting. Do
                    # not wait indefinitely for either FIFO I/O or worker exit.
                    wait(active, timeout=0.1, return_when=FIRST_COMPLETED)
                else:
                    if not token:
                        raise TransactionError('build jobserver closed while admitting a worker')
                    try:
                        future = pool.submit(launch, name)
                    except BaseException:
                        os.write(fd, token)
                        raise
                    active[future] = (name, token)
                    name = next(pending, None)
            elif active:
                # Under resource pressure wait for a build to finish before
                # admitting a new root; running clients still share the FIFO.
                wait(active, return_when=FIRST_COMPLETED)
            else:
                # No client remains to borrow or return slots. A missing or
                # duplicate token means this batch cannot be trusted to have
                # respected its shared budget.
                if not cancelled():
                    try:
                        remaining = os.read(fd, slots + 1)
                    except BlockingIOError:
                        remaining = b''
                    if remaining != b'+' * slots:
                        raise TransactionError('build jobserver slot inventory changed')
                    os.write(fd, remaining)
                return codes


def order_aur_names(aur_names, local_db="/var/lib/pacman/local"):
    """Order planned AUR rebuilds dependencies-first using the host local DB,
    so a package like howdy-git reuses a freshly sealed python-dlib instead of
    building it twice. Ties and cycles fall back to alphabetical order."""
    names = sorted(set(aur_names))
    if len(names) <= 1:
        return names
    target = set(names)
    deps_of = {n: [] for n in names}
    providers = {n: {n} for n in names}
    local = Path(local_db)
    if local.is_dir():
        for desc in local.glob("*/desc"):
            try:
                lines = desc.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            pkg_name = None
            section = None
            raw_deps = []
            raw_provs = []
            for line in lines:
                if line.startswith("%") and line.endswith("%"):
                    section = line
                    continue
                if not line:
                    section = None
                    continue
                if section == "%NAME%" and pkg_name is None:
                    pkg_name = line.strip()
                elif section == "%DEPENDS%":
                    raw_deps.append(re.split(r"[<>=:]", line.strip(), maxsplit=1)[0])
                elif section == "%PROVIDES%":
                    raw_provs.append(re.split(r"[<>=]", line.strip(), maxsplit=1)[0])
            if pkg_name in target:
                deps_of[pkg_name] = raw_deps
                for prov in raw_provs:
                    providers.setdefault(prov, set()).add(pkg_name)
    need = {n: set() for n in names}
    dependents = {n: set() for n in names}
    for n in names:
        for dep in deps_of[n]:
            for provider in providers.get(dep, ()):
                if provider in target and provider != n:
                    need[n].add(provider)
                    dependents[provider].add(n)
    ordered = []
    ready = sorted(n for n in names if not need[n])
    while ready:
        current = ready.pop(0)
        ordered.append(current)
        for dep_by in sorted(dependents[current]):
            need[dep_by].discard(current)
            if not need[dep_by] and dep_by not in ordered and dep_by not in ready:
                ready.append(dep_by)
                ready.sort()
    for n in names:
        if n not in ordered:
            ordered.append(n)
    return ordered


def build_batch(build_tool, directory, digest, workspace):
    """Entry point from the builder-lock holder; no operation installs on host."""
    def installed_hash():
        installed = subprocess.check_output(['pacman', '-Q'])
        return hashlib.sha256(installed).hexdigest()

    reference = {'directory': directory, 'sha256': digest}
    directory, data = load_snapshot(reference, installed_hash())
    workspace = Path(workspace).absolute()
    local_names = sorted(data.get('recipes', {}), key=lambda name: (name != 'libmupdf', name))
    aur_names = order_aur_names(aur_entries(data))
    names = local_names + aur_names
    if not names:
        raise TransactionError('prepared update has no packages')
    override = os.environ.get('LOCACHY_BUILD_WORKERS')
    active_limit = len(names)
    if override is not None:
        try:
            active_limit = min(active_limit, int(override))
        except ValueError:
            raise TransactionError('LOCACHY_BUILD_WORKERS must be a positive integer') from None
        if active_limit < 1:
            raise TransactionError('LOCACHY_BUILD_WORKERS must be a positive integer')
    slots = build_slot_budget(workspace)
    logs = directory / 'build-logs'
    logs.mkdir(mode=0o700, exist_ok=True)
    print(f'===> prepare: up to {min(slots, active_limit)} package build worker(s), '
          f'{slots} shared job slot(s), separate target roots', flush=True)

    processes = set()
    process_lock = RLock()
    cancelled = False

    def stop(signum, _frame):
        nonlocal cancelled
        cancelled = True
        with process_lock:
            for child in processes:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(child.pid, signal.SIGTERM)

    def run_stage(name, params, jobserver=None, jobs=None, private_home=None):
        run = workspace / name
        if private_home is None:
            private_home = name != 'contracts'
        work = dict(os.environ, LOCACHY_BUILD_STAGE='batch-worker', LOCACHY_BUILD_RUN=str(run),
                    LOCACHY_BUILD_PRIVATE_HOME='1' if private_home else '',
                    LOCACHY_BUILD_REPO_ONLY='1' if jobserver is not None else '0')
        for key in ('LOCACHY_BUILD_CPUS', 'LOCACHY_BUILD_JOBSERVER', 'LOCACHY_BUILD_JOBS'):
            work.pop(key, None)
        if jobserver is not None:
            work['LOCACHY_BUILD_JOBSERVER'] = str(jobserver)
            work['LOCACHY_BUILD_JOBS'] = str(slots)
        elif jobs is not None:
            work['LOCACHY_BUILD_JOBS'] = str(jobs)
        log = logs / (name + '.log')
        started = time.monotonic()
        print(f'===> prepare: {name} start ({time.time():.3f}); log: {log}', flush=True)
        with log.open('a', encoding='utf-8') as output:
            with process_lock:
                if cancelled:
                    return 130
                child = subprocess.Popen([build_tool, *params], env=work, stdin=subprocess.DEVNULL,
                                         stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                processes.add(child)
                if cancelled:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(child.pid, signal.SIGTERM)
            try:
                code = child.wait()
            finally:
                with process_lock:
                    processes.discard(child)
        print(f'===> prepare: {name} {"done" if code == 0 else "stopped"} '
              f'({time.monotonic() - started:.1f} s, exit {code})', flush=True)
        if code:
            with log.open('rb') as stream:
                stream.seek(max(0, log.stat().st_size - 6000))
                print(stream.read().decode(errors='replace').rstrip(), flush=True)
        return code

    def build(name, jobserver=None):
        return run_stage('pkg-' + name, ['--prepare', str(directory), '--local', name],
                         jobserver=jobserver)

    def build_aur(name, jobs=None):
        # Serial by construction: the persistent builder home is shared with
        # no other worker, so no private copy (and no lost cache writeback).
        return run_stage('pkg-' + name, ['--prepare', str(directory), '--aur', name],
                         jobs=jobs, private_home=False)

    def report_keys(failed):
        keys = set()
        for name, code in failed.items():
            if code == 3:
                report = workspace / ('pkg-' + name) / 'out/untrusted-keys'
                if report.is_symlink() or not report.exists() or not stat.S_ISREG(report.lstat().st_mode):
                    raise TransactionError(f'{name}: missing regular signing-key report')
                with report.open(encoding='utf-8') as entries:
                    content = entries.read(64 * 1024 + 1)
                if len(content) > 64 * 1024:
                    raise TransactionError(f'{name}: signing-key report is too large')
                for line in content.splitlines():
                    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9@._+-]* (?:[0-9A-F]{40}|[0-9A-F]{64})', line):
                        raise TransactionError(f'{name}: invalid signing-key report')
                    keys.add(line)
        if not keys:
            return False
        # Only the coordinator writes this legacy notification file.
        path = workspace.parent / 'untrusted-keys'
        fd, tmp = tempfile.mkstemp(dir=workspace.parent, prefix='.untrusted-keys.')
        try:
            with os.fdopen(fd, 'w') as output:
                output.write('\n'.join(sorted(keys)) + '\n')
                os.fchmod(output.fileno(), 0o644)
            os.replace(tmp, path)
        finally:
            Path(tmp).unlink(missing_ok=True)
        for line in sorted(keys):
            print('Source key to trust: ' + line, flush=True)
        fingerprints = sorted({line.split()[1] for line in keys})
        print('Trust them: locachy-build --trust-key ' + ' '.join(fingerprints), flush=True)
        return True

    with contextlib.ExitStack() as cleanup:
        for sig in (signal.SIGINT, signal.SIGTERM):
            cleanup.callback(signal.signal, sig, signal.signal(sig, stop))
        failed = {}
        if local_names:
            with build_jobserver(workspace, slots) as (jobserver, jobserver_fd):
                failed = run_build_workers(
                    local_names, slots, min(active_limit, len(local_names)), jobserver_fd,
                    lambda name: build(name, jobserver),
                    lambda: cancelled, lambda: build_resource_headroom(workspace))
        if cancelled:
            return 130
        if report_keys(failed):
            return 3
        if any(code != SERIAL_DEPS for code in failed.values()):
            return 1
        # Only pre-compilation deferrals are retried, after shared workers
        # drain. Keep the recipe's normal job settings outside the jobserver.
        for name in local_names:
            if name in failed:
                print(f'===> prepare: {name} uses the serial build path', flush=True)
                code = build(name)
                if code or cancelled:
                    return 130 if cancelled else (3 if report_keys({name: code}) else 1)
        for name in aur_names:
            (directory / 'prepared' / name / 'build.json').unlink(missing_ok=True)
        for name in aur_names:
            # AUR builds run one at a time once the local pool has drained. They
            # keep the persistent builder home (pikaur, Go, Cargo and git caches
            # stay warm) and the whole job budget for make, cmake, meson and cargo.
            # The FIFO pool is not used: no AUR build could join it anyway.
            print(f'===> prepare: AUR {name} (serial, {slots} job slot(s), persistent builder home)', flush=True)
            code = build_aur(name, jobs=slots)
            if cancelled:
                return 130
            if code:
                return 3 if report_keys({name: code}) else 1
        (workspace.parent / 'untrusted-keys').unlink(missing_ok=True)
        load_snapshot(reference, installed_hash())
        code = run_stage('contracts', ['--validate-prepared', str(directory)])
        if cancelled:
            return 130
        if code:
            return 1
        load_snapshot(reference, installed_hash())
        verify_combined_results({name: prepared_result(directory, name, data) for name in names})
    return 0


def prepare(reference, build_tool, host_hash, run=subprocess.run):
    directory, data = load_snapshot(reference, host_hash())
    reused = reusable_results(directory, data)
    if reused is not None:
        print("===> prepare: " + ", ".join(sorted(reused)) + ": built, sealed and tested together for this "
              "snapshot already; not built again", flush=True)
        return directory, data, reused
    names = prepared_names(data)
    for name in names:
        # Old results that do not hold for this snapshot cannot survive a failed replacement build.
        result = directory / "prepared" / name / "result.json"
        if result.exists():
            result.unlink()
    _invoke(run, [build_tool, "--prepare-batch", str(directory), reference["sha256"]])
    load_snapshot(reference, host_hash())
    results = {name: prepared_result(directory, name, data) for name in names}
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


def _replace_file(source, target):
    """Copy by rename, keeping source's time: a rewritten repository database
    keeps time 0, so that a plain pacman -Sy downloads it again whole rather
    than pairing it with the mirror's signature (locachy_stack.merge)."""
    temporary = target.with_name(target.name + ".locachy-new")
    shutil.copyfile(source, temporary)
    os.chmod(temporary, 0o644)
    st = os.stat(source)
    os.utime(temporary, ns=(st.st_atime_ns, st.st_mtime_ns))
    os.replace(temporary, target)


def publish_sync(snapshot_sync, host_sync):
    """The snapshot's repository databases as the host's, each replaced whole
    (rename), others left as they are. The snapshot's databases are the stack's
    merged set (locachy_stack): a rewritten one has no signature. So the host's
    old signature goes first, the database follows, and a signature the snapshot
    has comes last; pacman never pairs a database with another's signature."""
    snapshot_sync, host_sync = Path(snapshot_sync), Path(host_sync)
    host_sync.mkdir(parents=True, exist_ok=True)
    for source in sorted(snapshot_sync.iterdir()):
        if not source.is_file() or source.is_symlink() or source.name.endswith(".sig"):
            continue
        signature = snapshot_sync / (source.name + ".sig")
        with contextlib.suppress(FileNotFoundError):
            (host_sync / signature.name).unlink()
        _replace_file(source, host_sync / source.name)
        if signature.is_file() and not signature.is_symlink():
            _replace_file(signature, host_sync / signature.name)


def _stack():
    path = Path(__file__).with_name("locachy_stack.py")
    if not path.is_file():
        path = Path("/usr/local/lib/locachy/locachy_stack.py")
    spec = importlib.util.spec_from_file_location("locachy_stack", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def record_published(host_sync, record=None):
    """Note what was published, so locachy-pacman-guard can tell a plain
    pacman -Sy that replaced it since (locachy_stack.record). record: the
    record file, the system's own (locachy_stack.RECORD) when None."""
    stack = _stack()
    stack.record(str(host_sync), record or stack.RECORD)


def install(reference, directory, data, results, host_hash, db_path="/var/lib/pacman",
            run=subprocess.run, net=lambda c: c, record=None):
    """Prepare has already finished. Hold the real lock until the pacman child
    exits; its alternate local DB is the real one bind-mounted, not a copy.
    The snapshot's databases are then the host's, and recorded as published
    (record_published; record is the record file, the system's when None)."""
    expected = set(prepared_names(data))
    if not results:
        raise TransactionError("prepared update has no validated local packages")
    if set(results) != expected:
        raise TransactionError("package transaction omits a prepared local output")
    state = Path(directory)
    results = {name: prepared_result(directory, name, data) for name in results}
    verify_combined_results(results)
    workspace = Path(tempfile.mkdtemp(prefix="install-", dir=state))
    try:
        repo = workspace / "repo"
        repo.mkdir()
        artifacts = []
        seen_names = {}

        def stage_artifact(pkg_name, source_path, digest):
            if pkg_name in seen_names:
                if seen_names[pkg_name] != digest:
                    raise TransactionError(f"{pkg_name}: conflicting sealed artifact hashes")
                return
            seen_names[pkg_name] = digest
            target = repo / repo_artifact_name(source_path, digest)
            # Copy, not writable hardlinks into a previously validated artifact.
            shutil.copyfile(source_path, target)
            artifacts.append(str(target))

        for name in sorted(results):
            result = prepared_result(directory, name, data)
            stage_artifact(name, result["artifact"], result["sha256"])
        for name in sorted(results):
            for extra in results[name].get("extra_artifacts", ()):
                stage_artifact(extra["name"], extra["artifact"], extra["sha256"])
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
                    if PACKAGE_NAME.fullmatch(name)]
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
            with timed_stage("install: pacman transaction and hooks"):
                _invoke(run, net(with_local(pacman + ["-Su", "--noconfirm", "--ask", "4"] + targets,
                                            real_local, private_db / "local")))
            verify_recorded(selected_manifest, real_local)
            publish_sync(state / "sync", Path(db_path) / "sync")
            record_published(Path(db_path) / "sync", record)
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
