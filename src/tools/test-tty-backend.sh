#!/bin/bash
# Unit and real-PTY integration checks for the SSH/TTY terminal backend.
# Usage: test-tty-backend.sh [BUILD_CHECK_OUT_DIR]
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bin_dir=${1:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass() { printf 'PASS %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1" >&2; exit 1; }

zlib_cflags=
zlib_libs=
if pkg-config --exists zlib; then
    zlib_cflags="-DCONFIG_ZLIB $(pkg-config --cflags zlib)"
    zlib_libs="$(pkg-config --libs zlib)"
fi

gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_TTY $zlib_cflags \
    -I"$src/base/include" -I"$src/base" \
    "$src/tools/test-tty-backend.c" "$src/base/tty-backend.c" \
    "$src/base/say.c" "$src/base/key-action.c" \
    $zlib_libs -lm -o "$tmp/test-tty-backend"
"$tmp/test-tty-backend"

# Syntax and argument validation for reboot/poweroff helpers
sh -n "$src/link/reboot" && sh -n "$src/link/poweroff" \
    && pass 'reboot and poweroff shell syntax valid' || fail 'reboot/poweroff syntax'

# If candidate binaries from build-check.sh are provided, test real programs on a PTY.
if [ -n "$bin_dir" ] && [ -x "$bin_dir/settings" ]; then
    # Non-TTY stdin/stdout with explicit LOCACHY_BACKEND=tty must fail cleanly (69), never touching DRM.
    status=0
    timeout 10 env LOCACHY_BACKEND=tty LOCACHY_NO_SUDO=1 "$bin_dir/settings" </dev/null >/dev/null 2>&1 || status=$?
    [ "$status" = 69 ] && pass 'LOCACHY_BACKEND=tty on non-TTY exits 69 without DRM fallback' \
        || fail "LOCACHY_BACKEND=tty on non-TTY exited $status (expected 69)"

    # An explicit LOCACHY_BACKEND never re-runs a program under sudo (sudo would drop it): still 69.
    for prog in settings packages firmware; do
        status=0
        timeout 10 env -u LOCACHY_NO_SUDO LOCACHY_BACKEND=tty "$bin_dir/$prog" </dev/null >/dev/null 2>&1 || status=$?
        [ "$status" = 69 ] && pass "LOCACHY_BACKEND=tty $prog without LOCACHY_NO_SUDO exits 69 (no sudo)" \
            || fail "LOCACHY_BACKEND=tty $prog without LOCACHY_NO_SUDO exited $status (expected 69)"
    done

    # Unknown LOCACHY_BACKEND must exit 78.
    status=0
    timeout 10 env LOCACHY_BACKEND=unknown-backend LOCACHY_NO_SUDO=1 "$bin_dir/settings" </dev/null >/dev/null 2>&1 || status=$?
    [ "$status" = 78 ] && pass 'unknown LOCACHY_BACKEND exits 78' \
        || fail "unknown LOCACHY_BACKEND exited $status (expected 78)"

    # If Terminus PSF fonts are not installed at /usr/local/share/locachy yet (dev host),
    # stage them in a private mount namespace so real pixel-framebuffer runs can load BASE_FONT.
    pty_runner=(python3 -)
    if [ ! -r /usr/local/share/locachy/font.psf ] && [ -r /usr/share/kbd/consolefonts/ter-u16b.psf.gz ]; then
        mkdir -p "$tmp/share/locachy/terminus"
        gzip -dc /usr/share/kbd/consolefonts/ter-u16b.psf.gz >"$tmp/share/locachy/font.psf"
        for f in /usr/share/kbd/consolefonts/ter-u*.psf.gz; do
            [ -r "$f" ] || continue
            b=$(basename "$f" .gz)
            gzip -dc "$f" >"$tmp/share/locachy/terminus/$b"
        done
        if unshare --user --map-root-user --mount true 2>/dev/null; then
            pty_runner=(unshare --user --map-root-user --mount sh -c 'mount --bind "$1" /usr/local/share && exec python3 - "$2"' _ "$tmp/share")
        fi
    fi

    # Real PTY runs: settings, packages, firmware across cell, Kitty pixel-fb, and Sixel pixel-fb modes.
    "${pty_runner[@]}" "$bin_dir" <<'PY'
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

bin_dir = sys.argv[1]


def run_pty(prog, mode, expect_marker, label, send_px=None):
    path = os.path.join(bin_dir, prog)
    if not os.path.isfile(path):
        return
    if send_px is None:
        send_px = mode in ("kitty", "sixel")
    master, slave = pty.openpty()
    winsz = struct.pack("HHHH", 30, 100, 1000 if send_px else 0, 540 if send_px else 0)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, winsz)

    env = os.environ.copy()
    env.pop("LOCACHY_CONTROL", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["LOCACHY_BACKEND"] = "tty"
    env["LOCACHY_TTY_MODE"] = mode
    env["LOCACHY_NO_SUDO"] = "1"
    env["TERM"] = "xterm-256color"
    env["COLORTERM"] = "truecolor"
    env["LANG"] = "C.UTF-8"

    proc = subprocess.Popen(
        [path],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=env,
        close_fds=True,
    )
    os.close(slave)

    out = bytearray()
    replied_probe = False
    deadline = time.monotonic() + 8.0
    sent_resize = False
    checked_fds = False

    while time.monotonic() < deadline:
        r, _, _ = select.select([master], [], [], 0.1)
        if r:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            out.extend(chunk)
            # Answer in-band capability probe immediately so startup takes milliseconds.
            if not replied_probe and b"\x1b[c" in out:
                replied_probe = True
                if mode == "kitty":
                    os.write(master, b"\x1b_Gi=31;OK\x1b\\\x1b[?0u\x1b[?62;22c")
                elif mode == "sixel":
                    os.write(master, b"\x1b[?62;4;22c")
                else:
                    os.write(master, b"\x1b[?1;2c")

        if not checked_fds and b"\x1b[?1049h" in out and expect_marker in out:
            checked_fds = True
            # Forbidden: input devices, and any writable DRM open (DRM master or modeset).
            # A brief O_RDONLY /dev/dri/card* open is the connector probe behind settings' display list.
            fd_dir = f"/proc/{proc.pid}/fd"
            try:
                entries = os.listdir(fd_dir)
            except FileNotFoundError:
                entries = []
            for entry in entries:
                bad = None
                try:
                    target = os.readlink(os.path.join(fd_dir, entry))
                    if target.startswith("/dev/input/"):
                        bad = target
                    elif target.startswith("/dev/dri/"):
                        with open(f"/proc/{proc.pid}/fdinfo/{entry}") as fi:
                            flags = next(int(line.split()[1], 8) for line in fi if line.startswith("flags:"))
                        if flags & 3:
                            bad = target
                except (FileNotFoundError, StopIteration):
                    pass  # closed while we looked
                if bad:
                    proc.kill()
                    raise AssertionError(f"{label}: opened forbidden device {bad}")
            # Trigger a live resize via TIOCSWINSZ + SIGWINCH.
            if not sent_resize:
                sent_resize = True
                ws2 = struct.pack("HHHH", 32, 110, 1100 if send_px else 0, 576 if send_px else 0)
                fcntl.ioctl(master, termios.TIOCSWINSZ, ws2)
                proc.send_signal(signal.SIGWINCH)
                time.sleep(0.15)
                # Send Kitty CSI u Ctrl+Q when kitty keyboard protocol is active, else raw Ctrl+Q.
                os.write(master, b"\x1b[113;5u" if mode == "kitty" else b"\x11")

        if proc.poll() is not None:
            # Drain remaining teardown output.
            while True:
                r2, _, _ = select.select([master], [], [], 0.05)
                if not r2:
                    break
                try:
                    c2 = os.read(master, 65536)
                    if not c2:
                        break
                    out.extend(c2)
                except OSError:
                    break
            break

    os.close(master)
    if proc.poll() is None:
        proc.kill()
        raise AssertionError(f"{label}: timed out waiting for exit (len={len(out)}, tail={bytes(out[-300:])!r})")
    if proc.returncode != 0:
        raise AssertionError(f"{label}: exited {proc.returncode} (tail={bytes(out[-300:])!r})")
    if b"\x1b[?1049h" not in out or b"\x1b[?1049l" not in out:
        raise AssertionError(f"{label}: missing alt-screen enter/leave sequences")
    if expect_marker not in out:
        raise AssertionError(f"{label}: missing expected render marker {expect_marker!r}")
    # Backend diagnostics go to stderr, which is this terminal: none may show on screen.
    for needle in (b"tty: ", b"screen: "):
        if needle in out:
            at = out.index(needle)
            raise AssertionError(f"{label}: backend diagnostics reached the terminal: {bytes(out[at:at + 80])!r}")
    print(f"PASS {label}")


run_pty("settings", "cell", b"LOCachy", "real PTY: settings in ANSI cell mode + resize + Ctrl+Q + zero DRM/evdev fds")
run_pty("settings", "kitty", b"\x1b_Ga=T,f=24,", "real PTY: settings in Kitty pixel framebuffer mode + resize + Ctrl+Q + zero DRM/evdev fds")
run_pty("settings", "sixel", b"\x1bP0;1;0q", "real PTY: settings in Sixel pixel framebuffer mode + resize + Ctrl+Q + zero DRM/evdev fds")
run_pty("settings", "kitty", b"LOCachy", "real PTY: settings Kitty mode with 0x0 px gracefully falls back to cell mode", send_px=False)
run_pty("packages", "cell", b"packages", "real PTY: packages in ANSI cell mode + resize + Ctrl+Q + zero DRM/evdev fds")
run_pty("packages", "kitty", b"\x1b_Ga=T,f=24,", "real PTY: packages in Kitty pixel framebuffer mode + resize + Ctrl+Q + zero DRM/evdev fds")
run_pty("firmware", "cell", b"firmware", "real PTY: firmware in ANSI cell mode + resize + Ctrl+Q + zero DRM/evdev fds")
run_pty("home", "cell", b"LOCachy", "real PTY: home in ANSI cell mode + resize + Ctrl+Q + zero DRM/evdev fds")
PY
fi

pass 'test-tty-backend.sh completed'
