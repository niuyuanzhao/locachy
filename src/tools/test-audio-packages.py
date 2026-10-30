#!/usr/bin/env python3
"""Optional audio firmware declaration and upgrade migration, no pacman."""
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import stat
import sys
import tempfile
import types
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


migration = load("audio_migration", SOURCE / "link/migrate-audio-packages")
installer = load("audio_installer", SOURCE / "link/locachy-install")


class AudioPackages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="audio-packages-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "packages"
        self.marker = self.root / "state/migrations/audio-sof-firmware"

    def migrate(self, manifest=None):
        # Never the real /var/lib/locachy marker.
        return migration.migrate(manifest or self.manifest, self.marker)

    def test_firmware_belongs_to_optional_audio_not_core(self):
        modules = json.loads((SOURCE / "link/modules.json").read_text())
        self.assertIn("sof-firmware", modules["audio"]["packages"])
        core = {line.split("#", 1)[0].strip() for line in (SOURCE / "packages").read_text().splitlines()}
        self.assertNotIn("sof-firmware", core)
        self.assertFalse(any("sof-firmware" in value.get("packages", []) for name, value in modules.items()
                             if name != "audio"))

    def test_installer_audio_selection_declares_firmware_once(self):
        conf = self.root / "etc/locachy"
        conf.mkdir(parents=True)
        (conf / "packages").write_text("# user leaves\nbase\n")
        (conf / "rc.conf").write_text('hibernate_after="30"\n')
        args = types.SimpleNamespace(features="audio", containers="", encrypt=False)
        with mock.patch.object(installer, "update_source", return_value=("off", "main", "")):
            self.assertTrue(installer.apply_customizations(args, self.root))
            self.assertFalse(installer.apply_customizations(args, self.root))
        declared = (conf / "packages").read_text().splitlines()
        self.assertEqual(declared.count("sof-firmware"), 1)
        self.assertIn('service_audio="/usr/local/lib/locachy/audio-client"', (conf / "rc.conf").read_text())

    def test_minimal_install_does_not_gain_audio_or_firmware(self):
        conf = self.root / "etc/locachy"
        conf.mkdir(parents=True)
        (conf / "packages").write_text("base\n")
        (conf / "rc.conf").write_text('hibernate_after="30"\n')
        args = types.SimpleNamespace(features="", containers="", encrypt=False)
        with mock.patch.object(installer, "update_source", return_value=("off", "main", "")):
            self.assertFalse(installer.apply_customizations(args, self.root))
        self.assertEqual((conf / "packages").read_text(), "base\n")
        self.assertNotIn("service_audio=", (conf / "rc.conf").read_text())

    def test_legacy_complete_audio_manifest_is_completed_idempotently(self):
        original = "# retained\nbase\n" + "\n".join(sorted(migration.LEGACY_AUDIO))
        self.manifest.write_text(original)
        self.manifest.chmod(0o640)
        before = self.manifest.stat()
        self.assertTrue(self.migrate())
        self.assertTrue(self.marker.is_file())
        after = self.manifest.read_bytes()
        self.assertTrue(after.startswith(original.encode()))
        self.assertEqual(after.decode().splitlines().count("sof-firmware"), 1)
        self.assertFalse(self.migrate())
        self.assertEqual(self.manifest.read_bytes(), after)
        self.assertEqual(stat.S_IMODE(self.manifest.stat().st_mode), 0o640)
        self.assertEqual((self.manifest.stat().st_uid, self.manifest.stat().st_gid), (before.st_uid, before.st_gid))

    def test_multiple_names_per_line_complete_audio_and_find_existing_firmware(self):
        legacy = "base\npipewire pipewire-alsa\tpipewire-pulse # wireplumber in the next line\nwireplumber wiremix\n"
        self.manifest.write_text(legacy)
        self.assertTrue(self.migrate())
        self.assertEqual(self.manifest.read_text().splitlines().count("sof-firmware"), 1)
        self.marker.unlink()
        declared = legacy + "other\tsof-firmware # already declared\n"
        self.manifest.write_text(declared)
        self.assertFalse(self.migrate())
        self.assertEqual(self.manifest.read_text(), declared)
        self.assertTrue(self.marker.is_file())

    def test_user_removal_after_migration_is_not_added_again(self):
        legacy = "base\n" + "\n".join(sorted(migration.LEGACY_AUDIO)) + "\n"
        self.manifest.write_text(legacy)
        self.assertTrue(self.migrate())
        # locachy-pkg remove sof-firmware, then the pacman hook runs make.sh again.
        self.manifest.write_text(legacy)
        self.assertFalse(self.migrate())
        self.assertEqual(self.manifest.read_text(), legacy)

    def test_already_declared_firmware_records_the_migration(self):
        declared = "\n".join(sorted(migration.LEGACY_AUDIO | {"sof-firmware"})) + "\n"
        self.manifest.write_text(declared)
        self.assertFalse(self.migrate())
        self.assertTrue(self.marker.is_file())
        legacy = "\n".join(sorted(migration.LEGACY_AUDIO)) + "\n"
        self.manifest.write_text(legacy)
        self.assertFalse(self.migrate())
        self.assertEqual(self.manifest.read_text(), legacy)

    def test_partial_or_disabled_custom_manifest_is_unchanged(self):
        for text in ("base\n", "base\npipewire\nwireplumber\n"):
            with self.subTest(text=text):
                self.manifest.write_text(text)
                self.assertFalse(self.migrate())
                self.assertEqual(self.manifest.read_text(), text)
                self.assertFalse(self.marker.exists())
        self.assertFalse(self.migrate(self.root / "missing"))
        self.assertFalse(self.marker.exists())

    def test_symlink_manifest_is_rejected_without_writing_target(self):
        target = self.root / "target"
        target.write_text("\n".join(sorted(migration.LEGACY_AUDIO)))
        self.manifest.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "regular file"):
            self.migrate()
        self.assertNotIn("sof-firmware", target.read_text())
        self.assertFalse(self.marker.exists())

    def test_failed_atomic_replace_keeps_existing_manifest(self):
        original = "\n".join(sorted(migration.LEGACY_AUDIO)) + "\n"
        self.manifest.write_text(original)
        with mock.patch.object(migration.os, "replace", side_effect=OSError("fixture failure")):
            with self.assertRaises(OSError):
                self.migrate()
        self.assertEqual(self.manifest.read_text(), original)
        # No temporary file, and no marker: the migration is tried again.
        self.assertEqual(list(self.root.iterdir()), [self.manifest])

    def test_settings_and_build_deliver_same_optional_audio_completion(self):
        settings = (SOURCE / "progs/settings.c").read_text()
        self.assertIn('"pipewire pipewire-alsa pipewire-pulse wireplumber wiremix sof-firmware"', settings)
        build = (SOURCE / "make.sh").read_text()
        self.assertIn('python3 -I "$src/link/migrate-audio-packages" /etc/locachy/packages', build)
        self.assertNotIn('locachy-pkg sync\n', build)

    def test_migration_never_runs_package_or_service_commands(self):
        text = (SOURCE / "link/migrate-audio-packages").read_text()
        self.assertNotIn("import subprocess", text)
        self.assertNotIn("os.system", text)
        self.assertNotIn("service_audio", text)


if __name__ == "__main__":
    unittest.main()
