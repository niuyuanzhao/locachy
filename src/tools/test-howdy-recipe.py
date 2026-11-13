#!/usr/bin/env python3
"""The temporary Howdy AUR recipe patch, without a network or package install.

Only the upstream functions relevant to the split Meson projects are fixtures;
there is no copied replacement PKGBUILD. The pipeline is the real sed script,
then trusted fixture functions with recording Meson commands and real install.
  python3 -B tools/test-howdy-recipe.py [SCRATCH_PARENT]
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
PATCH = SOURCE / "aur-patches/howdy-git.sed"
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None

HEADER = '''# Fixture metadata must not be rewritten by a build-path patch.
_pkgbase=howdy
pkgname="$_pkgbase-git"
pkgver=r592.d3ab993
pkgrel=1
source=('howdy::git+https://example.invalid/howdy.git' 'model.dat.bz2')
md5sums=('SKIP' 'fixture-checksum')
backup=('etc/howdy/config.ini')
'''

FUNCTIONS = '''
prepare() {
\tcd "$srcdir"
\tmeson setup --reconfigure "$_pkgbase" "$_pkgbase/build"
}

build() {
\tcd "$srcdir"
\tarch-meson "$_pkgbase" build
\tmeson compile -C build
}

package() {
\tcd "$srcdir"
\tmeson install -C build --destdir "$pkgdir"
\tinstall -Dm644 "$_pkgbase/howdy/src/config.ini" "$pkgdir/etc/howdy/config.ini"
\tinstall -Dm644 "$_pkgbase/LICENSE" "$pkgdir/usr/share/licenses/$_pkgbase/LICENSE"

\tcd "$srcdir"
\tinstall -Dm644 *.dat "$pkgdir/usr/share/dlib-data"

\t# Upstream's polkit camera access workaround is not part of this patch.
\tinstall -Dm644 polkit-agent-helper-howdy.conf \\
\t\t"$pkgdir/usr/lib/systemd/system/polkit-agent-helper@.service.d/10-howdy.conf"
}
'''


class HowdyRecipe(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="howdy-recipe-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.recipe = self.root / "PKGBUILD"

    def apply(self, text):
        self.recipe.write_text(text, encoding="utf-8")
        result = subprocess.run(["sed", "-f", str(PATCH), str(self.recipe)],
                                text=True, capture_output=True, check=True)
        return result.stdout

    def test_only_split_build_paths_change_metadata_and_install_tail_stay(self):
        original = HEADER + FUNCTIONS
        patched = self.apply(original)
        expected = original.replace(
            '\tmeson setup --reconfigure "$_pkgbase" "$_pkgbase/build"',
            '\t: # Both projects are configured in build(), with Arch packaging defaults.')
        expected = expected.replace('\tarch-meson "$_pkgbase" build\n\tmeson compile -C build',
                                    '\tarch-meson "$_pkgbase/howdy" build\n\tmeson compile -C build\n'
                                    '\tarch-meson "$_pkgbase/howdy-gtk" build-gtk\n'
                                    '\tmeson compile -C build-gtk')
        expected = expected.replace('\tmeson install -C build --destdir "$pkgdir"',
                                    '\tmeson install -C build --destdir "$pkgdir"\n'
                                    '\tmeson install -C build-gtk --destdir "$pkgdir"')
        self.assertEqual(patched, expected)
        self.assertTrue(patched.startswith(HEADER))
        self.recipe.write_text(patched)
        subprocess.run(["bash", "-n", str(self.recipe)], check=True)

    def test_upstream_fixed_is_unchanged_for_the_builders_review_guard(self):
        fixed = self.apply(HEADER + FUNCTIONS)
        self.assertEqual(self.apply(fixed), fixed)
        builder = (SOURCE / "link/locachy-build").read_text()
        self.assertIn('if cmp -s "$dir/PKGBUILD" "$dir/PKGBUILD.aur"; then', builder)
        self.assertIn("no longer changes the AUR's PKGBUILD", builder)

    def test_drift_never_applies_only_part_of_the_recipe_patch(self):
        changes = {
            "prepare options": ('meson setup --reconfigure "$_pkgbase" "$_pkgbase/build"',
                                'meson setup --reconfigure -Dchanged=true "$_pkgbase" "$_pkgbase/build"'),
            "source layout": ('arch-meson "$_pkgbase" build', 'arch-meson "$_pkgbase/new-layout" build'),
            "build directory": ('meson compile -C build', 'meson compile -C changed'),
            "package options": ('meson install -C build --destdir "$pkgdir"',
                                'meson install --no-rebuild -C build --destdir "$pkgdir"'),
            "missing prepare": ('prepare() {', 'prepare_changed() {'),
        }
        for label, (old, new) in changes.items():
            with self.subTest(label=label):
                drift = HEADER + FUNCTIONS.replace(old, new)
                self.assertEqual(self.apply(drift), drift)

    def test_both_meson_projects_and_original_payload_are_packaged(self):
        patched = self.apply(HEADER + FUNCTIONS)
        self.recipe.write_text(patched)
        source = self.root / "src"
        core = source / "howdy/howdy"
        gtk = source / "howdy/howdy-gtk"
        (core / "src").mkdir(parents=True)
        gtk.mkdir()
        (core / "meson.build").write_text("project('howdy')\n")
        (gtk / "meson.build").write_text("project('howdy-gtk')\n")
        (core / "src/config.ini").write_text("[video]\n")
        (source / "howdy/LICENSE").write_text("upstream license\n")
        (source / "model.dat").write_bytes(b"model bytes")
        (source / "polkit-agent-helper-howdy.conf").write_text("[Service]\nPrivateDevices=no\n")
        out = self.root / "pkg"
        out.mkdir()
        # The unchanged Linux install -D tail also runs on BSD install in
        # this test: provide its parents (the path/copy contract is tested,
        # not install's platform-specific directory-creation option).
        for directory in ("etc/howdy", "usr/share/licenses/howdy",
                          "usr/lib/systemd/system/polkit-agent-helper@.service.d"):
            (out / directory).mkdir(parents=True)
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        log = self.root / "calls"
        commands = {
            "arch-meson": '''printf 'arch-meson %s %s\n' "$1" "$2" >>"$TEST_LOG"
test -f "$1/meson.build"
mkdir -p "$2"
printf '%s' "$1" >"$2/source"
''',
            "meson": '''printf 'meson %s\n' "$*" >>"$TEST_LOG"
case "$1" in
compile) test -f "$3/source"; touch "$3/compiled" ;;
install)
    test -f "$3/compiled"
    case "$(cat "$3/source")" in */howdy-gtk) name=howdy-gtk ;; *) name=howdy ;; esac
    mkdir -p "$5/usr/bin" "$5/usr/share/dlib-data"
    printf '%s\n' "$name" >"$5/usr/bin/$name"
    ;;
*) exit 9 ;;
esac
''',
        }
        for name, body in commands.items():
            path = bin_dir / name
            path.write_text("#!/bin/sh\nset -eu\n" + body)
            path.chmod(0o755)
        result = subprocess.run(["bash", "-euc", 'source "$1"; prepare; build; package',
                                 "bash", str(self.recipe)], capture_output=True, text=True,
                                env=dict(os.environ, srcdir=str(source), pkgdir=str(out), TEST_LOG=str(log),
                                         PATH=str(bin_dir) + os.pathsep + os.environ["PATH"]))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(log.read_text().splitlines(), [
            "arch-meson howdy/howdy build", "meson compile -C build",
            "arch-meson howdy/howdy-gtk build-gtk", "meson compile -C build-gtk",
            "meson install -C build --destdir " + str(out),
            "meson install -C build-gtk --destdir " + str(out),
        ])
        for path, expected in {
            "usr/bin/howdy": b"howdy\n", "usr/bin/howdy-gtk": b"howdy-gtk\n",
            "etc/howdy/config.ini": b"[video]\n", "usr/share/licenses/howdy/LICENSE": b"upstream license\n",
            "usr/share/dlib-data/model.dat": b"model bytes",
            "usr/lib/systemd/system/polkit-agent-helper@.service.d/10-howdy.conf": b"[Service]\nPrivateDevices=no\n",
        }.items():
            self.assertEqual((out / path).read_bytes(), expected)


if __name__ == "__main__":
    unittest.main()
