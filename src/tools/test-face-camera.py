#!/usr/bin/env python3
"""Generic face camera discovery/probe tests, without hardware or images."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("face_camera_tool", str(SOURCE / "link/locachy-face-camera"))
spec = importlib.util.spec_from_loader(loader.name, loader)
cam = importlib.util.module_from_spec(spec)
loader.exec_module(cam)
SCRATCH = sys.argv.pop(1) if len(sys.argv) > 1 else None
GOOD = {"status": "ok", "width": 480, "height": 360, "mean": 65.0, "std": 32.0,
        "darkness": 10.0, "dark_threshold": 60.0}


def device(path, candidate=True):
    return {"path": path, "candidate": candidate, "name": "Test camera", "formats": ["GREY"]}


class Camera(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="face-camera-", dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.config = self.root / "config.ini"
        self.config.write_text("[video]\ndevice_path = /missing\nrecording_plugin = opencv\n")
        patch = mock.patch.object(cam, "HOWDY_CONF", str(self.config))
        patch.start()
        self.addCleanup(patch.stop)

    def enumeration(self, kind):
        def ioctl(fd, request, buffer, mutate):
            self.assertEqual(fd, 17)
            self.assertTrue(mutate)
            if request == 0x80685600:
                capability = 0x80000000
                device_caps = 0x04a00000 if kind == "metadata" else 0x1000 if kind == "multiplane" else 1
                buffer[:] = struct.pack("=16s32s32s6I", b"uvcvideo", b"IR Camera", b"usb-test", 1,
                                        capability, device_caps, 0, 0, 0)
            else:
                index = struct.unpack_from("=I", buffer)[0]
                if index:
                    raise OSError("no more formats")
                buffer[44:48] = b"YUYV" if kind == "colour" else b"GREY"
        with mock.patch.object(cam.os, "stat", return_value=types.SimpleNamespace(st_mode=0o20600)), \
                mock.patch.object(cam.os, "open", return_value=17), mock.patch.object(cam.os, "close") as close, \
                mock.patch.object(cam.fcntl, "ioctl", side_effect=ioctl):
            result = cam.capture_device("/dev/video128")
        close.assert_called_once_with(17)
        return result

    def test_capabilities_not_device_numbers_distinguish_cameras(self):
        self.assertIsNone(self.enumeration("metadata"))
        ir = self.enumeration("grey")
        self.assertEqual(ir["path"], "/dev/video128")
        self.assertTrue(ir["candidate"])
        self.assertEqual(ir["name"], "IR Camera")
        self.assertFalse(self.enumeration("colour")["candidate"])
        self.assertTrue(self.enumeration("multiplane")["candidate"])

    def test_sparse_high_numbered_nodes_and_numeric_order(self):
        paths = ["/dev/video128", "/dev/video2", "/dev/video3", "/dev/video0", "/dev/videotext"]
        def capture(path):
            if path == "/dev/video0":
                raise OSError("absent")
            return None if path == "/dev/video3" else device(path)
        with mock.patch.object(cam.glob, "glob", return_value=paths), \
                mock.patch.object(cam, "capture_device", side_effect=capture):
            self.assertEqual([d["path"] for d in cam.devices()], ["/dev/video2", "/dev/video128"])

    def test_automatic_uses_emitter_candidates_and_tests_before_saving(self):
        found = [device("/dev/video0", False), device("/dev/video128")]
        listing = subprocess.CompletedProcess([], 0, "/dev/video128\n  unit=9 selector=1\n", "")
        before = self.config.read_bytes()
        with mock.patch.object(cam.subprocess, "run", return_value=listing) as run, \
                mock.patch.object(cam, "probe_frame", return_value=GOOD) as probe, contextlib.redirect_stdout(io.StringIO()):
            selected = cam.automatic(found)
        self.assertEqual(selected["path"], "/dev/video128")
        probe.assert_called_once_with("/dev/video128")
        self.assertEqual(run.call_args.args[0], [cam.EMITTER, "--grey-devices"])
        self.assertEqual(before, self.config.read_bytes())

    def test_zero_candidates_never_selects_colour_camera(self):
        with mock.patch.object(cam.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")), \
                mock.patch.object(cam, "probe_frame") as probe, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "no colour fallback"):
                cam.automatic([device("/dev/video0", False)])
        probe.assert_not_called()

    def test_multiple_working_candidates_require_choice(self):
        listing = subprocess.CompletedProcess([], 0, "/dev/video2\n/dev/video128\n", "")
        with mock.patch.object(cam.subprocess, "run", return_value=listing), \
                mock.patch.object(cam, "probe_frame", return_value=GOOD), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "More than one"):
                cam.automatic([device("/dev/video2"), device("/dev/video128")])

    def test_working_configured_candidate_is_preserved(self):
        self.config.write_text("[video]\ndevice_path=/dev/video128\n")
        listing = subprocess.CompletedProcess([], 0, "/dev/video2\n/dev/video128\n", "")
        with mock.patch.object(cam.subprocess, "run", return_value=listing), \
                mock.patch.object(cam, "probe_frame", return_value=GOOD) as probe, contextlib.redirect_stdout(io.StringIO()):
            result = cam.automatic([device("/dev/video2"), device("/dev/video128")])
        self.assertEqual(result["path"], "/dev/video128")
        probe.assert_called_once_with("/dev/video128")

    def test_failed_and_dark_cameras_are_not_saved(self):
        for status in ("error", "timeout", "dark"):
            result = dict(GOOD, status=status, detail="test")
            args = types.SimpleNamespace(command="test", device="/dev/video2", apply=True, user="test")
            with mock.patch.object(cam, "devices", return_value=[device("/dev/video2")]), \
                    mock.patch.object(cam, "probe_frame", return_value=result), \
                    mock.patch.object(cam, "apply_device") as apply, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cam.run(args), 1)
            apply.assert_not_called()

    def test_probe_timeout_kills_its_entire_process_group(self):
        process = mock.Mock(pid=9001)
        process.communicate.side_effect = [subprocess.TimeoutExpired("probe", 12), ("", "")]
        with mock.patch.object(cam.subprocess, "Popen", return_value=process) as popen, \
                mock.patch.object(cam.os, "killpg") as kill:
            self.assertEqual(cam.probe_frame("/dev/video2")["status"], "timeout")
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        kill.assert_called_once_with(9001, cam.signal.SIGKILL)

    def test_probe_result_reports_error_or_dark_frames(self):
        for output, rc, expected in ((json.dumps(GOOD), 0, "ok"), ("garbage", 0, "error"),
                                     ("", 1, "error"), (json.dumps(dict(GOOD, darkness=90)), 0, "dark")):
            process = mock.Mock(returncode=rc)
            process.communicate.return_value = (output, "failure")
            with mock.patch.object(cam.subprocess, "Popen", return_value=process):
                self.assertEqual(cam.probe_frame("/dev/video2")["status"], expected)

    def test_child_uses_howdy_backend_in_memory_and_releases(self):
        frame = mock.Mock(shape=(360, 480))
        frame.mean.return_value = 64.0
        frame.std.return_value = 30.0
        capture = mock.Mock()
        capture.read_frame.return_value = (frame, frame)
        backend = mock.Mock(return_value=capture)
        module = types.SimpleNamespace(VideoCapture=backend)
        class Histogram(list):
            def sum(self): return 100
            def ravel(self): return self
        cv2 = types.SimpleNamespace(calcHist=lambda *args: Histogram([10]))
        before = self.config.read_bytes()
        with mock.patch.dict(sys.modules, {"recorders": types.ModuleType("recorders"), "recorders.video_capture": module, "cv2": cv2}), \
                mock.patch.object(sys, "path", sys.path.copy()), contextlib.redirect_stdout(io.StringIO()) as out:
            cam.probe_child("/dev/video128")
        self.assertEqual(backend.call_args.args[0].get("video", "device_path"), "/dev/video128")
        capture.release.assert_called_once()
        self.assertEqual(json.loads(out.getvalue())["width"], 480)
        self.assertEqual(self.config.read_bytes(), before)

    def test_add_preserves_explicit_camera_and_does_not_run_if_bad(self):
        self.config.write_text("[video]\ndevice_path=/dev/video0\n")
        args = types.SimpleNamespace(command="add", user="test")
        with mock.patch.object(cam, "devices", return_value=[device("/dev/video0", False)]), \
                mock.patch.object(cam, "probe_frame", return_value=GOOD), \
                mock.patch.object(cam, "automatic") as auto, mock.patch.object(cam.subprocess, "call", return_value=0) as call, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cam.run(args), 0)
        auto.assert_not_called()
        call.assert_called_once_with([cam.HOWDY, "-U", "test", "add"])

    def test_add_does_not_enroll_after_capture_failure(self):
        self.config.write_text("[video]\ndevice_path=/dev/video128\n")
        args = types.SimpleNamespace(command="add", user="test")
        with mock.patch.object(cam, "devices", return_value=[device("/dev/video128")]), \
                mock.patch.object(cam, "probe_frame", return_value={"status": "error", "detail": "busy"}), \
                mock.patch.object(cam.subprocess, "call") as call, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "configured camera failed"):
                cam.run(args)
        call.assert_not_called()

    def test_subprocess_uses_backend_and_does_not_rewrite_config(self):
        library = self.root / "howdy"
        recorders = library / "recorders"
        recorders.mkdir(parents=True)
        (recorders / "__init__.py").write_text("")
        (library / "cv2.py").write_text('class Histogram(list):\n    def sum(self): return 100\n'
                                         '    def ravel(self): return self\n'
                                         'def calcHist(*args): return Histogram([10])\n')
        (recorders / "video_capture.py").write_text(
            'class Frame:\n    shape=(360,480)\n    def mean(self): return 64.0\n    def std(self): return 30.0\n'
            'class VideoCapture:\n    def __init__(self,config):\n'
            '        assert config.get("video","device_path")=="/dev/video128"\n'
            '    def read_frame(self): return Frame(),Frame()\n'
            '    def release(self): pass\n')
        script = self.root / "camera"
        script.write_text((SOURCE / "link/locachy-face-camera").read_text()
                          .replace('HOWDY_LIB = "/usr/lib/howdy"', 'HOWDY_LIB = ' + repr(str(library)))
                          .replace('HOWDY_CONF = "/etc/howdy/config.ini"', 'HOWDY_CONF = ' + repr(str(self.config))))
        before = self.config.read_bytes()
        result = subprocess.run([sys.executable, "-I", "-B", str(script), "--probe", "/dev/video128"],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["width"], 480)
        self.assertEqual(before, self.config.read_bytes())
        self.assertFalse(list(library.rglob("*.pyc")))

    def test_probe_handles_real_opencv_histogram_shape(self):
        try:
            import cv2
            import numpy
        except ImportError:
            self.skipTest("OpenCV is not installed in this test environment")
        frame = numpy.full((360, 480), 64, dtype=numpy.uint8)
        capture = mock.Mock()
        capture.read_frame.return_value = (frame, frame)
        module = types.SimpleNamespace(VideoCapture=mock.Mock(return_value=capture))
        with mock.patch.dict(sys.modules, {"recorders": types.ModuleType("recorders"), "recorders.video_capture": module}), \
                mock.patch.object(sys, "path", sys.path.copy()), contextlib.redirect_stdout(io.StringIO()) as out:
            cam.probe_child("/dev/video128")
        result = json.loads(out.getvalue())
        self.assertEqual(result["width"], 480)
        self.assertEqual(result["darkness"], 0)
        capture.release.assert_called_once()

    def test_apply_uses_howdy_and_verifies_persisted_choice(self):
        def write_config(command, **kwargs):
            self.assertEqual(command, [cam.HOWDY, "-U", "test", "set", "device_path", "/dev/video128"])
            self.config.write_text("[video]\ndevice_path=/dev/video128\n")
        with mock.patch.object(cam.subprocess, "run", side_effect=write_config), contextlib.redirect_stdout(io.StringIO()):
            cam.apply_device(device("/dev/video128"), "test")
        self.config.write_text("[video]\ndevice_path=/missing\n")
        with mock.patch.object(cam.subprocess, "run"), self.assertRaisesRegex(RuntimeError, "did not retain"):
            cam.apply_device(device("/dev/video128"), "test")

    def test_probe_has_no_snapshot_or_image_output(self):
        text = (SOURCE / "link/locachy-face-camera").read_text()
        self.assertNotIn("imwrite", text)
        self.assertNotIn('"snapshot"', text)
        self.assertIn('"$src/link/locachy-face-camera" /usr/local/bin/locachy-face-camera', (SOURCE / "make.sh").read_text())


if __name__ == "__main__":
    unittest.main()
