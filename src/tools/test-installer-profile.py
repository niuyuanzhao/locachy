#!/usr/bin/env python3
"""Build the ISO profile with command doubles, without Linux/archiso/network.
  python3 -B tools/test-installer-profile.py SCRATCH_PARENT
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

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


# Minimal unused upstream context so command doubles exercise the same patch.
ARCHISO_PATCH_CONTEXT = '''
    # Copy systemd-boot configuration files
    cp -r --remove-destination -- "${work_dir}/loader" "${isofs_dir}/"

    # Copy EFI binaries and related files
    _make_common_bootmode_uefi.systemd-boot_copy_files "${uefi_arch[$arch]}"

    if compgen -G "${isofs_dir}/shell"*'.efi' >/dev/null; then
        efiboot_rootdir_files+=("${isofs_dir}/shell"*'.efi')
    fi
    mcopy -s -i "${efibootimg}" "${efiboot_rootdir_files[@]}" '::/'
    if [[ -e "${isofs_dir}/boot/memtest86+/memtest.efi" ]]; then
        mmd -i "${efibootimg}" '::/boot' '::/boot/memtest86+'
        mcopy -i "${efibootimg}" \\
'''


class InstallerProfile(unittest.TestCase):
    APPS_URL = "https://example.invalid/locachy-apps.git"
    APPS_EXPORT_BASE = "c" * 40

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="installer-profile-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.src = self.root / "source"
        self.src.mkdir()
        shutil.copyfile(SOURCE / "make-installer.sh", self.src / "make-installer.sh")
        shutil.copytree(SOURCE / "installer", self.src / "installer")
        for directory in ("base", "init", "progs", "edit", "link", "tools", "pkgbuilds", "aur-patches", "containers"):
            path = self.src / directory
            path.mkdir()
            (path / "data").write_text("trusted " + directory)
        for filename in ("make-root.sh", "make.sh"):
            (self.src / filename).write_text("#!/bin/bash\nexit 0\n")
        (self.src / "link/locachy-install").write_text("#!/usr/bin/env python3\nprint('fixture installer')\n")
        (self.src / "link/gestures").write_text("#!/usr/bin/env python3\nprint('fixture')\n")
        shutil.copyfile(SOURCE / "link/locachy_apps.py", self.src / "link/locachy_apps.py")
        # An export of the app presets by default; checkout cases add real Git metadata.
        self.apps = self.root / "locachy-apps"
        (self.apps / "apps").mkdir(parents=True)
        (self.apps / "FORMAT").write_text("1\n")
        (self.apps / "apps/edge.conf").write_text('package="microsoft-edge-stable-bin"\n'
                                                  'command="/usr/bin/microsoft-edge-stable"\n')
        (self.src / "packages").write_text("base\nlabwc-locachy\n")
        (self.src / "rc.conf").write_text('key_home="Super+Esc"\n')
        self.baseline = self.root / "baseline"
        self.baseline.mkdir()
        (self.baseline / "packages.x86_64").write_text("base\nlinux\nopenssh\ncloud-init\n")
        (self.baseline / "profiledef.sh").write_text("# Baseline fixture is copied, not sourced by this test.\n"
                                                      "iso_name=upstream-fixture\n"
                                                      "bootmodes=('bios.syslinux' 'uefi.grub')\n"
                                                      "file_permissions=([\"/etc/shadow\"]=\"0:0:400\")\n")
        (self.baseline / "pacman.conf").write_text("[options]\n[cachyos-v4]\nInclude = bad-host-mirror\n")
        boot = self.baseline / "efiboot/loader/entries"
        boot.mkdir(parents=True)
        (boot / "01-archiso-linux.conf").write_text(
            "title Arch Linux (%ARCH%, UEFI)\nlinux /%INSTALL_DIR%/boot/%ARCH%/vmlinuz-linux\n"
            "initrd /%INSTALL_DIR%/boot/%ARCH%/initramfs-linux.img\n"
            "options archisobasedir=%INSTALL_DIR% archisosearchuuid=%ARCHISO_UUID%\n")
        (self.baseline / "syslinux").mkdir()
        (self.baseline / "syslinux/syslinux-linux.cfg").write_text(
            "LABEL arch\nMENU LABEL Arch Linux (%ARCH%, BIOS)\n"
            "LINUX /%INSTALL_DIR%/boot/%ARCH%/vmlinuz-linux\n"
            "INITRD /%INSTALL_DIR%/boot/%ARCH%/initramfs-linux.img\n"
            "APPEND archisobasedir=%INSTALL_DIR% archisosearchuuid=%ARCHISO_UUID%\n")
        sysroot = self.baseline / "airootfs/etc/systemd/system"
        for target, service in (("multi-user.target.wants", "sshd.service"),
                                ("multi-user.target.wants", "systemd-networkd.service"),
                                ("network-online.target.wants", "systemd-networkd-wait-online.service"),
                                ("sockets.target.wants", "systemd-networkd.socket"),
                                ("multi-user.target.wants", "vmtoolsd.service")):
            (sysroot / target).mkdir(parents=True, exist_ok=True)
            (sysroot / target / service).symlink_to("/usr/lib/systemd/system/" + service)
        (self.baseline / "airootfs/etc/ssh/sshd_config.d").mkdir(parents=True)
        (self.baseline / "airootfs/etc/ssh/sshd_config.d/10-archiso.conf").write_text("PermitRootLogin yes\n")
        (self.baseline / "airootfs/etc/mkinitcpio.conf.d").mkdir(parents=True)
        (self.baseline / "airootfs/etc/mkinitcpio.conf.d/archiso.conf").write_text("HOOKS=(base udev modconf archiso block filesystems)\n")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        tool = self.bin / "mkarchiso"
        tool.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >\"$TEST_CALLS\"\nexit \"${TEST_RESULT:-0}\"\n" +
                        ARCHISO_PATCH_CONTEXT)
        tool.chmod(0o755)
        # The builder compiles from its copied payload, including this double.
        self.live = self.root / "live-calls"
        live = self.src / "installer/make-live.sh"
        live.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$@" >"$TEST_LIVE"\nmkdir -p "$2/usr/local/bin"\n'
                        'printf elf >"$2/usr/local/bin/installer"\nprintf elf >"$2/usr/local/bin/term"\n'
                        'printf elf >"$2/usr/local/bin/rescue"\n')
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("GIT_", "LOCACHY_")) and key not in ("PACMAN_CACHE", "MKARCHISO")}
        self.env.update(ARCHISO_BASELINE=str(self.baseline), PATH=str(self.bin) + os.pathsep + self.env["PATH"],
                        TEST_CALLS=str(self.calls), TEST_LIVE=str(self.live),
                        LOCACHY_APPS_SRC=str(self.apps), LOCACHY_APPS_REPO=self.APPS_URL,
                        LOCACHY_APPS_BRANCH="main", LOCACHY_APPS_COMMIT=self.APPS_EXPORT_BASE,
                        GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.work = self.root / "work with spaces"
        self.out = self.root / "out"

    def git_apps(self, *args):
        return subprocess.run(["git", "-C", str(self.apps), *args], env=self.env,
                              check=True, capture_output=True, text=True).stdout.strip()

    def checkout_apps(self):
        for key in ("LOCACHY_APPS_REPO", "LOCACHY_APPS_BRANCH", "LOCACHY_APPS_COMMIT"):
            self.env.pop(key)
        self.git_apps("init", "-q", "-b", "catalog")
        self.git_apps("remote", "add", "origin", self.APPS_URL)
        self.git_apps("add", "FORMAT", "apps")
        self.git_apps("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                      "commit", "-q", "-m", "Initial presets")
        return self.git_apps("rev-parse", "HEAD")

    def run_builder(self, *args):
        return subprocess.run(["bash", str(self.src / "make-installer.sh"), *map(str, args)],
                              env=self.env, text=True, capture_output=True)

    def profile(self):
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.calls.exists())
        return self.work / "profile"

    def test_official_generic_profile_preserves_upstream_live_implementation(self):
        profile = self.profile()
        config = (profile / "pacman.conf").read_text()
        self.assertIn("Architecture = x86_64", config)
        self.assertEqual([line for line in config.splitlines() if line.startswith("[")], ["[options]", "[core]", "[extra]"])
        self.assertNotIn("cachyos", config.lower())
        self.assertNotIn("alhp", config.lower())
        self.assertIn("CacheDir = " + str(self.work / "cache"), config)
        self.assertTrue((self.work / "cache").is_dir())
        self.assertEqual(config.count("Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch"), 2)
        self.assertNotIn("Include =", config)
        live = profile / "airootfs"
        live_config = (live / "etc/pacman.conf").read_text()
        self.assertEqual(live_config, (self.src / "installer/pacman.conf").read_text())
        self.assertNotIn(str(self.work), live_config)
        self.assertIn("Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch",
                      (live / "etc/pacman.d/mirrorlist").read_text())
        self.assertNotIn("bad-host-mirror", live_config)
        packages = (profile / "packages.x86_64").read_text().splitlines()
        self.assertEqual(len(packages), len(set(packages)))
        self.assertTrue({"base", "linux", "linux-firmware", "base-devel", "python", "git", "curl",
                         "pacman-contrib", "networkmanager", "rsync", "e2fsprogs", "dosfstools"} <= set(packages))
        self.assertNotIn("grub", packages)  # UEFI through systemd-boot, from the live system's systemd
        self.assertNotIn("linux-cachyos-bore", packages)
        self.assertTrue({"openssh", "cloud-init"} <= set(packages))  # Do not trim the packaged baseline.
        self.assertEqual((profile / "airootfs/etc/mkinitcpio.conf.d/archiso.conf").read_text(),
                         (self.baseline / "airootfs/etc/mkinitcpio.conf.d/archiso.conf").read_text())
        definition = (profile / "profiledef.sh").read_text()
        # The baseline's line is kept; LOCachy's override after it wins when sourced.
        self.assertIn("bootmodes=('bios.syslinux' 'uefi.grub')", definition)
        self.assertGreater(definition.rindex("bootmodes=('bios.syslinux' 'uefi.systemd-boot')"),
                           definition.index("bootmodes=('bios.syslinux' 'uefi.grub')"))
        self.assertIn('iso_name="locachy-installer"', definition)
        self.assertIn("buildmodes=('iso')", definition)
        self.assertNotIn('source "$', definition)

    def test_payload_and_permissions_cover_source_entrypoints_not_data(self):
        profile = self.profile()
        root = profile / "airootfs"
        payload = root / "usr/local/src/locachy"
        for directory in ("base", "init", "progs", "edit", "link", "tools", "pkgbuilds", "aur-patches", "containers"):
            self.assertEqual((payload / directory / "data").read_text(), "trusted " + directory)
        for name in ("make-root.sh", "make.sh", "packages", "rc.conf"):
            self.assertEqual((payload / name).read_bytes(), (self.src / name).read_bytes())
        self.assertEqual((root / "usr/local/bin/locachy-install").read_bytes(),
                         (SOURCE / "installer/locachy-install").read_bytes())
        definition = (profile / "profiledef.sh").read_text()
        for name in ("make-root.sh", "make.sh", "link/locachy-install", "link/gestures"):
            self.assertIn('["/usr/local/src/locachy/' + name + '"]="0:0:755"', definition)
        self.assertNotIn('["/usr/local/src/locachy/base/data"]', definition)
        self.assertTrue((root / "usr/local/bin/locachy-install").stat().st_mode & 0o111)

    def test_full_screen_installer_on_tty1_only(self):
        profile = self.profile()
        root = profile / "airootfs"
        self.assertEqual(self.live.read_text().splitlines(), [str(root / "usr/local/src/locachy"), ".", str(root)])
        definition = (profile / "profiledef.sh").read_text()
        for name in ("installer", "term", "rescue", "locachy-rescue"):
            self.assertTrue((root / "usr/local/bin" / name).is_file())
            self.assertIn('["/usr/local/bin/' + name + '"]="0:0:755"', definition)
        for name in ("rescue-storage", "rescue-link.py"):
            self.assertTrue((root / "usr/local/lib/locachy" / name).is_file())
            self.assertIn('["/usr/local/lib/locachy/' + name + '"]="0:0:755"', definition)
        rescue_unit = (root / "etc/systemd/system/locachy-rescue.service").read_text()
        self.assertIn("ConditionKernelCommandLine=locachy.mode=rescue", rescue_unit)
        self.assertIn("ExecStart=/usr/local/bin/rescue", rescue_unit)
        self.assertIn("Environment=LOCACHY_RESCUE_BOOT=1", rescue_unit)
        self.assertNotIn("/term", rescue_unit)
        self.assertNotIn("/bin/bash", rescue_unit)
        getty = (root / "etc/systemd/system/getty@tty1.service.d/rescue.conf").read_text()
        self.assertIn("ConditionKernelCommandLine=!locachy.mode=rescue", getty)
        wants = root / "etc/systemd/system/multi-user.target.wants/locachy-rescue.service"
        self.assertEqual(os.readlink(wants), "../locachy-rescue.service")
        self.assertTrue((root / "etc/systemd/system/locachy-rescue-link.service").is_file())
        self.assertFalse((root / "etc/systemd/system/multi-user.target.wants/locachy-rescue-link.service").exists())
        rescue_uefi = (profile / "efiboot/loader/entries/02-locachy-rescue.conf").read_text()
        self.assertIn("title   LOCachy Rescue", rescue_uefi)
        self.assertIn("locachy.mode=rescue", rescue_uefi)
        rescue_bios = (profile / "syslinux/syslinux-linux.cfg").read_text()
        self.assertIn("LABEL locachy-rescue", rescue_bios)
        self.assertIn("MENU LABEL LOCachy Rescue", rescue_bios)
        self.assertIn("locachy.mode=rescue", rescue_bios)
        autologin = (root / "etc/systemd/system/getty@tty1.service.d/autologin.conf").read_text()
        self.assertIn("--autologin root", autologin)
        self.assertFalse((root / "etc/systemd/system/serial-getty@.service.d").exists())
        login = (root / "root/.bash_profile").read_text()
        self.assertIn('if [ "$(tty)" = /dev/tty1 ]; then', login)
        self.assertTrue((root / "usr/local/src/locachy/installer/live-update.py").is_file())
        self.assertLess(login.index('/usr/local/bin/locachy-install --ui'), login.index('/usr/local/bin/installer --text'))
        packages = set((profile / "packages.x86_64").read_text().splitlines())
        self.assertTrue({"freetype2", "libinput", "json-c", "ttf-dejavu", "noto-fonts-cjk", "wayland",
                         "openssh", "openssl", "iproute2", "util-linux"} <= packages)
        # The clipboard the installer copies to and its terminal pastes from.
        unit = (root / "etc/systemd/system/locachy-clipboard.service").read_text()
        self.assertIn("ExecStart=/usr/local/lib/locachy/clipboard --serve", unit)
        self.assertIn("RuntimeDirectory=locachy", unit)
        wants = root / "etc/systemd/system/multi-user.target.wants/locachy-clipboard.service"
        self.assertEqual(os.readlink(wants), "../locachy-clipboard.service")
        self.assertIn('["/usr/local/lib/locachy/clipboard"]="0:0:755"', definition)

    def test_rescue_boot_template_drift_stops_before_installation_tools(self):
        path = self.baseline / "efiboot/loader/entries/01-archiso-linux.conf"
        path.write_text(path.read_text().replace("options ", "changed-options "))
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("packaged UEFI Linux entry changed", result.stderr)
        self.assertFalse(self.live.exists())
        self.assertFalse(self.calls.exists())

    def test_source_origin_records_repository_and_branch(self):
        self.env.update(LOCACHY_REPO="https://github.com/me/locachy.git", LOCACHY_BRANCH="next", LOCACHY_COMMIT="b" * 40)
        origin = json.loads((self.profile() / "airootfs/etc/locachy/source-origin.json").read_text())
        self.assertEqual(origin, {"repo": "https://github.com/me/locachy.git", "branch": "next", "commit": "b" * 40,
                                  "source_changed": False,
                                  "apps_repo": self.APPS_URL, "apps_branch": "main", "apps_commit": "",
                                  "apps_base_commit": self.APPS_EXPORT_BASE, "apps_source_kind": "export"})

    def assert_checkout_catalog(self, base, commit, kind):
        root = self.profile() / "airootfs"
        origin = json.loads((root / "etc/locachy/source-origin.json").read_text())
        self.assertEqual({key: value for key, value in origin.items() if key.startswith("apps_")},
                         dict(apps_repo=self.APPS_URL, apps_branch="catalog", apps_commit=commit,
                              apps_base_commit=base, apps_source_kind=kind))
        catalog = root / "usr/local/share/locachy/apps-catalog"
        self.assertIn(f'commit="{commit}"\n', (catalog / "CATALOG").read_text())
        self.assertEqual((catalog / "edge.conf").read_bytes(), (self.apps / "apps/edge.conf").read_bytes())

    def test_clean_checkout_records_commit(self):
        commit = self.checkout_apps()
        self.assert_checkout_catalog(commit, commit, "checkout")

    def test_dirty_checkout_records_only_base_commit(self):
        commit = self.checkout_apps()
        self.env["LOCACHY_APPS_COMMIT"] = commit
        (self.apps / "README").write_text("A local change outside the catalog\n")
        self.assert_checkout_catalog(commit, "", "dirty-checkout")

    def test_git_status_cannot_hide_uncommitted_catalog_bytes(self):
        commit = self.checkout_apps()
        self.git_apps("update-index", "--assume-unchanged", "apps/edge.conf")
        recipe = self.apps / "apps/edge.conf"
        recipe.write_text(recipe.read_text().replace("-stable-bin", "-test-bin"))
        self.assertEqual(self.git_apps("status", "--porcelain", "--untracked-files=all"), "")
        self.assert_checkout_catalog(commit, "", "dirty-checkout")

    def test_checkout_cannot_claim_a_different_base_commit(self):
        self.checkout_apps()
        self.env["LOCACHY_APPS_COMMIT"] = self.APPS_EXPORT_BASE
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the app presets checkout HEAD", result.stderr)
        self.assertFalse(self.work.exists())
        self.assertFalse(self.out.exists())


    def test_the_medium_has_a_validated_catalog_of_the_app_presets(self):
        catalog = self.profile() / "airootfs/usr/local/share/locachy/apps-catalog"
        self.assertEqual(sorted(p.name for p in catalog.iterdir()), ["CATALOG", "edge.conf"])
        self.assertEqual((catalog / "edge.conf").read_bytes(), (self.apps / "apps/edge.conf").read_bytes())
        meta = (catalog / "CATALOG").read_text()
        for line in ('format="1"', 'origin="medium"', f'repo="{self.APPS_URL}"', 'branch="main"', 'commit=""'):
            self.assertIn(line + "\n", meta)

    def test_the_medium_preserves_format_two_host_access_recipes(self):
        (self.apps / "FORMAT").write_text("2\n")
        recipe = 'host="1"\npackage="chatgpt-desktop"\ncommand="/usr/bin/chatgpt"\n'
        (self.apps / "apps/chatgpt.conf").write_text(recipe)
        catalog = self.profile() / "airootfs/usr/local/share/locachy/apps-catalog"
        self.assertIn('format="2"\n', (catalog / "CATALOG").read_text())
        self.assertEqual((catalog / "chatgpt.conf").read_text(), recipe)

    def test_missing_or_invalid_app_presets_stop_the_build_before_anything_is_made(self):
        def refused(said):
            result = self.run_builder("--profile-only", self.work, self.out)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(said, result.stderr)
            self.assertFalse(self.work.exists())  # never an empty catalog
            self.assertFalse(self.out.exists())
        (self.apps / "FORMAT").write_text("4\n")
        refused("not compatible: format 4")
        (self.apps / "FORMAT").write_text("2\n")
        (self.apps / "apps/edge.conf").write_text('tor="1"\n')
        refused("tor requires format 3")
        (self.apps / "FORMAT").write_text("1\n")
        (self.apps / "apps/edge.conf").write_text('host="1"\n')
        refused("host requires format 2")
        shutil.rmtree(self.apps / "apps")
        refused("no apps directory")
        self.env["LOCACHY_APPS_SRC"] = str(self.root / "nowhere")
        refused("the app presets are missing")
        del self.env["LOCACHY_APPS_SRC"]
        refused("the app presets are missing")

    def test_networkmanager_and_resolved_replace_networkd_without_remote_root(self):
        profile = self.profile()
        root = profile / "airootfs"
        system = root / "etc/systemd/system"
        for name in ("sshd.service", "cloud-init.target", "systemd-networkd.service", "systemd-networkd.socket"):
            self.assertEqual(os.readlink(system / name), "/dev/null")
        self.assertFalse((system / "multi-user.target.wants/sshd.service").is_symlink())
        self.assertFalse((root / "etc/ssh/sshd_config.d/10-archiso.conf").exists())
        self.assertFalse((system / "multi-user.target.wants/systemd-networkd.service").is_symlink())
        self.assertFalse((system / "sockets.target.wants/systemd-networkd.socket").is_symlink())
        self.assertTrue((system / "multi-user.target.wants/vmtoolsd.service").is_symlink())
        self.assertEqual(os.readlink(system / "multi-user.target.wants/NetworkManager.service"),
                         "/usr/lib/systemd/system/NetworkManager.service")
        self.assertEqual(os.readlink(root / "etc/resolv.conf"), "/run/systemd/resolve/stub-resolv.conf")
        self.assertEqual((root / "etc/NetworkManager/conf.d/20-locachy-installer.conf").read_text(),
                         "[main]\ndns=systemd-resolved\n")

    def test_success_requires_one_image_and_writes_cpu_source_record(self):
        tool = self.bin / "mkarchiso"
        tool.write_text('#!/bin/sh\nwhile [ "$1" != -o ]; do shift; done; shift\nprintf fixture-image >"$1/locachy-installer-test-x86_64.iso"\nexit 0\n' +
                        ARCHISO_PATCH_CONTEXT)
        tool.chmod(0o755)
        result = self.run_builder(self.work, self.out)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = json.loads((self.out / "build.json").read_text())
        self.assertEqual(record["cpu_baseline"], "x86-64")
        self.assertTrue(record["target_built_on_install"])
        self.assertEqual(record["source_sha256"]["make-root.sh"],
                         hashlib.sha256((self.src / "make-root.sh").read_bytes()).hexdigest())
        root = self.work / "profile/airootfs"
        self.assertEqual(record["source_origin"], json.loads((root / "etc/locachy/source-origin.json").read_text()))
        catalog = root / "usr/local/share/locachy/apps-catalog"
        self.assertEqual(record["apps_catalog_sha256"], {
            name: hashlib.sha256((catalog / name).read_bytes()).hexdigest() for name in ("CATALOG", "edge.conf")})
        self.assertIn(record["sha256"], (self.out / "SHA256SUMS").read_text())

    def test_upstream_builder_drift_stops_without_running_it(self):
        tool = self.bin / "mkarchiso"
        tool.write_text('#!/bin/sh\nprintf ran >"$TEST_CALLS"\n')
        result = self.run_builder(self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())
        self.assertEqual(tool.read_text(), '#!/bin/sh\nprintf ran >"$TEST_CALLS"\n')

    def test_existing_destination_refusal_does_not_change_source_or_baseline(self):
        self.out.mkdir()
        sentinel = self.out / "keep"
        sentinel.write_text("do not overwrite")
        before = hashlib.sha256((self.src / "make-root.sh").read_bytes()).hexdigest()
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("destination exists", result.stderr)
        self.assertFalse(self.work.exists())
        self.assertEqual(sentinel.read_text(), "do not overwrite")
        self.assertEqual(hashlib.sha256((self.src / "make-root.sh").read_bytes()).hexdigest(), before)
        self.assertEqual((self.baseline / "pacman.conf").read_text(), "[options]\n[cachyos-v4]\nInclude = bad-host-mirror\n")

    def test_source_tree_and_shared_destination_are_rejected(self):
        result = self.run_builder("--profile-only", self.src / "work", self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.out.exists())
        result = self.run_builder("--profile-only", self.work, self.work)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.work.exists())
        alias = self.root / "source-alias"
        alias.symlink_to(self.src, target_is_directory=True)
        result = subprocess.run(["bash", str(alias / "make-installer.sh"), "--profile-only",
                                 str(self.src / "work"), str(self.out)], env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside the source tree", result.stderr)
        self.assertFalse((self.src / "work").exists())

    def test_real_mode_invokes_standard_mkarchiso_and_propagates_failure(self):
        self.env["TEST_RESULT"] = "19"
        result = self.run_builder(self.work, self.out)
        self.assertEqual(result.returncode, 19, result.stdout + result.stderr)
        self.assertEqual(self.calls.read_text().strip(),
                         f"-w {self.work}/build -o {self.out} {self.work}/profile")

    def test_missing_installer_is_not_replaced_with_a_placeholder(self):
        (self.src / "link/locachy-install").unlink()
        result = self.run_builder("--profile-only", self.work, self.out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source installer entry is missing", result.stderr)
        self.assertFalse(self.work.exists())
        self.assertFalse(self.out.exists())


class LiveEntry(unittest.TestCase):
    """installer/locachy-install, the live entry: the newest source from the
    update repository (here a local one, reached at an https URL through
    git's insteadOf), else what was fetched from there before, else the
    medium's own; LOCACHY_SOURCE says which and why."""
    URL = "https://example.invalid/locachy.git"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="live-entry-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.git_env = dict(os.environ, HOME=str(self.root), GIT_CONFIG_NOSYSTEM="1",
                            GIT_AUTHOR_NAME="test", GIT_AUTHOR_EMAIL="test@example.invalid",
                            GIT_COMMITTER_NAME="test", GIT_COMMITTER_EMAIL="test@example.invalid")
        self.repo = self.root / "repo"
        self.git("init", "-q", "-b", "main", str(self.repo))
        self.commit("v1")
        self.bundled = self.root / "bundled"
        self.fake(self.bundled / "link/locachy-install", "bundled")
        self.origin = self.root / "source-origin.json"
        self.origin.write_text(json.dumps({"repo": self.URL, "branch": "main", "commit": "0" * 40}))
        self.fetched = self.root / "run/locachy-source"
        self.fetched.parent.mkdir()
        self.apps_fetched = self.root / "run/locachy-apps"
        self.apps_bundled = self.root / "apps-catalog"
        self.apps_bundled.mkdir()
        (self.apps_bundled / "CATALOG").write_text('format="1"\norigin="medium"\n')
        self.state = self.fetched.with_suffix(".snapshots")
        self.seen = self.root / "apps-seen"
        self.env = dict(self.git_env, LOCACHY_SOURCE_BUNDLED=str(self.bundled), LOCACHY_SOURCE_FETCHED=str(self.fetched),
                        LOCACHY_SOURCE_ORIGIN=str(self.origin), GIT_ALLOW_PROTOCOL="file", GIT_CONFIG_COUNT="1",
                        GIT_CONFIG_KEY_0=f"url.file://{self.repo}.insteadOf", GIT_CONFIG_VALUE_0=self.URL,
                        LOCACHY_APPS_FETCHED=str(self.apps_fetched), LOCACHY_APPS_BUNDLED=str(self.apps_bundled),
                        TEST_APPS=str(self.seen))

    def git(self, *args):
        subprocess.run(["git", *args], check=True, capture_output=True, env=self.git_env)

    @staticmethod
    def fake(path, tag):
        """A locachy-install that says which it is, how it was got, its
        arguments; and (in TEST_APPS) which app presets it was given."""
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"import os, sys\nprint({tag!r}, os.environ.get('LOCACHY_SOURCE'), *sys.argv[1:])\n"
                        "open(os.environ['TEST_APPS'], 'w').write(os.environ.get('LOCACHY_APPS', '') + '\\n' + "
                        "os.environ.get('LOCACHY_APPS_CATALOG', '') + '\\n')\n")

    @staticmethod
    def updater(path, body=None):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body or """\
import json, os, sys
source = sys.argv[sys.argv.index('--source') + 1]
print(json.dumps({'snapshot': os.environ['LOCACHY_INSTALL_SNAPSHOT'],
                  'source': source, 'catalog': os.environ['LOCACHY_APPS_CATALOG'],
                  'source_how': os.environ['LOCACHY_SOURCE'], 'apps_how': os.environ['LOCACHY_APPS']}))
""")

    def commit(self, tag, helper=None):
        self.fake(self.repo / "src/link/locachy-install", tag)
        helper_path = self.repo / "src/link/locachy_apps.py"
        if helper is None:
            shutil.copyfile(SOURCE / "link/locachy_apps.py", helper_path)
        else:
            helper_path.write_text(helper)
        self.git("-C", str(self.repo), "add", "-A")
        self.git("-C", str(self.repo), "commit", "-q", "-m", tag)

    def apps_seen(self):
        """(LOCACHY_APPS, LOCACHY_APPS_CATALOG) the installer was given."""
        return tuple(self.seen.read_text().splitlines())

    def run_entry(self, *args):
        return subprocess.run(["bash", str(SOURCE / "installer/locachy-install"), *args], env=self.env,
                              capture_output=True, text=True, timeout=60)

    def entry(self, *args):
        result = self.run_entry(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip(), result.stderr

    def test_installs_from_the_newest_then_the_one_before_then_the_medium_own(self):
        out, err = self.entry("--describe-json")
        self.assertEqual(out, "v1 fetched --describe-json")
        self.assertIn(f"Fetching the newest LOCachy source: {self.URL} (main)", err)
        self.commit("v2")
        out, err = self.entry("--check", "--disk=/dev/sda")
        self.assertEqual(out, "v1 fetched earlier --check --disk=/dev/sda")
        self.assertEqual(err, "")
        out, _ = self.entry("--yes", "--disk=/dev/sda")
        self.assertEqual(out, "v2 fetched --yes --disk=/dev/sda")
        out, _ = self.entry("--yes", "--update-branch", "other")
        self.assertTrue(out.startswith(f"bundled bundled: could not fetch {self.URL} (other): "), out)
        shutil.rmtree(self.repo)
        out, _ = self.entry("--yes")
        self.assertTrue(out.startswith(f"v2 fetched before: could not fetch {self.URL} (main): "), out)
        self.assertEqual(sorted(p.name for p in self.fetched.parent.iterdir()),
                         ["locachy-source", "locachy-source.snapshots"])
        self.assertEqual(len(list(self.state.glob("source.*"))), 2)

    def test_recorded_local_changes_are_kept_only_over_the_same_remote_commit(self):
        commit = subprocess.check_output(['git', '-C', str(self.repo), 'rev-parse', 'HEAD'], text=True).strip()
        self.origin.write_text(json.dumps({'repo': self.URL, 'branch': 'main', 'commit': commit,
                                          'source_changed': True}))
        out, err = self.entry('--yes')
        self.assertTrue(out.startswith('bundled bundled: local changes over the fetched commit '), out)
        self.assertIn("keeping this medium's recorded local changes", err)
        self.updater(self.bundled / 'installer/live-update.py')
        selected = json.loads(self.entry('--ui')[0])
        self.assertEqual(selected['source_how'], 'bundled: local changes over the fetched commit')
        self.assertNotEqual(Path(selected['source']), self.bundled)
        self.assertNotEqual(Path(selected['catalog']), self.apps_bundled)
        self.env['LOCACHY_INSTALL_SNAPSHOT'] = selected['snapshot']
        self.commit('v2')
        self.fake(self.bundled / 'link/locachy-install', 'changed medium')
        out, err = self.entry('--yes')
        self.assertEqual(out, 'bundled bundled: local changes over the fetched commit --yes')
        self.assertEqual(err, '')
        self.env.pop('LOCACHY_INSTALL_SNAPSHOT')
        out, _ = self.entry('--yes')
        self.assertEqual(out, 'v2 fetched --yes')

    def test_ui_check_install_and_refreshed_resume_each_use_their_exact_selection(self):
        self.apps_repository()
        self.updater(self.repo / 'src/installer/live-update.py')
        self.commit('v2')
        selected = json.loads(self.entry('--ui')[0])
        initial_catalog = Path(selected['catalog'])
        recipe = initial_catalog / 'edge.conf'
        original = recipe.read_text()
        self.assertRegex(selected['snapshot'], r'^[0-9a-f]{64}$')
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.commit('v3')
        (self.apps / 'apps/edge.conf').write_text(original.replace('microsoft-edge-stable-bin', 'edge-new'))
        self.apps_commit('Catalog update')
        self.entry('--yes')  # A separate serial call advances both latest pointers.
        self.assertNotEqual(self.fetched.resolve() / 'src', Path(selected['source']))
        self.assertNotEqual(self.apps_fetched.resolve(), initial_catalog)
        self.env['LOCACHY_INSTALL_SNAPSHOT'] = selected['snapshot']
        for operation in ('--describe-json', '--check', '--yes'):
            self.assertEqual(self.entry(operation), ('v2 fetched ' + operation, ''))
            self.assertEqual(self.apps_seen(), ('fetched', str(initial_catalog)))
            self.assertEqual(recipe.read_text(), original)
        saved = self.root / 'resume.json'
        saved.write_text(json.dumps({'phase': 'preflight', 'update_repo': self.URL, 'update_branch': 'main',
                                     'apps_repo': self.APPS_URL, 'apps_branch': 'main'}))
        self.env['LOCACHY_RESUME_RECORD'] = str(saved)
        updated, err = self.entry('--refresh-live')
        updated = json.loads(updated)
        self.assertIn('Fetching the newest LOCachy source:', err)
        self.assertIn('Fetching the newest app presets:', err)
        self.assertNotEqual(updated['snapshot'], selected['snapshot'])
        self.assertEqual(Path(updated['catalog']), self.apps_fetched.resolve())
        self.assertIn('edge-new', (Path(updated['catalog']) / 'edge.conf').read_text())
        self.commit('v4')
        (self.apps / 'apps/edge.conf').write_text(original)
        self.apps_commit('Another catalog update')
        self.env.pop('LOCACHY_INSTALL_SNAPSHOT')
        self.entry('--yes')
        self.env['LOCACHY_INSTALL_SNAPSHOT'] = updated['snapshot']
        self.assertEqual(self.entry('--resume', '--yes'), ('v3 fetched --resume --yes', ''))
        self.assertEqual(self.apps_seen(), ('fetched', updated['catalog']))
        # Deleting a selected version cannot silently fall through to latest.
        shutil.rmtree(updated['catalog'])
        result = self.run_entry('--resume', '--yes')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('cannot reopen the live selection', result.stderr)
        self.assertNotIn('Fetching the newest', result.stderr)

    def test_invalid_or_mismatched_live_selection_fails_except_read_only_status(self):
        self.updater(self.repo / 'src/installer/live-update.py')
        self.commit('v2')
        selected = json.loads(self.entry('--ui')[0])
        self.env['LOCACHY_INSTALL_SNAPSHOT'] = selected['snapshot']
        for option in ('--update-branch=other', '--apps-branch=other', '--apps-repo=off'):
            result = self.run_entry('--check', option)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('refresh first', result.stderr)
            self.assertNotIn('Fetching the newest', result.stderr)
        selection = self.state / (selected['snapshot'] + '.json')
        selection.write_text('{}')
        result = self.run_entry('--check')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('selection record does not match the token', result.stderr)
        for invalid in ('', '../resume.json', '0' * 64):
            self.env['LOCACHY_INSTALL_SNAPSHOT'] = invalid
            result = self.run_entry('--yes')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('cannot reopen the live selection', result.stderr)
            self.assertNotIn('Fetching the newest', result.stderr)
            for status in ('--resume-status', '--resume-info'):
                out, err = self.entry(status)
                self.assertEqual((out, err), ('bundled None ' + status, ''))
        # A fresh UI invocation ignores even an invalid inherited token.
        self.assertEqual(json.loads(self.entry('--ui')[0])['snapshot'], selected['snapshot'])

    def test_apps_refresh_failure_leaves_initial_selection_usable(self):
        self.apps_repository()
        self.updater(self.repo / 'src/installer/live-update.py')
        self.commit('v2')
        selected = json.loads(self.entry('--ui')[0])
        self.env['LOCACHY_INSTALL_SNAPSHOT'] = selected['snapshot']
        self.commit('v3')
        (self.apps / 'FORMAT').write_text('4\n')
        self.apps_commit('Unsupported format')
        result = self.run_entry('--refresh-live')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('the app presets could not be refreshed', result.stderr)
        self.assertIn('not compatible: format 4', result.stderr)
        self.assertEqual(self.entry('--check'), ('v2 fetched --check', ''))
        self.assertEqual(self.apps_seen(), ('fetched', selected['catalog']))

    def test_legacy_or_mismatched_updater_response_cannot_enable_refresh(self):
        path = self.repo / 'src/installer/live-update.py'
        for tag, output in (('v2', {}), ('v3', {'snapshot': 'a' * 64})):
            self.updater(path, f'print({json.dumps(output)!r})\n')
            self.commit(tag)
            result = self.run_entry('--refresh-live', '--apps-repo=off')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertIn('the selected updater did not return the live selection', result.stderr)

    def test_failed_refresh_does_not_enter_cached_updater(self):
        updater = self.repo / 'src/installer/live-update.py'
        updater.parent.mkdir()
        updater.write_text('raise SystemExit("cached updater must not be entered")\n')
        self.commit('v2')
        self.entry('--describe-json')
        shutil.rmtree(self.repo)
        result = self.run_entry('--refresh-live')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Installer refresh failed: the source could not be refreshed: fetched before:', result.stderr)
        self.assertNotIn('cached updater must not be entered', result.stderr)
        self.assertEqual(result.stdout, '')

    def test_updates_off_or_no_repository_run_the_medium_own(self):
        out, _ = self.entry("--yes", "--update-repo=off")
        self.assertEqual(out, "bundled bundled: updates are off --yes --update-repo=off")
        out, _ = self.entry("--update-repo", "/somewhere/locachy", "--yes")
        self.assertEqual(out, "bundled bundled: no repository to fetch from (/somewhere/locachy, main) "
                              "--update-repo /somewhere/locachy --yes")
        self.assertFalse(self.fetched.exists())

    def test_continuing_fetches_from_where_that_installation_did(self):
        fork = "https://example.invalid/fork.git"
        self.git("-C", str(self.repo), "branch", "next")
        record = self.root / "resume.json"
        record.write_text(json.dumps({"install_id": "a" * 32, "update_repo": fork, "update_branch": "next"}))
        self.env.update(LOCACHY_RESUME_RECORD=str(record), GIT_CONFIG_COUNT="2",
                        GIT_CONFIG_KEY_1=f"url.file://{self.repo}.insteadOf", GIT_CONFIG_VALUE_1=fork)
        out, err = self.entry("--resume", "--yes")
        self.assertEqual(out, "v1 fetched --resume --yes")
        self.assertIn(f"Fetching the newest LOCachy source: {fork} (next)", err)
        out, err = self.entry("--trust-key", "40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C")
        self.assertEqual(out, "v1 fetched earlier --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C")
        self.assertEqual(err, "")
        out, err = self.entry("--yes")  # not continuing: the medium's update repository
        self.assertIn(f"Fetching the newest LOCachy source: {self.URL} (main)", err)

    APPS_URL = "https://example.invalid/locachy-apps.git"

    def apps_repository(self):
        """The app presets' repository, at APPS_URL, named by the medium."""
        self.apps = self.root / "apps-repo"
        self.git("init", "-q", "-b", "main", str(self.apps))
        (self.apps / "apps").mkdir()
        (self.apps / "FORMAT").write_text("1\n")
        (self.apps / "apps/edge.conf").write_text('package="microsoft-edge-stable-bin"\n'
                                                  'command="/usr/bin/microsoft-edge-stable"\n')
        self.apps_commit("First")
        self.origin.write_text(json.dumps({"repo": self.URL, "branch": "main", "commit": "0" * 40,
                                           "apps_repo": self.APPS_URL, "apps_branch": "main"}))
        self.env.update(GIT_CONFIG_COUNT="2", GIT_CONFIG_KEY_1=f"url.file://{self.apps}.insteadOf",
                        GIT_CONFIG_VALUE_1=self.APPS_URL)

    def apps_commit(self, message):
        self.git("-C", str(self.apps), "add", "-A")
        self.git("-C", str(self.apps), "commit", "-q", "-m", message)
        return subprocess.run(["git", "-C", str(self.apps), "rev-parse", "HEAD"], check=True, capture_output=True,
                              text=True).stdout.strip()

    def test_app_presets_the_newest_then_the_one_before_then_the_medium_own(self):
        self.apps_repository()
        first = subprocess.run(["git", "-C", str(self.apps), "rev-parse", "HEAD"], capture_output=True,
                               text=True).stdout.strip()
        out, err = self.entry("--describe-json")
        self.assertEqual(out, "v1 fetched --describe-json")
        self.assertIn(f"Fetching the newest app presets: {self.APPS_URL} (main)", err)
        path = str(self.apps_fetched.resolve())
        self.assertEqual(self.apps_seen(), ("fetched", path))
        catalog = (self.apps_fetched / "CATALOG").read_text()
        for line in ('origin="repository"', f'repo="{self.APPS_URL}"', 'branch="main"', f'commit="{first}"'):
            self.assertIn(line + "\n", catalog)
        self.assertEqual(sorted(p.name for p in self.apps_fetched.iterdir()), ["CATALOG", "edge.conf"])
        out, err = self.entry("--check")
        self.assertEqual((err, self.apps_seen()), ("", ("fetched earlier", path)))
        # Content this system does not take: the one fetched before, and why.
        (self.apps / "FORMAT").write_text("4\n")
        self.apps_commit("Format four")
        self.entry("--yes")
        self.assertEqual(self.apps_seen(), (f"fetched before: {self.APPS_URL} (main): not compatible: format 4", path))
        self.assertIn(f'commit="{first}"', (self.apps_fetched / "CATALOG").read_text())
        # Another branch, never fetched from: the medium's own.
        self.entry("--yes", "--apps-branch", "other")
        how, where = self.apps_seen()
        self.assertTrue(how.startswith(f"bundled: could not fetch {self.APPS_URL} (other): "), how)
        self.assertEqual(where, str(self.apps_bundled))
        self.entry("--yes", "--apps-repo=off")
        self.assertEqual(self.apps_seen(), ("bundled: updates are off", str(self.apps_bundled)))
        self.assertEqual(sorted(p.name for p in self.apps_fetched.parent.iterdir()),
                         ["locachy-apps", "locachy-source", "locachy-source.snapshots"])

    def test_app_presets_are_made_a_catalog_by_the_fetched_source_s_own_helper(self):
        self.apps_repository()
        self.commit("v2", helper="import sys\nprint('the helper of v2', file=sys.stderr)\nsys.exit(1)\n")
        self.entry("--yes")
        self.assertEqual(self.apps_seen(), (f"bundled: {self.APPS_URL} (main): the helper of v2",
                                            str(self.apps_bundled)))
        self.assertFalse(self.apps_fetched.exists())
        # A source without the helper (from before the app presets' repository).
        (self.repo / "src/link/locachy_apps.py").unlink()
        self.git("-C", str(self.repo), "commit", "-q", "-am", "v3")
        self.entry("--yes")
        self.assertEqual(self.apps_seen(), ("bundled: this source has no app presets helper", str(self.apps_bundled)))

    def test_live_update_stages_executable_and_rejects_stale_metadata(self):
        updater = SOURCE / "installer/live-update.py"
        spec = importlib.util.spec_from_file_location("live_update_test", updater)
        live = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(live)
        stage = self.root / "stage"
        (stage / "usr/local/bin").mkdir(parents=True)
        for name in ("installer", "term", "rescue"):
            path = stage / "usr/local/bin" / name
            path.write_text("fixture")
            if name != "rescue":
                path.chmod(0o755)
        with self.assertRaises(RuntimeError):
            live.check_programs(stage)
        (stage / "usr/local/bin/rescue").chmod(0o755)
        live.check_programs(stage)

    def test_fetches_over_https_and_ssh_only(self):
        del self.env["GIT_ALLOW_PROTOCOL"]
        out, _ = self.entry("--yes")
        self.assertTrue(out.startswith(f"bundled bundled: could not fetch {self.URL} (main): "), out)
        self.assertIn("transport 'file' not allowed", out)


if __name__ == "__main__":
    unittest.main()
