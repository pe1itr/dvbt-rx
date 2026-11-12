"""Exercise the shared linrad SRT supervisor and this receiver's UDP adapter."""
import importlib.util
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import unittest

WORKER = Path(sys.argv.pop(1)).resolve()
UPSTREAM = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("linrad_srt_tests", UPSTREAM)
shared = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(shared)


class ReceiverSrtTests(shared.SrtTests):
    def worker(self, ffmpeg=None, url="srt://127.0.0.1:4001", loss=300, options=()):
        log = (self.directory / "worker.log").open("w+")
        self.addCleanup(log.close)
        p = subprocess.Popen([WORKER, "--url", url, "--loss-ms", str(loss),
                              "--retry-ms", "200", "--ffmpeg", ffmpeg or shared.SRT_FFMPEG, *options],
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=log, bufsize=0,
                             env={**os.environ, "DISPLAY": "", "WAYLAND_DISPLAY": ""})
        self.addCleanup(shared.stop, p)
        return p

    def test_udp_loss_reacquisition_and_port_conflict(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
            source.bind(("127.0.0.1", 0))
            endpoint = f"127.0.0.1:{source.getsockname()[1]}"
        p = self.worker(self.fake(), options=["--udp-input", endpoint])
        shared.wait_for(lambda: "Waiting for video" in (self.directory / "worker.log").read_text())
        duplicate = subprocess.run([WORKER, "--udp-input", endpoint, "--url", "srt://127.0.0.1:4001"],
                                   capture_output=True, timeout=3)
        self.assertNotEqual(duplicate.returncode, 0)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
            source.sendto(shared.packet(8191, video=False) * 7, ("127.0.0.1", int(endpoint.split(":")[1])))
            self.assertEqual(self.calls(), [])
            video = b"".join(shared.packet(cc=i) for i in range(7))
            source.sendto(video, ("127.0.0.1", int(endpoint.split(":")[1])))
            shared.wait_for(lambda: len(self.calls()) == 1)
            first = self.calls()[0]
            shared.wait_for(lambda: not Path(f"/proc/{first}").exists())
            self.assertIsNone(p.poll())
            source.sendto(video, ("127.0.0.1", int(endpoint.split(":")[1])))
            shared.wait_for(lambda: len(self.calls()) == 2)
        p.terminate()
        p.wait(timeout=3)
        self.assertEqual(p.returncode, 0)
        self.assertFalse(Path(f"/proc/{self.calls()[-1]}").exists())

    def test_invalid_options(self):
        for options in (["--udp-input", "localhost:10000"], ["--udp-input", "127.0.0.1:0"],
                        ["--udp-input", "127.0.0.1:65536"], ["--udp-input"],
                        ["--preset", "bad"], ["--video-threads", "-1"], ["--filter-threads", "65"],
                        ["--url", "srt://127.0.0.1:4001?mode=listener"],
                        ["--url", "srt://127.0.0.1:4001?mode=listener&mode=caller"]):
            with self.subTest(options=options):
                result = subprocess.run([WORKER, "--url", "srt://127.0.0.1:4001", *options],
                                        input=b"", capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)

    def test_headless_launcher_keeps_receiver_during_loss(self):
        launcher = Path(__file__).resolve().parents[1] / "tools-pi6ehv/dvbt_rx_pi6ehv.sh"
        ffmpeg = self.fake()
        ffmpeg.write_text(ffmpeg.read_text().replace(
            "import json, os, pathlib, signal, sys, time\n",
            "import json, os, pathlib, signal, sys, time\n"
            "if '-protocols' in sys.argv:\n    print('  srt'); sys.exit(0)\n"))
        for transport in ("udp", "stdout"):
            with self.subTest(transport=transport):
                root = self.directory / transport
                root.mkdir()
                rtl = root / "rtl"
                rtl.write_text(f"#!{sys.executable}\nimport os, pathlib, time\n"
                               f"pathlib.Path({str(root / 'rtl-pid')!r}).write_text(str(os.getpid()))\n"
                               "while True: time.sleep(0.05)\n")
                rtl.chmod(0o755)
                decoder = root / "decoder"
                decoder.write_text(f"#!{sys.executable}\n"
                    "import json, os, pathlib, socket, sys, time\n"
                    f"root = pathlib.Path({str(root)!r})\n"
                    "(root / 'decoder-pid').write_text(str(os.getpid()))\n"
                    "status = pathlib.Path(sys.argv[sys.argv.index('--status-json') + 1])\n"
                    "sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
                    "endpoint = None\n"
                    "if '--udp-ts' in sys.argv:\n"
                    "    host, port = sys.argv[sys.argv.index('--udp-ts') + 1].split(':')\n"
                    "    endpoint = (host, int(port))\n"
                    "start = time.monotonic()\n"
                    "cc = 0\n"
                    "while True:\n"
                    "    active = time.monotonic() - start < 0.8 or (root / 'resume').exists()\n"
                    "    status.write_text(json.dumps(dict(locked=active, lamp_ofdm_sync=active, updated_unix=int(time.time()))))\n"
                    "    if active:\n"
                    "        data = b''.join(bytes((0x47, 0x41, 0, 0x10 | ((cc+i) & 15))) + "
                    "b'\\x00\\x00\\x01\\xe0\\x00\\x00\\x80\\x00\\x00' + bytes(175) for i in range(7))\n"
                    "        cc += 7\n"
                    "        if endpoint: sock.sendto(data, endpoint)\n"
                    "        else: os.write(1, data)\n"
                    "    time.sleep(0.04)\n")
                decoder.chmod(0o755)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.bind(("127.0.0.1", 0))
                    endpoint = f"127.0.0.1:{sock.getsockname()[1]}"
                env = {**os.environ, "RTL_SDR": str(rtl), "RBDVBT_RX": str(decoder),
                       "RBDVBT_SRT": str(WORKER), "FFMPEG": str(ffmpeg),
                       "LOG_DIR": str(root), "STATUS_JSON": str(root / "status.json"),
                       "FFMPEGLOG": str(root / "srt.log"), "UDP_TS": endpoint if transport == "udp" else "off",
                       "WATCHDOG_INTERVAL": "0.1", "SRT_LOSS_TIMEOUT_MS": "300",
                       "LOCK_LOSS_TIMEOUT": "1", "DISPLAY": "", "WAYLAND_DISPLAY": ""}
                log = (root / "launcher.log").open("w+")
                self.addCleanup(log.close)
                before = len(self.calls())
                p = subprocess.Popen([launcher], env=env, stdout=subprocess.DEVNULL, stderr=log)
                self.addCleanup(shared.stop, p)
                shared.wait_for(lambda: len(self.calls()) > before)
                first = self.calls()[-1]
                rtl_pid = (root / "rtl-pid").read_text()
                decoder_pid = (root / "decoder-pid").read_text()
                shared.wait_for(lambda: not Path(f"/proc/{first}").exists())
                time.sleep(2)
                self.assertIsNone(p.poll(), (root / "launcher.log").read_text())
                self.assertEqual((root / "rtl-pid").read_text(), rtl_pid)
                self.assertEqual((root / "decoder-pid").read_text(), decoder_pid)
                self.assertEqual(len(self.calls()), before + 1)
                (root / "resume").touch()
                shared.wait_for(lambda: len(self.calls()) == before + 2)
                p.terminate()
                p.wait(timeout=4)
                self.assertEqual(p.returncode, 143)
                for pid in (rtl_pid, decoder_pid, self.calls()[-1]):
                    self.assertFalse(Path(f"/proc/{pid}").exists(), f"Child {pid} left running")


if __name__ == "__main__":
    # Reuse upstream supervision/real transcoding tests without its GTK launcher tests.
    names = ["test_gate_loss_reacquisition_and_full_hd_arguments",
             "test_failed_output_retries_without_stopping_receiver",
             "test_backpressure_and_stubborn_encoder_shutdown",
             "test_real_srt_full_hd_audio_and_reconnect",
             "test_udp_loss_reacquisition_and_port_conflict", "test_invalid_options",
             "test_headless_launcher_keeps_receiver_during_loss"]
    suite = unittest.TestSuite(ReceiverSrtTests(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
