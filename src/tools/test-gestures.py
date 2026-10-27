#!/usr/bin/env python3
"""Gesture hotplug glue with udev/process fixtures, no input or display access.
  python3 -B tools/test-gestures.py SCRATCH_PARENT
"""
import importlib.machinery
import importlib.util
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("gesture_glue", str(SOURCE / "link/gestures"))
spec = importlib.util.spec_from_loader(loader.name, loader)
glue = importlib.util.module_from_spec(spec)
loader.exec_module(glue)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None


class Device:
    def __init__(self, path, node=None, touch=True, action="add", initialized="100"):
        self.sys_path = path
        self.device_node = node
        self.action = action
        self.touch = touch
        self.initialized = initialized

    def get(self, key):
        if key == "USEC_INITIALIZED":
            return self.initialized
        return "1" if key == "ID_INPUT_TOUCHSCREEN" and self.touch else None


class Context:
    def __init__(self):
        self.devices = []
        self.monitor = None

    def list_devices(self, *, subsystem):
        if self.monitor is not None:
            assert self.monitor.started, "snapshot taken before subscribing"
        assert subsystem == "input"
        return list(self.devices)


class Child:
    def __init__(self, args, **kwargs):
        self.args = args
        self.kwargs = kwargs
        self.status = None
        self.terminated = self.waited = self.killed = 0
        self.stubborn = False

    def poll(self):
        return self.status

    def terminate(self):
        self.terminated += 1
        if not self.stubborn:
            self.status = -signal.SIGTERM

    def wait(self, timeout=None):
        self.waited += 1
        if self.status is None:
            raise subprocess.TimeoutExpired(self.args, timeout)
        return self.status

    def kill(self):
        self.killed += 1
        self.status = -signal.SIGKILL


class Gestures(unittest.TestCase):
    def setUp(self):
        self.context = Context()
        self.children = []
        self.glue = glue.Gestures(self.popen, lambda name: "/usr/bin/" + name)
        self.addCleanup(self.glue.close)

    def popen(self, args, **kwargs):
        child = Child(args, **kwargs)
        self.children.append(child)
        return child

    def sync(self, *devices):
        self.context.devices = devices
        self.glue.sync(self.context)

    def screens(self):
        return [c for c in self.children if c.args[0].endswith("/lisgd")]

    def test_initial_devices_and_duplicate_change(self):
        one = Device("/sys/devices/one/event5", "/dev/input/event5")
        two = Device("/sys/devices/two/event6", "/dev/input/event6")
        self.sync(one, two, Device("/sys/mouse", "/dev/input/event7", touch=False),
                  Device("/sys/input", "/dev/input/mouse0"), Device("/sys/input2", None))
        self.assertEqual(len(self.children), 3)
        self.sync(one, two)
        self.assertEqual(len(self.children), 3)
        touchpad = self.children[0]
        self.assertEqual(touchpad.args, ["/usr/bin/libinput-gestures", "-c", glue.CONFIG])
        for child in self.screens():
            self.assertNotIn("start_new_session", child.kwargs)
            self.assertNotIn("preexec_fn", child.kwargs)
            self.assertEqual(child.kwargs["stdin"], subprocess.DEVNULL)
            self.assertEqual(child.args[3], "-g")
            self.assertIn(" --show &", child.args[4])
            self.assertIn(" --hide &", child.args[6])

    def test_tag_delayed_until_change(self):
        one = Device("/sys/one", "/dev/input/event5", touch=False)
        self.sync(one)
        self.assertEqual(self.screens(), [])
        one.touch = True
        self.sync(one)
        self.assertEqual(len(self.screens()), 1)
        one.touch = False
        self.sync(one)
        self.assertEqual(self.screens()[0].terminated, 1)
        self.assertFalse(self.glue.screens)

    def test_remove_without_properties_only_stops_matching_lisgd(self):
        one = Device("/sys/one", "/dev/input/event5")
        two = Device("/sys/two", "/dev/input/event6")
        self.sync(one, two)
        first, second = self.screens()
        self.glue.remove(one.sys_path)
        self.assertEqual((first.terminated, first.waited), (1, 1))
        self.assertEqual(second.terminated, 0)
        self.assertEqual(self.children[0].terminated, 0)
        self.glue.remove(one.sys_path)  # Duplicate remove is harmless.
        self.assertEqual(first.terminated, 1)

    def test_event_node_and_sys_path_reuse(self):
        old = Device("/sys/old", "/dev/input/event5")
        self.sync(old)
        first = self.screens()[0]
        new = Device("/sys/new", "/dev/input/event5")
        self.sync(new)
        self.assertEqual(first.terminated, 1)
        self.assertEqual(len(self.glue.screens), 1)
        second = self.screens()[-1]
        new.initialized = "200"  # Same path/node, new udev incarnation.
        self.sync(new)
        self.assertEqual(second.terminated, 1)
        self.assertEqual(len(self.glue.screens), 1)
        new.device_node = "/dev/input/event8"
        self.sync(new)
        self.assertEqual(self.screens()[-1].args[2], "/dev/input/event8")
        self.assertEqual(len(self.glue.screens), 1)

    def test_dead_children_are_reaped_and_restarted(self):
        one = Device("/sys/one", "/dev/input/event5")
        self.sync(one)
        pad, screen = self.children
        pad.status = screen.status = 3
        self.sync(one)
        self.assertEqual((pad.waited, screen.waited), (1, 1))
        self.assertEqual(len(self.children), 4)
        self.sync()
        self.assertFalse(self.glue.screens)

    def test_cleanup_reaps_even_stubborn_child_without_group_kill(self):
        self.sync(Device("/sys/one", "/dev/input/event5"))
        screen = self.screens()[0]
        screen.stubborn = True
        self.glue.close()
        self.assertEqual((screen.terminated, screen.killed, screen.waited), (1, 1, 2))
        self.assertTrue(all(c.waited for c in self.children))
        self.assertFalse(self.glue.screens)
        self.assertIsNone(self.glue.touchpad)

    def test_absent_tools_do_not_start_other_programs(self):
        self.glue.close()
        self.glue = glue.Gestures(self.popen, lambda name: None)
        self.sync(Device("/sys/one", "/dev/input/event5"))
        self.assertEqual(self.children, [])

    def test_subscribe_before_snapshot_and_ignore_old_queued_removal(self):
        one = Device("/sys/one", "/dev/input/event5")
        self.context.devices = [one]
        context = self.context
        steps = []

        class Monitor:
            started = False
            done = False

            def filter_by(self, **kwargs):
                steps.append(("filter", kwargs))

            def start(self):
                self.started = True
                steps.append("start")

            def poll(self, timeout):
                if len(steps) == 2:
                    steps.append("remove")
                    return Device(one.sys_path, action="remove")
                self.done = True
                return None

        monitor = Monitor()
        context.monitor = monitor
        glue.run(context, monitor, lambda: monitor.done, self.popen, lambda name: "/usr/bin/" + name)
        self.assertEqual(steps, [("filter", {"subsystem": "input"}), "start", "remove"])
        self.assertEqual(len(self.screens()), 1)
        self.assertTrue(all(c.terminated == 1 and c.waited == 1 for c in self.children))

    def test_exception_and_stop_both_reap_children(self):
        context = self.context
        context.devices = [Device("/sys/one", "/dev/input/event5")]

        class Monitor:
            started = False

            def filter_by(self, **kwargs):
                pass

            def start(self):
                self.started = True

            def poll(self, timeout):
                raise RuntimeError("monitor failed")

        monitor = Monitor()
        context.monitor = monitor
        with self.assertRaisesRegex(RuntimeError, "monitor failed"):
            glue.run(context, monitor, lambda: False, self.popen, lambda name: "/usr/bin/" + name)
        self.assertTrue(all(c.waited for c in self.children))

    def test_main_uses_udev_channel_and_sigterm_stops(self):
        context = object()
        monitor = object()
        handlers = {}
        calls = []
        module = mock.Mock()
        module.Context.return_value = context
        module.Monitor.from_netlink.return_value = monitor

        def run(actual_context, actual_monitor, stopped):
            calls.append((actual_context, actual_monitor))
            self.assertFalse(stopped())
            handlers[signal.SIGTERM](signal.SIGTERM, None)
            self.assertTrue(stopped())

        with mock.patch.dict(sys.modules, {"pyudev": module}), \
                mock.patch.object(glue.signal, "signal", side_effect=lambda sig, fn: handlers.setdefault(sig, fn)), \
                mock.patch.object(glue, "run", side_effect=run):
            glue.main()
        module.Monitor.from_netlink.assert_called_once_with(context, source="udev")
        self.assertEqual(calls, [(context, monitor)])
        self.assertIn(signal.SIGINT, handlers)


class OverviewLifetime(unittest.TestCase):
    def test_background_command_does_not_block_lisgd_or_kill_overview(self):
        # Real harmless subprocesses, no Wayland or input devices. A stand-in
        # shell models lisgd's system(command); the overview remains in the
        # inherited group and is cleaned up explicitly by this test.
        with tempfile.TemporaryDirectory(prefix="gesture-command-", dir=SCRATCH) as directory:
            path = Path(directory)
            pidfile = path / "overview.pid"
            command = f"{sys.executable} -c 'import os,time; print(os.getpid(),flush=True); time.sleep(30)' >{pidfile} &"
            child = subprocess.Popen(["sh", "-c", command])
            self.assertEqual(child.wait(timeout=3), 0)
            pid = int(pidfile.read_text()) if pidfile.exists() and pidfile.read_text() else None
            # The shell can finish before the background process has printed.
            if pid is None:
                import time
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    text = pidfile.read_text() if pidfile.exists() else ""
                    if text:
                        pid = int(text)
                        break
                    time.sleep(0.01)
            self.assertIsNotNone(pid)
            import os
            try:
                os.kill(pid, 0)
                self.assertEqual(os.getpgid(pid), os.getpgrp())
            finally:
                if pid is not None:
                    os.kill(pid, signal.SIGTERM)


if __name__ == "__main__":
    unittest.main()
