# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Rob Hardenberg / PE1ITR
"""Original TS preservation and loss gating, without SDR or FFmpeg."""
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest

WORKER = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]


def packet(pid, cc=0, video=False, tei=False, text=b""):
    payload = b"\x00\x00\x01\xe0\x00\x00\x80\x00\x00" if video else text
    return bytes((0x47, (pid >> 8) | 0x40 | (0x80 if tei else 0), pid & 255, 0x10 | (cc & 15))) + payload.ljust(184, b"\xff")


def wait_for(predicate, timeout=3):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out")


def stop(p):
    if p.poll() is None:
        p.terminate()
        p.wait(timeout=3)
    if p.stdin:
        p.stdin.close()


class UdpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)

    def receiver(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        self.addCleanup(sock.close)
        return sock

    def collect(self, sock, size):
        data = bytearray()
        until = time.monotonic() + 3
        while len(data) < size and time.monotonic() < until:
            if select.select([sock], [], [], 0.05)[0]:
                chunk = sock.recv(65536)
                self.assertEqual(len(chunk) % 188, 0)
                self.assertLessEqual(len(chunk), 1316)
                data.extend(chunk)
        return data

    def test_preservation_loss_and_recovery(self):
        for transport in ("udp", "stdin"):
            with self.subTest(transport=transport):
                output = self.receiver()
                address = f"127.0.0.1:{output.getsockname()[1]}"
                args = [WORKER, "--destination", address, "--loss-ms", "150"]
                source = self.receiver()
                port = source.getsockname()[1]
                source.close()
                if transport == "udp":
                    args += ["--udp-input", f"127.0.0.1:{port}"]
                logpath = self.directory / f"{transport}.log"
                log = logpath.open("w+")
                self.addCleanup(log.close)
                p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
                self.addCleanup(stop, p)
                wait_for(lambda: "Waiting for video" in logpath.read_text())
                sender = self.receiver()

                def send(data):
                    if transport == "udp":
                        sender.sendto(data, ("127.0.0.1", port))
                    else:
                        p.stdin.write(data)
                        p.stdin.flush()

                # All of these must survive byte-for-byte, not just video/audio.
                prefix = b"".join(packet(pid, text=text) for pid, text in (
                    (0, b"PAT"), (0x1000, b"PMT"), (0x11, b"Service name / Provider name"),
                    (0x12, b"EIT"), (0x14, b"TDT/TOT"), (0x101, b"audio"),
                    (0x102, b"teletext"), (0x103, b"subtitles"), (0x777, b"private"), (8191, b"null")))
                send(b"noise" + prefix)
                self.assertEqual(select.select([output], [], [], 0.04)[0], [])
                video = b"".join(packet(256, i, video=True) for i in range(7))
                tail = packet(0x101, 1, text=b"audio") + packet(0x102, 1, text=b"teletext")
                send(video + tail)
                expected = prefix + video + tail
                self.assertEqual(self.collect(output, len(expected)), expected)
                wait_for(lambda: "Video lost" in logpath.read_text())
                # Continuing audio/PSI, duplicates and TEI cannot keep output open.
                ignored = (packet(0x101, 2) + packet(0) + packet(8191) + packet(256, 6, video=True) +
                           packet(256, 6, video=True, tei=True))
                send(ignored)
                self.assertEqual(select.select([output], [], [], 0.2)[0], [])
                resume = b"".join(packet(256, i, video=True) for i in range(7, 10))
                send(resume)
                self.assertEqual(self.collect(output, len(ignored + resume)), ignored + resume)
                self.assertIsNone(p.poll())
                stop(p)
                self.assertEqual(p.returncode, 0)
                self.assertEqual(p.stdout.read(), b"")
                p.stdout.close()

    def test_invalid_arguments(self):
        for args in ([], ["--destination", "localhost:5001"], ["--destination", "127.0.0.1:0"],
                     ["--destination", "127.0.0.1:65536"], ["--destination", "239.1.1.1:5001"],
                     ["--destination", "127.0.0.1:5001?pkt_size=1"],
                     ["--destination", "127.0.0.1:5001", "--loss-ms", "0"],
                     ["--destination", "127.0.0.1:5001", "--udp-input", "127.0.0.1:5001"]):
            result = subprocess.run([WORKER, *args], input=b"", capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0, args)
            self.assertEqual(result.stdout, b"")

    def test_launcher_udp_needs_no_ffmpeg_or_srt(self):
        for transport in ("udp", "stdin"):
            root = self.directory / transport
            root.mkdir()
            rtl = root / "rtl"
            rtl.write_text(f"#!{sys.executable}\nimport time\nwhile True: time.sleep(0.05)\n")
            rtl.chmod(0o755)
            decoder = root / "decoder"
            data = b"".join(packet(256, i, video=True) for i in range(7))
            decoder.write_text(f"#!{sys.executable}\n"
                "import os,socket,sys,time\n"
                "s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n"
                f"data={data!r}\n"
                "time.sleep(0.15)\n"
                "if '--udp-ts' in sys.argv:\n"
                "    host,port=sys.argv[sys.argv.index('--udp-ts')+1].split(':')\n"
                "    s.sendto(data,(host,int(port)))\n"
                "else: os.write(1,data)\n"
                "while True: time.sleep(0.05)\n")
            decoder.chmod(0o755)
            output = self.receiver()
            internal = self.receiver()
            port = internal.getsockname()[1]
            internal.close()
            env = {**os.environ, "OUTPUT_MODE": "udp", "MPEGTS_DEST": f"127.0.0.1:{output.getsockname()[1]}",
                   "RBDVBT_UDP": str(WORKER), "RBDVBT_SRT": "/nonexistent/srt", "FFMPEG": "/nonexistent/ffmpeg",
                   "RBDVBT_RX": str(decoder), "RTL_SDR": str(rtl), "LOG_DIR": str(root),
                   "UDP_TS": f"127.0.0.1:{port}" if transport == "udp" else "off",
                   "WATCHDOG_ENABLED": "0", "WATCHDOG_INTERVAL": "0.05", "SRT_LOSS_TIMEOUT_MS": "150"}
            with (root / "launcher.log").open("w+") as log:
                p = subprocess.Popen([ROOT / "tools-pi6ehv/dvbt_rx_pi6ehv.sh"], env=env,
                                     stdout=subprocess.DEVNULL, stderr=log)
                self.addCleanup(stop, p)
                self.assertEqual(self.collect(output, len(data)), data, (root / "launcher.log").read_text())
                self.assertEqual(select.select([output], [], [], 0.4)[0], [])
                self.assertIsNone(p.poll())
                stop(p)


if __name__ == "__main__":
    unittest.main()
