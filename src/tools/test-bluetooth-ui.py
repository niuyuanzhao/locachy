#!/usr/bin/env python3
"""Isolated bluetui 0.8.2 PTY/LOCachy term regression tests.

Uses python-dbusmock's bluez5 template for adapters, devices and actions.
Agent1 callbacks are injected by that mock's own bus connection, not a real
controller or authentication exchange. No custom BlueZ protocol is provided.
"""

import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import termios
import time

import dbus
import dbus.mainloop.glib
from dbusmock import BusType, MOCK_IFACE, PrivateDBus, SpawnedMock
from gi.repository import GLib

BLUEZ_MOCK = "org.bluez.Mock"
DEVICE = "org.bluez.Device1"
TEST_IFACE = "org.locachy.BluetoothUITest"
ADDRESS = "AA:BB:CC:DD:EE:01"
NEW_ADDRESS = "AA:BB:CC:DD:EE:02"
PAIRED_NAME = "LOCachy Keyboard"
NEW_NAME = "LOCachy Phone"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def dispatch():
    context = GLib.MainContext.default()
    while context.pending():
        context.iteration(False)


class UI:
    def __init__(self, out, grid, name, width=100, height=30):
        self.out, self.grid, self.name = out, grid, name
        self.width, self.height = width, height
        self.raw = bytearray()
        self.replied = 0
        self.home = out / name
        self.home.mkdir()
        pid, master = os.forkpty()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
            os.environ.update(HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / "config"),
                              XDG_DATA_HOME=str(self.home / "data"), TERM="xterm-256color")
            os.execv("/usr/bin/bluetui", ["/usr/bin/bluetui"])
        self.pid, self.master = pid, master
        self.exited = False
        os.set_blocking(master, False)

    def pump(self, duration=0.08):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            dispatch()
            readable, _, _ = select.select([self.master], [], [], min(0.02, max(0, end - time.monotonic())))
            if readable:
                try:
                    data = os.read(self.master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        return
                    raise
                if not data:
                    return
                self.raw.extend(data)
                self.render()

    def render(self):
        rendered = subprocess.run([self.grid, str(self.width), str(self.height)],
                                  input=self.raw, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, check=True, timeout=3)
        replies = rendered.stderr[self.replied:]
        if replies:
            os.write(self.master, replies)
        self.replied = len(rendered.stderr)
        return rendered.stdout.decode("utf-8")

    def screen(self):
        return self.render()

    def save(self, label):
        stem = self.out / f"{self.name}-{label}"
        stem.with_suffix(".pty").write_bytes(self.raw)
        text = self.screen()
        stem.with_suffix(".grid").write_text(text)
        return text

    def wait(self, *texts, label, timeout=6, status=None):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            screen = self.screen()
            status_ok = status is None or any(status in " ".join(line.split()) for line in screen.splitlines())
            if all(text in screen for text in texts) and status_ok:
                return self.save(label)
        screen = self.save(f"FAIL-{label}")
        raise AssertionError(f"{self.name}/{label}: missing {texts!r} or row {status!r} in real term grid:\n{screen}")

    def key(self, data):
        os.write(self.master, data)
        self.pump()

    def close(self):
        if self.exited:
            return
        try:
            # UnpairConfirmation stays focused after RemoveDevice in 0.8.2;
            # Esc dismisses it before the global quit shortcut is available.
            self.key(b"\x1b")
            self.pump(0.15)
            self.key(b"\x03")
            end = time.monotonic() + 3
            while time.monotonic() < end:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
                if pid:
                    self.exited = True
                    check(os.waitstatus_to_exitcode(status) == 0,
                          f"bluetui exit status {os.waitstatus_to_exitcode(status)}")
                    return
                self.pump(0.02)
            raise AssertionError("bluetui did not exit after Ctrl+C")
        finally:
            if not self.exited:
                try:
                    os.kill(self.pid, signal.SIGKILL)
                    os.waitpid(self.pid, 0)
                except ProcessLookupError:
                    pass
            self.exited = True
            os.close(self.master)


def property_is(bus, path, name, value, ui):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        result = bus.get_object("org.bluez", path).Get(DEVICE, name, dbus_interface=dbus.PROPERTIES_IFACE)
        if bool(result) == value:
            return
        ui.pump()
    ui.save(f"FAIL-{name}-{value}")
    raise AssertionError(f"{path}: {name} did not become {value}")


def method_calls(bus, path, method):
    return bus.get_object("org.bluez", path).GetMethodCalls(method, dbus_interface=MOCK_IFACE)


def install_agent_injector(root):
    # Keep the standard template intact. The only added behavior invokes an
    # already-registered application Agent1 from org.bluez's real connection.
    root.AddMethod(TEST_IFACE, "AgentCall", "soosav", "", """
self.agent_done = False
self.agent_value = '[]'
self.agent_error = ''
signatures = {'RequestConfirmation': 'ou', 'RequestPinCode': 'o',
              'RequestPasskey': 'o', 'DisplayPinCode': 'os', 'DisplayPasskey': 'ouq'}
def success(*values, self=self):
    self.agent_value = __import__('json').dumps(list(values))
    self.agent_done = True
def failure(error, self=self):
    self.agent_error = error.get_dbus_name()
    self.agent_done = True
self.connection.call_async(str(args[0]), str(args[1]), 'org.bluez.Agent1',
    str(args[3]), signatures[str(args[3])], [args[2]] + list(args[4]),
    success, failure, timeout=15)
""", dbus_interface=MOCK_IFACE)
    root.AddMethod(TEST_IFACE, "AgentResult", "", "bss",
                   "ret = (self.agent_done, self.agent_value, self.agent_error)",
                   dbus_interface=MOCK_IFACE)


def agent_call(root, agent, path, method, *args):
    root.AgentCall(agent[0], dbus.ObjectPath(agent[1]), dbus.ObjectPath(path), method,
                   dbus.Array(args, signature="v"), dbus_interface=TEST_IFACE)


def agent_result(root, ui, expected=None, error=""):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        done, value, failure = root.AgentResult(dbus_interface=TEST_IFACE)
        if done:
            check(str(failure) == error, f"Agent error {failure!r}, expected {error!r}")
            if expected is not None:
                check(json.loads(value) == expected, f"Agent reply {value!r}, expected {expected!r}")
            return
        ui.pump()
    ui.save("FAIL-agent-result")
    raise AssertionError("Agent callback has not completed")


def run(args):
    out, grid = Path(args.out).resolve(), str(Path(args.grid).resolve())
    package = subprocess.check_output(["pacman", "-Q", "bluetui", "python-dbusmock"], text=True)
    version = subprocess.check_output(["/usr/bin/bluetui", "--version"], text=True).strip()
    help_text = subprocess.check_output(["/usr/bin/bluetui", "--help"], text=True)
    check(version == "bluetui 0.8.2", f"Expected exact bluetui 0.8.2, got {version!r}")
    check("--ascii" not in help_text, "Unexpected --ascii option: re-check the package version")
    (out / "versions.txt").write_text(package + version + "\n" + help_text)
    print(package.strip(), flush=True)
    print("SIMULATION: private D-Bus, bluez5 template and injected Agent1 callbacks; no physical Bluetooth", flush=True)
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    with PrivateDBus(BusType.SYSTEM) as private:
        bus = dbus.bus.BusConnection(private.address)
        monitor = dbus.bus.BusConnection(private.address)
        registrations = []
        connection_signals = []

        def connected_changed(interface, changed, _invalidated, path):
            if interface != DEVICE or "Connected" not in changed:
                return
            connection_signals.append((str(path), bool(changed["Connected"])))
            # bluez5 in dbusmock 0.38.1 changes device.connected and emits
            # Connected, but does not update props[DEVICE]['Connected'].
            # Reflect the template's own signal into its property store so
            # bluetui's periodic Get and subsequent Disconnect see that state.
            obj = bus.get_object("org.bluez", path)
            current = obj.Get(DEVICE, "Connected", dbus_interface=dbus.PROPERTIES_IFACE)
            if bool(current) != bool(changed["Connected"]):
                obj.UpdateProperties(DEVICE, {"Connected": changed["Connected"]}, dbus_interface=MOCK_IFACE)

        bus.add_signal_receiver(connected_changed, signal_name="PropertiesChanged",
                                dbus_interface=dbus.PROPERTIES_IFACE, bus_name="org.bluez",
                                path_keyword="path")

        def registered(_connection, message):
            if message.get_interface() == "org.bluez.AgentManager1" and message.get_member() == "RegisterAgent":
                registrations.append((str(message.get_sender()), str(message.get_args_list()[0])))

        monitor.add_message_filter(registered)
        monitor.add_match_string("type='method_call',interface='org.bluez.AgentManager1',member='RegisterAgent',eavesdrop='true'")
        with (out / "bluez-mock.log").open("w") as log:
            with SpawnedMock.spawn_with_template("bluez5", bustype=BusType.SYSTEM, stdout=log, stderr=log) as mock:
                root = mock.obj
                ui = UI(out, grid, "no-adapter", width=100)
                try:
                    ui.wait("No Bluetooth adapters found.", label="message")
                    print("PASS no-adapter message in LOCachy term", flush=True)
                finally:
                    ui.close()
                root.AddAdapter("hci0", "LOCachy Simulated Adapter", dbus_interface=BLUEZ_MOCK)
                paired = str(root.AddDevice("hci0", ADDRESS, PAIRED_NAME, dbus_interface=BLUEZ_MOCK))
                new = str(root.AddDevice("hci0", NEW_ADDRESS, NEW_NAME, dbus_interface=BLUEZ_MOCK))
                root.PairDevice("hci0", ADDRESS, dbus_interface=BLUEZ_MOCK)
                # The generic template has no Battery1 interface. Supply typed
                # fixture data using dbusmock, as on a battery-reporting device.
                for path in (paired, new):
                    bus.get_object("org.bluez", path).AddProperties(
                        "org.bluez.Battery1", {"Percentage": dbus.Byte(83)}, dbus_interface=MOCK_IFACE)
                # Include real PUA keyboard status icons without installing fonts.
                bus.get_object("org.bluez", paired).UpdateProperties(
                    DEVICE, {"Icon": dbus.String("input-keyboard")}, dbus_interface=MOCK_IFACE)
                install_agent_injector(root)
                registrations.clear()
                ui = UI(out, grid, "devices", width=100)
                try:
                    ui.wait(PAIRED_NAME, "Dis/Connect", "Unpair", label="paired-initial")
                    ui.key(b"\x1b[Z")
                    ui.wait("hci0", "LOCachy", PAIRED_NAME, "Scan on/off", "Power on/off",
                            "Pairing on/off", "Discovery on/off", label="adapter-help")
                    ui.key(b"\t")
                    screen = ui.wait(PAIRED_NAME, "Dis/Connect", "Unpair", "Un/Trust", "Rename", label="paired-help")
                    check(any(0xE000 <= ord(c) <= 0xF8FF or 0xF0000 <= ord(c) <= 0xFFFFD for c in screen),
                          "Expected upstream PUA icons in rendered grid")
                    print("PASS paired device and contextual key help survive PUA icons", flush=True)
                    ui.key(b"\r")
                    property_is(bus, paired, "Connected", True, ui)
                    ui.wait("Device connected", PAIRED_NAME, label="connected")
                    check(len(method_calls(bus, paired, "Connect")) == 1, "Connect method not called once")
                    ui.key(b"\r")
                    property_is(bus, paired, "Connected", False, ui)
                    ui.wait("Device disconnected", PAIRED_NAME, label="disconnected")
                    check(len(method_calls(bus, paired, "Disconnect")) == 1, "Disconnect method not called once")
                    ui.key(b"t")
                    property_is(bus, paired, "Trusted", True, ui)
                    ui.wait("Device trusted", label="trusted")
                    ui.key(b"t")
                    property_is(bus, paired, "Trusted", False, ui)
                    ui.wait("Device untrusted", label="untrusted")
                    print("PASS Connect/Disconnect and Trust/Untrust keys change mock state", flush=True)
                    ui.key(b"s")
                    ui.key(b"\t")
                    ui.wait(NEW_NAME, "Pair", "Scan on/off", label="unpaired-help")
                    ui.key(b"\r")
                    property_is(bus, new, "Paired", True, ui)
                    property_is(bus, new, "Trusted", True, ui)
                    property_is(bus, new, "Connected", True, ui)
                    check(len(method_calls(bus, new, "Pair")) == 1, "Pair method not called once")
                    ui.wait(NEW_NAME, "Device paired", label="paired-new")
                    print("PASS unpaired display and Pair key (standard template auto-pair/trust/connect)", flush=True)
                    end = time.monotonic() + 3
                    while not registrations and time.monotonic() < end:
                        ui.pump()
                    check(registrations, "No Agent1 registration observed")
                    agent = registrations[-1]
                    agent_call(root, agent, paired, "RequestPinCode")
                    ui.wait("Enter the PIN Code for the device", ADDRESS, "Pin Code", "Submit", "Discard", label="pin-request")
                    ui.key(b"004271")
                    ui.wait("004271", label="pin-typed")
                    ui.key(b"\t\r")
                    agent_result(root, ui, ["004271"])
                    agent_call(root, agent, paired, "DisplayPinCode", dbus.String("004271"))
                    ui.wait("Pin Code for the device", ADDRESS, "004271", label="pin-display")
                    ui.key(b"\r")
                    agent_result(root, ui, [])
                    print("PASS simulated PIN input/display preserves leading zeros and Agent1 reply", flush=True)
                    agent_call(root, agent, paired, "RequestPasskey")
                    ui.wait("Enter the Passkey for the device", ADDRESS, "Submit", label="passkey-request")
                    ui.key(b"012345")
                    ui.wait("012345", label="passkey-typed")
                    ui.key(b"\t\r")
                    agent_result(root, ui, [12345])
                    agent_call(root, agent, paired, "RequestConfirmation", dbus.UInt32(4271))
                    ui.wait("Authentication required for the device", ADDRESS, "Confirm Passkey 004271", "No", "Yes", "Discard", label="confirmation-yes")
                    ui.key(b"\r")
                    agent_result(root, ui, [])
                    agent_call(root, agent, paired, "RequestConfirmation", dbus.UInt32(123456))
                    ui.wait("Confirm Passkey 123456", label="confirmation-no")
                    ui.key(b"\t\r")
                    agent_result(root, ui, error="org.bluez.Error.Rejected")
                    print("PASS simulated passkey input and six-digit confirmation accept/reject", flush=True)
                    # Agent completion restores paired-list focus. The unpair
                    # popup initially selects No; Tab explicitly selects Yes.
                    ui.wait("Dis/Connect", "Unpair", label="before-remove")
                    ui.key(b"u")
                    ui.wait("unpair", "No", "Yes", label="remove-confirmation")
                    ui.key(b"\t\r")
                    end = time.monotonic() + 5
                    while time.monotonic() < end:
                        calls = method_calls(bus, "/org/bluez/hci0", "RemoveDevice")
                        if calls:
                            break
                        ui.pump()
                    check(calls, "RemoveDevice not called")
                    removed = str(calls[-1][1][0])
                    objects = root.GetManagedObjects(dbus_interface="org.freedesktop.DBus.ObjectManager")
                    check(removed not in objects, "Removed device remains in object manager")
                    ui.wait("Device unpaired", label="removed")
                    print("PASS explicit Unpair confirmation calls RemoveDevice", flush=True)
                finally:
                    ui.close()
        monitor.close()
        bus.close()
    print(f"ALL PASSED; artifacts: {out}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--grid", required=True)
    parser.add_argument("--out", required=True)
    run(parser.parse_args())
