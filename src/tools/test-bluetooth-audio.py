#!/usr/bin/env python3
"""Real PipeWire/WirePlumber against the standard BlueZ dbusmock template.

No audio device or Bluetooth controller is used. Media application registration
checks policy/backend integration, not physical playback, microphone or pairing.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import time

import dbus
from dbusmock import DBusTestCase


class BluetoothAudio(DBusTestCase):
    def setUp(self):
        self.start_system_bus()
        self.start_session_bus()
        self.out = Path(tempfile.mkdtemp(prefix="bluetooth-audio-", dir=os.environ.get("TEST_OUTPUT", "/var/tmp")))
        self.children = []
        self.files = []
        self.runtime = self.out / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.config = self.out / "config"
        wp = self.config / "wireplumber" / "wireplumber.conf.d"
        wp.mkdir(parents=True)
        source = Path(__file__).resolve().parents[1]
        (wp / "50-locachy-bluetooth.conf").write_bytes((source / "link/50-locachy-bluetooth.conf").read_bytes())
        # Only the Bluetooth backend is under test: do not open the host sound
        # or video devices even in an accidentally unisolated invocation.
        (wp / "99-test-devices.conf").write_text("""
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
    monitor.bluez-midi = disabled
    support.reserve-device = disabled
    support.portal-permissionstore = disabled
    script.client.access-portal = disabled
  }
}
""")
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.runtime),
                        XDG_CONFIG_HOME=str(self.config), XDG_STATE_HOME=str(self.out / "state"),
                        XDG_CACHE_HOME=str(self.out / "cache"), WIREPLUMBER_DEBUG="4")
        self.bus = dbus.SystemBus()
        self.bluez = None
        self.adapter = "/org/bluez/hci0"
        self.addCleanup(self.cleanup_processes)
        self.start_bluez()
        self.start("pipewire")
        self.wait(lambda: (self.runtime / "pipewire-0").is_socket(), "PipeWire socket")
        self.wp = self.start("wireplumber")

    def cleanup_processes(self):
        for proc in reversed(self.children):
            if proc.poll() is None:
                proc.terminate()
        for proc in reversed(self.children):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for stream in self.files:
            stream.close()
        print("Bluetooth audio logs: " + str(self.out), flush=True)

    def start(self, name):
        stream = (self.out / (name + ".log")).open("wb")
        self.files.append(stream)
        proc = subprocess.Popen([name], env=self.env, stdout=stream, stderr=subprocess.STDOUT)
        self.children.append(proc)
        return proc

    def start_bluez(self):
        stream = (self.out / ("bluez-" + str(len(self.children)) + ".log")).open("wb")
        self.files.append(stream)
        # Publish org.bluez only once ObjectManager and its template are
        # ready. dbusmock normally acquires the name before loading the
        # template, which is a startup race for a running PipeWire monitor.
        name = "org.locachy.BluetoothAudioMock"
        proc = self.spawn_server(name, "/", "org.freedesktop.DBus.ObjectManager", system_bus=True, stdout=stream)
        obj = self.bus.get_object(name, "/")
        self.children.append(proc)
        self.bluez = proc
        root = dbus.Interface(obj, "org.freedesktop.DBus.Mock")
        root.AddTemplate("bluez5", {})
        dbus.Interface(obj, "org.bluez.Mock").AddAdapter("hci0", "LOCachy test adapter")
        # The standard template defines media methods but omits Media1 from
        # ObjectManager until it has properties. Real BlueZ exports this.
        adapter = self.bus.get_object(name, self.adapter)
        mock = dbus.Interface(adapter, "org.freedesktop.DBus.Mock")
        mock.AddProperties("org.bluez.Media1", {"SupportedUUIDs": dbus.Array([], signature="s")})
        # PipeWire 1.6 uses BlueZ application registration, not individual
        # RegisterEndpoint calls from older versions of the template.
        mock.AddMethods("org.bluez.Media1", [
            ("RegisterApplication", "oa{sv}", "", ""),
            ("UnregisterApplication", "o", "", ""),
        ])
        # Announce the additional interface for a running monitor, not only
        # for its initial GetManagedObjects snapshot.
        root.EmitSignal("org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "oa{sa{sv}}",
                        [dbus.ObjectPath(self.adapter), {"org.bluez.Media1": {
                            "SupportedUUIDs": dbus.Array([], signature="s")}}])
        root.AddMethod("org.freedesktop.DBus.Mock", "PublishBlueZ", "", "",
                       "self.bluez_name = dbus.service.BusName('org.bluez', self.connection, do_not_queue=True)")
        root.PublishBlueZ()

    def wait(self, check, message):
        until = time.monotonic() + 15
        while time.monotonic() < until:
            try:
                if check():
                    return
            except dbus.exceptions.DBusException:
                pass
            time.sleep(0.1)
        raise AssertionError("Timed out waiting for " + message)

    def registrations(self):
        obj = self.bus.get_object("org.bluez", self.adapter)
        return dbus.Interface(obj, "org.freedesktop.DBus.Mock").GetCalls("RegisterApplication")

    def test_media_registration_survives_bluez_restart(self):
        self.wait(lambda: len(self.registrations()) > 0, "Bluetooth media registration without logind")
        first = len(self.registrations())
        self.assertIsNone(self.wp.poll())
        print(f"PASS real WirePlumber/PipeWire registered {first} Bluetooth media applications without logind", flush=True)
        self.bluez.terminate()
        self.bluez.wait(timeout=5)
        self.wait(lambda: not self.bus.name_has_owner("org.bluez"), "old BlueZ name release")
        self.start_bluez()
        self.wait(lambda: len(self.registrations()) > 0, "media application registration after BlueZ restart")
        self.assertIsNone(self.wp.poll())
        second = len(self.registrations())
        print(f"PASS BlueZ restart re-registers {second} media applications without restarting WirePlumber", flush=True)


if __name__ == "__main__":
    import unittest
    unittest.main()
