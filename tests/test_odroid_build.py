# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Rob Hardenberg / PE1ITR
"""Check native build planning without installing packages or touching hardware."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools-pi6ehv/build_odroid_arm64.sh"


class OdroidBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name, body in {
            "uname": "print(os.environ['TEST_ARCH'])\n",
            "cmake": "with open(os.environ['TEST_COMMANDS'], 'a') as f: f.write(json.dumps(sys.argv) + '\\n')\n",
            "ctest": "with open(os.environ['TEST_COMMANDS'], 'a') as f: f.write(json.dumps(sys.argv) + '\\n')\n"
                     "with open(os.environ['TEST_COMMANDS'] + '.cwd', 'w') as f: f.write(os.getcwd())\n",
        }.items():
            tool = self.root / name
            tool.write_text(f"#!{sys.executable}\nimport json, os, sys\n" + body)
            tool.chmod(0o755)
        self.env = {**os.environ, "PATH": f"{self.root}:{os.environ['PATH']}",
                    "TEST_ARCH": "aarch64", "TEST_COMMANDS": str(self.root / "commands"),
                    "BUILD_DIR": str(self.root / "build with spaces"), "BUILD_TYPE": "Release",
                    "TARGET_CPU": "auto", "JOBS": "2", "WITH_X11": "0", "RUN_TESTS": "0",
                    "INSTALL_DEPS": "0"}
        self.env.pop("RBDVBT_DECODER_ROOT", None)
        Path(self.env["BUILD_DIR"]).mkdir()

    def run_build(self, **settings):
        self.env.update(settings)
        result = subprocess.run([SCRIPT], env=self.env, text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [json.loads(line) for line in (self.root / "commands").read_text().splitlines()]

    def test_n2_release_without_gui_or_x11(self):
        commands = self.run_build()
        self.assertIn("-DRBDVBT_TARGET_CPU=odroid-n2", commands[0])
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", commands[0])
        self.assertIn("-DRBDVBT_BUILD_GUI=OFF", commands[0])
        self.assertIn("-DRBDVBT_WITH_X11=OFF", commands[0])
        self.assertIn("-DRBDVBT_BUILD_SRT=ON", commands[0])
        self.assertEqual(commands[1][-2:], ["-j", "2"])
        self.assertEqual(len(commands), 2)

    def test_desktop_does_not_get_arm_flags(self):
        commands = self.run_build(TEST_ARCH="x86_64")
        self.assertIn("-DRBDVBT_TARGET_CPU=generic", commands[0])

    def test_explicit_decoder_path(self):
        decoder_root = str(self.root / "pipeline with spaces/components/dvbt")
        commands = self.run_build(RBDVBT_DECODER_ROOT=decoder_root)
        self.assertIn(f"-DRBDVBT_DECODER_ROOT={decoder_root}", commands[0])

    def test_explicit_generic_x11_and_native_tests(self):
        commands = self.run_build(TARGET_CPU="generic", WITH_X11="1", RUN_TESTS="1")
        self.assertIn("-DRBDVBT_TARGET_CPU=generic", commands[0])
        self.assertIn("-DRBDVBT_WITH_X11=ON", commands[0])
        self.assertEqual(Path(commands[2][0]).name, "ctest")
        self.assertIn("--output-on-failure", commands[2])
        self.assertNotIn("--test-dir", commands[2])
        self.assertEqual((self.root / "commands.cwd").read_text(), self.env["BUILD_DIR"])

    def test_invalid_settings_do_not_build(self):
        for setting in ({"TARGET_CPU": "unknown"}, {"JOBS": "0"}, {"JOBS": "-2"}, {"WITH_X11": "yes"}):
            with self.subTest(setting=setting):
                result = subprocess.run([SCRIPT], env={**self.env, **setting}, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2)
                self.assertFalse((self.root / "commands").exists())


if __name__ == "__main__":
    unittest.main()
