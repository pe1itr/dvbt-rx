"""SRT supervision and real loopback Full HD transcoding; no SDR or display."""
import configparser
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "build/linrad_dvb"
SRT_FFMPEG = os.environ.get("SRT_TEST_FFMPEG", "ffmpeg")


def wait_for(predicate, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for condition")


def packet(pid=256, cc=0, video=True, tei=False):
    payload = b"\x00\x00\x01\xe0\x00\x00\x80\x00\x00" if video else b""
    return bytes((0x47, (pid >> 8) | 0x40 | (0x80 if tei else 0), pid & 255, 0x10 | (cc & 15))) + payload + bytes(184 - len(payload))


def stop(p):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=4)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    for stream in (p.stdin, p.stdout, p.stderr):
        if stream:
            stream.close()


class SrtTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)

    def fake(self, behavior="read"):
        script = self.directory / "ffmpeg"
        script.write_text("#!/usr/bin/env python3\n"
            "import json, os, pathlib, signal, sys, time\n"
            f"root = pathlib.Path({str(self.directory)!r})\n"
            "with (root / 'calls').open('a') as f: f.write(str(os.getpid()) + '\\n')\n"
            "(root / 'args').write_text(json.dumps(sys.argv[1:]))\n" + {
                "read": "while os.read(0, 65536): pass\n",
                "fail": "sys.exit(7)\n",
                "stall": "signal.signal(signal.SIGTERM, signal.SIG_IGN)\ntime.sleep(60)\n",
            }[behavior])
        script.chmod(0o755)
        return script

    def calls(self):
        file = self.directory / "calls"
        return file.read_text().splitlines() if file.exists() else []

    def worker(self, ffmpeg=None, url="srt://127.0.0.1:4001", loss=300, options=()):
        log = (self.directory / "worker.log").open("w+")
        self.addCleanup(log.close)
        args = [UI, "--srt-worker", "--url", url, "--loss-ms", str(loss), "--retry-ms", "200"]
        args += ["--ffmpeg", ffmpeg or SRT_FFMPEG]
        args += options
        p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=log, bufsize=0,
                             env={**os.environ, "DISPLAY": "", "WAYLAND_DISPLAY": ""})
        self.addCleanup(stop, p)
        return p

    def send(self, p, data):
        os.write(p.stdin.fileno(), data)

    def test_gate_loss_reacquisition_and_full_hd_arguments(self):
        p = self.worker(self.fake())
        self.send(p, b"noise" + packet(8191, video=False) * 4 + packet(0, video=False) * 4 + packet(tei=True) * 4)
        time.sleep(0.25)
        self.assertEqual(self.calls(), [])
        self.send(p, b"".join(packet(cc=i) for i in range(4)))
        wait_for(lambda: len(self.calls()) == 1)
        args = json.loads((self.directory / "args").read_text())
        self.assertEqual(args[args.index("-c:v") + 1], "libx264")
        self.assertEqual(args[args.index("-c:a") + 1], "aac")
        self.assertEqual(args[args.index("-preset") + 1], "veryfast")
        self.assertIn("pad=1920:1080", args[args.index("-vf") + 1])
        self.assertIn("mode=caller", args[-1])
        self.assertNotIn("mode=listener", args[-1])
        first = self.calls()[0]
        for _ in range(10):  # PSI/null input must not hold the connection open.
            self.send(p, packet(0, video=False) + packet(8191, video=False))
            time.sleep(0.05)
        wait_for(lambda: not Path(f"/proc/{first}").exists())
        self.assertIsNone(p.poll())
        self.send(p, b"".join(packet(cc=i) for i in range(4)))
        wait_for(lambda: len(self.calls()) == 2)
        p.send_signal(signal.SIGINT)
        p.wait(timeout=3)
        self.assertEqual(p.returncode, 0)
        self.assertFalse(Path(f"/proc/{self.calls()[-1]}").exists())

    def test_failed_output_retries_without_stopping_receiver(self):
        p = self.worker(self.fake("fail"), loss=2000)
        self.send(p, b"".join(packet(cc=i) for i in range(4)))
        wait_for(lambda: len(self.calls()) >= 3)
        self.assertIsNone(p.poll())
        p.stdin.close()
        p.wait(timeout=3)
        self.assertEqual(p.returncode, 0)

    def test_backpressure_and_stubborn_encoder_shutdown(self):
        p = self.worker(self.fake("stall"), loss=2000)
        data = b"".join(packet(cc=i) for i in range(32))
        self.send(p, data)
        wait_for(lambda: len(self.calls()) == 1)
        os.set_blocking(p.stdin.fileno(), False)
        total = 0
        deadline = time.monotonic() + 4
        while total < 4 * 1024 * 1024 and time.monotonic() < deadline:
            if select.select([], [p.stdin], [], 0.1)[1]:
                total += os.write(p.stdin.fileno(), data)
        self.assertGreaterEqual(total, 4 * 1024 * 1024, "Encoder backpressure blocked TS ingestion")
        p.terminate()
        p.wait(timeout=3)
        self.assertEqual(p.returncode, 0)
        self.assertFalse(Path(f"/proc/{self.calls()[0]}").exists())

    def test_configuration_and_url_validation(self):
        base = [UI, "--config", ROOT / "linrad-dvb.example.ini", "--print-pipeline",
                "--source", "rtlsdr", "--standard", "DVB-T", "--sr", "250000"]
        good = subprocess.run([*base, "--srt-output", "srt://127.0.0.1:4001?mode=caller&latency=200000"],
                              capture_output=True, text=True, timeout=3)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertIn("--decoder-worker=dvbt", good.stdout)
        self.assertIn("--srt-worker", good.stdout)
        for url in ("udp://127.0.0.1:4001", "srt://127.0.0.1", "srt://:4001",
                    "srt://127.0.0.1:4001?mode=listener", "srt://127.0.0.1:4001?timeout=-1",
                    "srt://127.0.0.1:4001?mode=listener&mode=caller", "srt://host:65536",
                    "srt://host:0", "srt://[abcd]:4001", "srt://host:4001?mode=%ZZ"):
            with self.subTest(url=url):
                result = subprocess.run([*base, "--srt-output", url], capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)
        for url in ("srt://[::1]:4001", "srt://repeater.example:4001?streamid=test%20station"):
            result = subprocess.run([*base, "--srt-output", url], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_odroid_encoder_settings_and_invalid_options(self):
        p = self.worker(self.fake(), options=["--preset", "ultrafast", "--video-threads", "3", "--filter-threads", "1"])
        self.send(p, b"".join(packet(cc=i) for i in range(4)))
        wait_for(lambda: (self.directory / "args").exists())
        args = json.loads((self.directory / "args").read_text())
        for flag, value in (("-preset", "ultrafast"), ("-threads", "3"), ("-threads:v", "3"), ("-filter_threads", "1")):
            self.assertEqual(args[args.index(flag) + 1], value)
        for flag, value in (("--preset", "bad"), ("--video-threads", "-1"), ("--filter-threads", "65")):
            result = subprocess.run([UI, "--srt-worker", "--url", "srt://127.0.0.1:4001", flag, value],
                                    input=b"", capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)

    def test_headless_pipeline_shutdown_without_display(self):
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT / "linrad-dvb.example.ini")
        cfg["paths"]["sdr"] = str(ROOT / "build/sdr_to_stdout")
        cfg["paths"]["linrad"] = str(ROOT / "build/linrad_udp_to_stdout")
        cfg["paths"]["ffmpeg"] = str(self.fake())
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            cfg["linrad"]["port"] = str(sock.getsockname()[1])
        path = self.directory / "receiver.ini"
        with path.open("w") as out:
            cfg.write(out)
        log = (self.directory / "pipeline.log").open("w+")
        self.addCleanup(log.close)
        p = subprocess.Popen([UI, "--config", path, "--headless", "--source", "linrad", "--standard", "DVB-T",
                              "--sr", "250000", "--srt-output", "srt://127.0.0.1:4001"],
                             stdout=subprocess.DEVNULL, stderr=log,
                             env={**os.environ, "DISPLAY": "", "WAYLAND_DISPLAY": ""})
        self.addCleanup(stop, p)
        wait_for(lambda: "[srt] Waiting" in (self.directory / "pipeline.log").read_text())
        self.assertIsNone(p.poll())
        self.assertEqual(self.calls(), [])
        p.terminate()
        p.wait(timeout=4)
        self.assertEqual(p.returncode, 0, (self.directory / "pipeline.log").read_text())

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "FFmpeg/ffprobe unavailable")
    def test_real_srt_full_hd_audio_and_reconnect(self):
        protocols = subprocess.run([SRT_FFMPEG, "-hide_banner", "-protocols"], capture_output=True, text=True, check=True)
        if "srt" not in protocols.stdout.split():
            self.skipTest("FFmpeg has no SRT support")
        fixture = self.directory / "input.ts"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
                        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000", "-t", "4",
                        "-c:v", "mpeg2video", "-b:v", "500k", "-g", "12", "-c:a", "mp2",
                        "-f", "mpegts", fixture], check=True, timeout=15)
        anamorphic = self.directory / "anamorphic.ts"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=720x576:rate=25",
                        "-t", "4", "-vf", "setsar=16/15", "-c:v", "mpeg2video", "-b:v", "1000k",
                        "-g", "12", "-an", "-f", "mpegts", anamorphic], check=True, timeout=15)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        p = self.worker(url=f"srt://127.0.0.1:{port}", loss=1000,
                        options=["--preset", "ultrafast", "--video-threads", "3", "--filter-threads", "1"])
        for cycle, source in enumerate((fixture, anamorphic)):
            data = source.read_bytes()
            output = self.directory / f"output-{cycle}.ts"
            log = (self.directory / f"listener-{cycle}.log").open("w+")
            self.addCleanup(log.close)
            listener = subprocess.Popen([SRT_FFMPEG, "-v", "warning", "-nostdin", "-i",
                f"srt://127.0.0.1:{port}?mode=listener&listen_timeout=10000000&timeout=3000000",
                "-map", "0", "-c", "copy", "-f", "mpegts", output], stdout=subprocess.DEVNULL, stderr=log)
            self.addCleanup(stop, listener)
            time.sleep(0.2)
            # Supply real TS in roughly real time, then leave stdin open and silent.
            for at in range(0, len(data), 1316):
                self.send(p, data[at:at+1316])
                time.sleep(4 * 1316 / len(data))
            listener.wait(timeout=10)
            self.assertIsNone(p.poll(), (self.directory / "worker.log").read_text())
            self.assertTrue(output.exists(), log.name)
            probe = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", output],
                                   capture_output=True, text=True, check=True, timeout=5)
            streams = json.loads(probe.stdout)["streams"]
            video = next(s for s in streams if s["codec_type"] == "video")
            audio = next((s for s in streams if s["codec_type"] == "audio"), None)
            self.assertEqual((video["width"], video["height"], video["codec_name"]), (1920, 1080, "h264"))
            self.assertEqual(video["sample_aspect_ratio"], "1:1")
            self.assertEqual(video["r_frame_rate"], "25/1")
            if cycle == 0:
                self.assertEqual((audio["codec_name"], audio["sample_rate"], audio["channels"]), ("aac", "48000", 2))
            else:
                self.assertIsNone(audio)
            # Confirm 4:3 pillarboxing on a decoded RGB frame, not just metadata.
            frame = subprocess.run(["ffmpeg", "-v", "error", "-i", output, "-frames:v", "1",
                                    "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"],
                                   capture_output=True, check=True, timeout=10).stdout
            self.assertEqual(len(frame), 1920 * 1080 * 3)
            row = frame[540 * 1920 * 3:541 * 1920 * 3]
            self.assertLess(max(row[:200*3]), 12)
            self.assertGreater(max(row[250*3:270*3]), 100)
            self.assertGreater(max(row[300*3:1600*3]), 100)
            self.assertLess(max(row[1720*3:]), 12)
        p.terminate()
        p.wait(timeout=3)
        self.assertEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
