#!/usr/bin/env python3
"""The library overlay (pkgbuilds/libinput), without network, makepkg, mounts or
pacman. Only a synthetic upstream recipe is sourced by bash, never the live
host's; patch(1) is a stand-in.
  python3 -B tools/test-libinput.py SCRATCH_PARENT
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.parse

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("upstream", SOURCE / "link/locachy_upstream.py")
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None

# Synthetic Arch recipe retaining the reviewed patch anchors. No downloaded
# package code is run by this test; staging and compilation are stand-ins.
RECIPE = '''pkgbase=libinput
pkgname=(
  libinput
  libinput-tools
)
pkgver=1.32.0
pkgrel=1
pkgdesc="Input library"
arch=(x86_64)
depends=(
  glibc
  libevdev
  libgcc
  libwacom
  lua54
  mtdev
  systemd-libs
)
makedepends=(
  cairo
  check
  git
  glib2
  gtk4
  libx11
  meson
  python
  python-libevdev
  python-pyudev
  python-yaml
  wayland
  wayland-protocols
)
checkdepends=(python-pytest)
source=(
  'release.tar.gz'
)
sha256sums=('upstream-checksum')
validpgpkeys=('upstream-key')

build() {
  local meson_options=(
    # upstream recommends not building docs
    -D documentation=false
  )

  arch-meson libinput build "${meson_options[@]}"
  meson compile -C build
}

check() {
  printf 'check:%s\\n' "$pkgname" >>"$TEST_LOG"
}
package_libinput() {
  mkdir -p tools/usr/bin tools/usr/lib/libinput "$pkgdir/usr/lib"
  printf library >"$pkgdir/usr/lib/libinput.so.10"
  printf cli >tools/usr/bin/libinput
  printf events >tools/usr/lib/libinput/libinput-debug-events
  printf devices >tools/usr/lib/libinput/libinput-list-devices
}

package_libinput-tools() {
  pkgdesc+=" (debug utilities)"
  depends=(
    cairo
    glib2
    glibc
    gtk4
    libevdev
    libgcc
    libinput
    libx11
    python
    python-libevdev
    python-pyudev
    python-yaml
    systemd-libs
    wayland
  )

  mv tools/* "$pkgdir"

  install -Dm644 libinput/COPYING -t "$pkgdir/usr/share/licenses/$pkgname"
}
'''


class Libinput(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="libinput-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.baseline = self.root / "baseline"
        shutil.copytree(SOURCE / "pkgbuilds/libinput", self.baseline)
        self.state = self.root / "state"
        self.remote = {"PKGBUILD": RECIPE.encode(), ".SRCINFO": self.info()}
        self.urls = []

    def info(self):
        return b'''pkgbase = libinput
\tpkgver = 1.32.0
\tpkgrel = 1
\tpkgdesc = Input library
\tarch = x86_64
\tdepends = libwacom
pkgname = libinput
\tpkgdesc = Library output
\tprovides = libinput.so
pkgname = libinput-tools
\tpkgdesc = CLI and GUI tools
\tdepends = gtk4
\tdepends = libinput
'''

    def fetch(self, url, **kwargs):
        self.urls.append(url)
        self.assertIn("archlinux%2Fpackaging%2Fpackages%2Flibinput", url)
        if "/repository/tags/" in url:
            return json.dumps({"name": "1.32.0-1", "commit": {"id": "a" * 40}}).encode()
        if "/repository/tree?" in url:
            return json.dumps([{"path": n, "type": "blob", "mode": "100644"} for n in self.remote]).encode()
        encoded = url.split("/repository/files/", 1)[1].split("/raw?", 1)[0]
        return self.remote[urllib.parse.unquote(encoded)]

    def candidate(self):
        return u.prepare_candidate("libinput", self.baseline, "extra", "1.32.0-1",
                                   self.state, stable_version="1.32.0-1", fetch=self.fetch)

    def source(self, recipe, script):
        return subprocess.run(["bash", "-euc", script], cwd=recipe,
                              env=dict(os.environ, TEST_LOG=str(self.root / "calls")),
                              text=True, capture_output=True)

    def test_candidate_selects_library_metadata_without_executing_recipe(self):
        self.remote["PKGBUILD"] = b"touch /HOST-MUST-NOT-EXECUTE\n" + self.remote["PKGBUILD"]
        lock_path = self.candidate()
        lock = u.validate_candidate(lock_path, "libinput", self.baseline, self.state)
        self.assertEqual(lock["upstream_pkgbase"], "libinput")
        self.assertEqual(lock["upstream_package"], "libinput")
        self.assertEqual(lock["package_version"], "1:1.32.0-1")
        self.assertEqual(lock["gate"], "libinput-circular-scroll-v2")
        self.assertEqual(sorted(lock["input_hashes"]), ["circular-scroll.patch", "library.patch"])
        wrapper = (lock_path.parent / "recipe/PKGBUILD").read_text()
        self.assertIn("pkgname='libinput'", wrapper)
        self.assertEqual(self.candidate(), lock_path)

    def test_adapter_keeps_build_tests_checksums_patches_source_and_emits_only_the_library(self):
        path = self.candidate().parent / "recipe"
        (path / "src/libinput").mkdir(parents=True)
        (path / "src/libinput/COPYING").write_text("license")
        (path / "pkg/libinput").mkdir(parents=True)
        result = self.source(path, '''
startdir=$PWD
srcdir=$PWD/src
pkgdir=$PWD/pkg/libinput
package_info() { :; }
source ./PKGBUILD
[[ ${pkgname[*]} == libinput && $pkgbase == libinput && $epoch == 1 && $pkgver == 1.32.0 && $pkgrel == 1 ]]
[[ ${depends[*]} == 'glibc libevdev libgcc libwacom lua54 mtdev systemd-libs' ]]
[[ ${makedepends[*]} == 'check git meson python python-libevdev python-pyudev python-yaml' ]]
[[ ${checkdepends[*]} == python-pytest && ${validpgpkeys[*]} == upstream-key ]]
[[ ${#source[@]} == 3 && ${#sha256sums[@]} == 3 && ${sha256sums[0]} == upstream-checksum ]]
[[ ${source[1]} == library.patch && ${source[2]} == circular-scroll.patch ]]
patch() { printf 'patch:%s\\n' "$*" >>"$TEST_LOG"; }
arch-meson() { printf 'meson:%s\\n' "$*" >>"$TEST_LOG"; }
meson() { printf 'compile:%s\\n' "$*" >>"$TEST_LOG"; }
install() { [[ $1 == -Dm644 && $3 == -t ]]; mkdir -p "$4"; cp "$2" "$4/"; }
prepare
build
check
cd "$srcdir"
package_libinput
[[ -f $pkgdir/usr/lib/libinput.so.10 && ! -e $pkgdir/usr/bin/libinput ]]
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        log = (self.root / "calls").read_text()
        self.assertIn("-D debug-gui=false", log)
        self.assertIn("check:libinput", log)
        self.assertIn(f"patch:--batch --forward --fuzz=0 -d {path}/src/libinput -Np1 -i {path}/src/circular-scroll.patch",
                      log)
        for name in ("library.patch", "circular-scroll.patch"):
            digest = hashlib.sha256((path / name).read_bytes()).hexdigest()
            self.assertIn(digest, (path / "PKGBUILD").read_text())
        self.assertEqual(list((path / "pkg").iterdir()), [path / "pkg/libinput"])

    def test_source_patch_carries_the_tap_fix_and_panasonic_quirks(self):
        patch = (self.baseline / "circular-scroll.patch").read_text()
        self.assertIn("touch that has not scrolled yet must keep its zone", patch)
        self.assertIn("+START_TEST(touchpad_circular_scroll_tap_enabled)", patch)
        for model in ("CF-SV8", "CF-FV1"):
            self.assertIn(f"+[Panasonic Let's Note {model} circular trackpad]", patch)

    def test_ring_width_is_unitless_uint_and_dwell_keeps_milliseconds(self):
        patch = (self.baseline / "circular-scroll.patch").read_text()
        added = "\n".join(line[1:] for line in patch.splitlines()
                          if line.startswith("+") and not line.startswith("+++"))
        self.assertIn('return "AttrCircularScrollRingWidth"', added)
        self.assertIn("#define CIRCULAR_SCROLL_RING_WIDTH 25", added)
        self.assertIn("quirks_get_uint32(q, QUIRK_ATTR_CIRCULAR_SCROLL_RING_WIDTH, &width)", added)
        self.assertIn("v < 1 || v > 50", added)
        self.assertIn("radius * (100 - width) / 100", added)
        self.assertIn("usec_from_millis(dwell)", added)
        self.assertNotIn("AttrCircularScrollWidth", added)
        self.assertNotIn("7.5mm", added)
        self.assertNotIn("ring_mm", added)

    def test_v2_contract_is_explicit_about_required_vm_behavior_proof(self):
        contract = (SOURCE / "tools/test-libinput.sh").read_text()
        self.assertIn("'id': 'libinput-circular-scroll-v2'", contract)
        self.assertIn("'scope': 'installed-library-smoke'", contract)
        self.assertIn("'release_tests_required': ['upstream-circular-litest', 'pointer-libinput-vm']", contract)
        instructions = (SOURCE / "pkgbuilds/libinput/BEHAVIOR.md").read_text()
        self.assertIn("--filter-test='touchpad_circular_scroll_*'", instructions)
        self.assertIn("test-pointer-libinput.sh", instructions)
        self.assertIn("smoke result alone is insufficient", instructions)

    def test_changed_patch_anchor_or_upstream_prepare_fail_closed(self):
        for recipe, expected in ((RECIPE.replace("-D documentation=false", "-D documentation=true"), "failed"),
                                 (RECIPE.replace("build() {", "prepare() {\n  :\n}\n\nbuild() {"), "prepare()")):
            self.remote["PKGBUILD"] = recipe.encode()
            result = self.source(self.candidate().parent / "recipe", "startdir=$PWD; source ./PKGBUILD")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected, result.stderr.lower())

    def test_unknown_split_layout_is_rejected(self):
        self.remote[".SRCINFO"] = self.info() + b"pkgname = extra-tools\n"
        with self.assertRaisesRegex(u.UpstreamError, "split"):
            self.candidate()

    def test_config_cannot_redirect_source_authority_or_output(self):
        config = self.baseline / "upstream.json"
        original = json.loads(config.read_text())
        for key, value in (("repository", "https://example.invalid/libinput.git"),
                           ("package", "libinput-tools"), ("gate", "libinput-tools-cli-v1"),
                           ("inputs", ["circular-scroll.patch"])):
            config.write_text(json.dumps(dict(original, **{key: value})))
            with self.assertRaises(u.UpstreamError):
                self.candidate()
        self.assertEqual(self.urls, [])


if __name__ == "__main__":
    unittest.main()
