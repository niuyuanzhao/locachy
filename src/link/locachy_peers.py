#!/usr/bin/env python3
"""Shared once-forever peer pairing store for LOCachy (extdisplay & compute).

All long-term peer credentials live in a root-only directory
(default ``/etc/locachy/peers``; ``0700`` dir, ``0600`` files):

  host.json         stable 128-bit ``host_id``, host TLS cert/key paths
  host-cert.pem     host self-signed ECDSA P-256 certificate (compute mTLS)
  host-key.pem      host private key (0600)
  peers/<id>.json   one file per paired remote device, with:
                    - ``peer_id``, ``name``, ``created_at``, ``scopes``
                    - ``extdisplay_psk_hex`` (16 random bytes = 32 hex)
                    - ``extdisplay_identity`` (``locachy-peer-<peer_id>``)
                    - ``client_cert_sha256``, ``client_cert_pem`` (optional
                      for compute mTLS; created on ``add`` unless imported)

Default state is OFF (no peers exist until explicitly added by the user).
Peers stay trusted until explicitly removed via ``revoke`` / ``remove``.
"""
from __future__ import annotations

import base64
import datetime
import fcntl
import hashlib
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterable

DEFAULT_PEERS_DIR = "/etc/locachy/peers"
VALID_SCOPES = ("extdisplay", "compute")
PEER_ID_RE = re.compile(r"^[0-9a-f]{16,32}$")
HOST_ID_RE = re.compile(r"^[0-9a-f]{16,32}$")
HEX32_RE = re.compile(r"^[0-9a-f]{32}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
SAFE_NAME_RE = re.compile(r"^[\x20-\x7e]{1,64}$")
B32_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"


class PeerStoreError(RuntimeError):
    """Raised when the peer store or requested operation is invalid."""


def peers_dir_path(base: str | os.PathLike[str] | None = None) -> Path:
    raw = str(base) if base is not None else os.environ.get("LOCACHY_PEERS_DIR", DEFAULT_PEERS_DIR)
    if not raw or not os.path.isabs(raw):
        raise PeerStoreError(f"peers directory must be an absolute path: {raw!r}")
    return Path(raw)


def _utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def encode_pairing_code(key: bytes) -> str:
    """Encode 16 bytes into LOCachy's canonical 26-symbol Base32 code."""
    if len(key) != 16:
        raise ValueError("extdisplay PSK must be exactly 16 bytes")
    buf = 0
    bits = 0
    symbols: list[str] = []
    for byte in key:
        buf = (buf << 8) | byte
        bits += 8
        while bits >= 5:
            symbols.append(B32_ALPHABET[(buf >> (bits - 5)) & 31])
            bits -= 5
            buf &= (1 << bits) - 1
    if bits > 0:
        symbols.append(B32_ALPHABET[(buf << (5 - bits)) & 31])
    assert len(symbols) == 26
    groups = ["".join(symbols[i : i + 4]) for i in range(0, 26, 4)]
    return "-".join(groups)


def decode_pairing_code(text: str) -> bytes:
    """Decode a canonical 26-symbol LOCachy Base32 code into 16 bytes."""
    acc = 0
    pending = 0
    symbols = 0
    out = bytearray()
    for ch in text:
        if ch in "- \t\r\n":
            continue
        up = ch.upper()
        idx = B32_ALPHABET.find(up)
        if idx < 0:
            raise ValueError("invalid character in pairing code")
        symbols += 1
        if symbols > 26:
            raise ValueError("pairing code too long")
        acc = (acc << 5) | idx
        pending += 5
        if pending >= 8:
            pending -= 8
            out.append((acc >> pending) & 0xFF)
            acc &= (1 << pending) - 1
    if symbols != 26 or len(out) != 16 or pending != 2 or acc != 0:
        raise ValueError("invalid or non-canonical pairing code")
    return bytes(out)


def _fsync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _ensure_private_dir(path: Path) -> None:
    if path.exists():
        st = os.lstat(path)
        if not stat.S_ISDIR(st.st_mode):
            raise PeerStoreError(f"not a directory: {path}")
        if st.st_uid != os.geteuid():
            raise PeerStoreError(f"directory not owned by uid {os.geteuid()}: {path}")
        if st.st_mode & 0o077:
            os.chmod(path, 0o700)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        os.mkdir(path, 0o700)
        os.chmod(path, 0o700)
        _fsync_dir(path.parent)


def _atomic_write_private(path: Path, data: bytes, mode: int = 0o600) -> None:
    _ensure_private_dir(path.parent)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_name, path)
        tmp_name = ""
        _fsync_dir(path.parent)
    finally:
        if tmp_name:
            try:
                os.unlink(tmp_name)
            except OSError:
                pass


def _lock_store(base: Path):
    _ensure_private_dir(base)
    lock_path = base / ".lock"
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    os.fchmod(fd, 0o600)
    fcntl.flock(fd, fcntl.LOCK_EX)
    return fd


def _run_openssl(args: list[str], stdin_bytes: bytes | None = None) -> bytes:
    openssl = shutil.which("openssl")
    if not openssl:
        raise PeerStoreError("openssl binary is required for peer certificates")
    proc = subprocess.run(
        [openssl, *args],
        input=stdin_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        raise PeerStoreError(f"openssl {' '.join(args[:2])} failed: {err}")
    return proc.stdout


def cert_fingerprint_sha256(cert_pem: str | bytes) -> str:
    """Return lowercase 64-char SHA-256 hex digest of an X.509 certificate DER."""
    raw = cert_pem.encode("utf-8") if isinstance(cert_pem, str) else cert_pem
    der = _run_openssl(["x509", "-inform", "PEM", "-outform", "DER"], stdin_bytes=raw)
    return hashlib.sha256(der).hexdigest()


def generate_self_signed_cert(common_name: str, days: int = 7300) -> tuple[str, str, str]:
    """Generate a self-signed ECDSA P-256 certificate & private key.

    Returns ``(cert_pem, key_pem, sha256_hex)``.
    """
    if not re.match(r"^[A-Za-z0-9._-]{1,64}$", common_name):
        raise PeerStoreError(f"invalid certificate CN: {common_name!r}")
    with tempfile.TemporaryDirectory(prefix="locachy-cert-") as tmp:
        os.chmod(tmp, 0o700)
        key_path = Path(tmp) / "key.pem"
        cert_path = Path(tmp) / "cert.pem"
        _run_openssl(
            [
                "req",
                "-x509",
                "-newkey",
                "ec",
                "-pkeyopt",
                "ec_paramgen_curve:prime256v1",
                "-nodes",
                "-days",
                str(days),
                "-subj",
                f"/CN={common_name}",
                "-keyout",
                str(key_path),
                "-out",
                str(cert_path),
            ]
        )
        cert_pem = cert_path.read_text(encoding="utf-8")
        key_pem = key_path.read_text(encoding="utf-8")
        fp = cert_fingerprint_sha256(cert_pem)
        return cert_pem, key_pem, fp


def _validate_scopes(scopes: Iterable[str]) -> list[str]:
    clean: list[str] = []
    for item in scopes:
        for part in str(item).split(","):
            s = part.strip().lower()
            if not s:
                continue
            if s not in VALID_SCOPES:
                raise PeerStoreError(f"unknown scope {s!r}; expected one of {', '.join(VALID_SCOPES)}")
            if s not in clean:
                clean.append(s)
    if not clean:
        raise PeerStoreError("at least one scope (extdisplay, compute) is required")
    return clean


def _validate_name(name: str) -> str:
    cleaned = name.strip()
    if not cleaned or not SAFE_NAME_RE.match(cleaned):
        raise PeerStoreError("peer name must be 1..64 printable ASCII characters")
    return cleaned


def _init_host_unlocked(base: Path) -> dict[str, Any]:
    _ensure_private_dir(base)
    _ensure_private_dir(base / "peers")
    host_json = base / "host.json"
    cert_path = base / "host-cert.pem"
    key_path = base / "host-key.pem"
    if host_json.is_file() and cert_path.is_file() and key_path.is_file():
        data = json.loads(host_json.read_text(encoding="utf-8"))
        if isinstance(data, dict) and HOST_ID_RE.match(str(data.get("host_id", ""))):
            return data
    host_id = secrets.token_hex(16)
    cert_pem, key_pem, fp = generate_self_signed_cert(f"locachy-{host_id[:16]}")
    _atomic_write_private(cert_path, cert_pem.encode("utf-8"), 0o600)
    _atomic_write_private(key_path, key_pem.encode("utf-8"), 0o600)
    data = {
        "version": 1,
        "host_id": host_id,
        "created_at": _utc_now(),
        "host_cert": str(cert_path),
        "host_key": str(key_path),
        "host_cert_sha256": fp,
    }
    _atomic_write_private(host_json, (json.dumps(data, indent=2, sort_keys=True) + "\n").encode("utf-8"), 0o600)
    return data


def init_host(base: str | os.PathLike[str] | None = None) -> dict[str, Any]:
    """Ensure host identity & TLS keypair exist under the peer store."""
    root = peers_dir_path(base)
    lock_fd = _lock_store(root)
    try:
        return _init_host_unlocked(root)
    finally:
        os.close(lock_fd)


def load_host(base: str | os.PathLike[str] | None = None) -> dict[str, Any] | None:
    """Read host.json if initialized; returns None if not yet initialized."""
    root = peers_dir_path(base)
    host_json = root / "host.json"
    if not host_json.is_file():
        return None
    data = json.loads(host_json.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not HOST_ID_RE.match(str(data.get("host_id", ""))):
        raise PeerStoreError(f"corrupt host record: {host_json}")
    return data


def _validate_peer_record(rec: Any) -> dict[str, Any] | None:
    if not isinstance(rec, dict):
        return None
    if rec.get("version") != 1:
        return None
    peer_id = str(rec.get("peer_id", ""))
    if not PEER_ID_RE.match(peer_id):
        return None
    name = str(rec.get("name", ""))
    if not SAFE_NAME_RE.match(name):
        return None
    scopes = rec.get("scopes")
    if not isinstance(scopes, list) or not scopes or any(s not in VALID_SCOPES for s in scopes):
        return None
    psk_hex = str(rec.get("extdisplay_psk_hex", ""))
    if psk_hex and not HEX32_RE.match(psk_hex):
        return None
    ident = str(rec.get("extdisplay_identity", ""))
    if ident and ident != f"locachy-peer-{peer_id}":
        return None
    cert_fp = str(rec.get("client_cert_sha256", ""))
    if cert_fp and not HEX64_RE.match(cert_fp):
        return None
    return rec


def list_peers(base: str | os.PathLike[str] | None = None, scope: str | None = None) -> list[dict[str, Any]]:
    """Return all valid paired peer records, optionally filtered by scope."""
    root = peers_dir_path(base)
    pdir = root / "peers"
    if not pdir.is_dir():
        return []
    out: list[dict[str, Any]] = []
    for entry in sorted(pdir.iterdir()):
        if not entry.name.endswith(".json") or entry.name.startswith("."):
            continue
        try:
            st = os.lstat(entry)
            if not stat.S_ISREG(st.st_mode) or (st.st_mode & 0o077):
                continue
            raw = json.loads(entry.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        rec = _validate_peer_record(raw)
        if rec is None:
            continue
        if scope and scope not in rec["scopes"]:
            continue
        out.append(rec)
    return out


def get_peer(peer_id_or_name: str, base: str | os.PathLike[str] | None = None) -> dict[str, Any] | None:
    """Find a peer by exact peer_id or unique name."""
    peers = list_peers(base)
    for rec in peers:
        if rec["peer_id"] == peer_id_or_name:
            return rec
    matches = [rec for rec in peers if rec["name"] == peer_id_or_name]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise PeerStoreError(f"peer name {peer_id_or_name!r} is ambiguous; specify peer_id")
    return None


def add_peer(
    name: str,
    scopes: Iterable[str] = ("extdisplay", "compute"),
    *,
    peer_id: str | None = None,
    extdisplay_psk_hex: str | None = None,
    client_cert_pem: str | None = None,
    generate_client_cert: bool = True,
    base: str | os.PathLike[str] | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Create and persist a paired peer record.

    Returns ``(stored_peer_record, export_bundle)``. The ``export_bundle``
    contains the client private key (when generated on-host) and host cert so
    the user can hand it once to the remote device; the client private key is
    NOT retained on the LOCachy host.
    """
    clean_name = _validate_name(name)
    clean_scopes = _validate_scopes(scopes)
    if peer_id is not None:
        if not PEER_ID_RE.match(peer_id):
            raise PeerStoreError("peer_id must be 16..32 lowercase hex characters")
        pid = peer_id
    else:
        pid = secrets.token_hex(8)

    if extdisplay_psk_hex is not None:
        psk_hex = extdisplay_psk_hex.lower()
        if not HEX32_RE.match(psk_hex):
            raise PeerStoreError("extdisplay_psk_hex must be 32 hex characters (16 bytes)")
    else:
        psk_hex = secrets.token_hex(16)

    identity = f"locachy-peer-{pid}"
    client_key_pem: str | None = None
    cert_pem_final = ""
    cert_fp = ""

    if client_cert_pem:
        cert_fp = cert_fingerprint_sha256(client_cert_pem)
        cert_pem_final = client_cert_pem.strip() + "\n"
    elif "compute" in clean_scopes and generate_client_cert:
        cert_pem_final, client_key_pem, cert_fp = generate_self_signed_cert(f"locachy-peer-{pid}")

    root = peers_dir_path(base)
    lock_fd = _lock_store(root)
    try:
        host = _init_host_unlocked(root)
        target = root / "peers" / f"{pid}.json"
        if target.exists():
            raise PeerStoreError(f"peer {pid} already exists")
        record: dict[str, Any] = {
            "version": 1,
            "peer_id": pid,
            "name": clean_name,
            "created_at": _utc_now(),
            "scopes": clean_scopes,
            "extdisplay_psk_hex": psk_hex,
            "extdisplay_identity": identity,
            "client_cert_sha256": cert_fp,
            "client_cert_pem": cert_pem_final,
        }
        payload = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8")
        _atomic_write_private(target, payload, 0o600)
        host_cert_pem = Path(host["host_cert"]).read_text(encoding="utf-8")
    finally:
        os.close(lock_fd)

    bundle: dict[str, Any] = {
        "version": 1,
        "host_id": host["host_id"],
        "host_cert_sha256": host["host_cert_sha256"],
        "host_cert_pem": host_cert_pem,
        "peer_id": pid,
        "name": clean_name,
        "scopes": clean_scopes,
        "extdisplay_identity": identity,
        "extdisplay_psk_hex": psk_hex,
        "extdisplay_code": encode_pairing_code(bytes.fromhex(psk_hex)),
        "client_cert_sha256": cert_fp,
        "client_cert_pem": cert_pem_final,
    }
    if client_key_pem is not None:
        bundle["client_key_pem"] = client_key_pem
    return record, bundle


def revoke_peer(peer_id_or_name: str, base: str | os.PathLike[str] | None = None) -> dict[str, Any]:
    """Permanently delete a paired peer's credential file."""
    root = peers_dir_path(base)
    lock_fd = _lock_store(root)
    try:
        rec = get_peer(peer_id_or_name, root)
        if rec is None:
            raise PeerStoreError(f"paired peer not found: {peer_id_or_name}")
        target = root / "peers" / f"{rec['peer_id']}.json"
        if target.exists():
            target.unlink()
            _fsync_dir(target.parent)
        return rec
    finally:
        os.close(lock_fd)


def encode_extdisplay_uri(bundle: dict[str, Any]) -> str:
    """Encode a compact ``locachy-pair://v1?...`` URI for macOS/extdisplay pairing."""
    from urllib.parse import urlencode

    host_id = str(bundle.get("host_id", ""))
    identity = str(bundle.get("extdisplay_identity", ""))
    code = str(bundle.get("extdisplay_code", ""))
    if not code and bundle.get("extdisplay_psk_hex"):
        code = encode_pairing_code(bytes.fromhex(str(bundle["extdisplay_psk_hex"])))
    if not host_id or not identity or not code:
        raise PeerStoreError("bundle is missing extdisplay pairing fields")
    params: list[tuple[str, str]] = [
        ("host", host_id),
        ("id", identity),
        ("code", code),
    ]
    label = str(bundle.get("host_name") or bundle.get("name") or "").strip()
    if label:
        params.append(("name", label))
    return "locachy-pair://v1?" + urlencode(params)


def encode_bundle_token(bundle: dict[str, Any]) -> str:
    """Encode an export bundle as a single copy-pasteable base64url token."""
    raw = json.dumps(bundle, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return "locachy-pair-v1:" + base64.urlsafe_b64encode(raw).decode("ascii")


def decode_bundle_token(token: str) -> dict[str, Any]:
    """Decode a ``locachy-pair-v1:...`` bundle token or ``locachy-pair://v1?...`` URI."""
    text = token.strip()
    if text.lower().startswith("locachy-pair://v1?"):
        from urllib.parse import parse_qsl, urlsplit

        parts = urlsplit(text)
        params = dict(parse_qsl(parts.query, strict_parsing=True))
        host_id = params.get("host") or params.get("hostid") or ""
        identity = params.get("id") or params.get("identity") or ""
        code = params.get("code") or params.get("psk") or ""
        if not host_id or not identity or not code:
            raise PeerStoreError("incomplete locachy-pair URI")
        psk_hex = decode_pairing_code(code).hex()
        return {
            "version": 1,
            "host_id": host_id,
            "name": params.get("name") or host_id,
            "scopes": ["extdisplay"],
            "extdisplay_identity": identity,
            "extdisplay_psk_hex": psk_hex,
            "extdisplay_code": code,
        }
    prefix = "locachy-pair-v1:"
    if not text.startswith(prefix):
        raise PeerStoreError("invalid pairing token prefix")
    try:
        data = base64.urlsafe_b64decode(text[len(prefix) :].encode("ascii"))
        parsed = json.loads(data.decode("utf-8"))
    except Exception as exc:
        raise PeerStoreError(f"cannot decode pairing token: {exc}") from exc
    if not isinstance(parsed, dict) or parsed.get("version") != 1:
        raise PeerStoreError("unsupported pairing token version")
    return parsed


def verify_compute_peer_cert(cert_der: bytes, base: str | os.PathLike[str] | None = None) -> dict[str, Any] | None:
    """Return the matching paired peer record if ``cert_der`` is authorized for compute."""
    if not cert_der:
        return None
    fp = hashlib.sha256(cert_der).hexdigest()
    for peer in list_peers(base, scope="compute"):
        if peer.get("client_cert_sha256") == fp:
            return peer
    return None
