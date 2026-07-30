#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "summarize_capture.py"


class SummarizeCaptureTests(unittest.TestCase):
    def run_summary(self, content: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            capture = Path(temporary_directory) / "capture.log"
            capture.write_text(content, encoding="utf-8")
            return subprocess.run(
                ["python3", str(SCRIPT), str(capture)],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_bidirectional_exchange_is_captured(self) -> None:
        result = self.run_summary(
            "(1784533000.100000) slcan0 667#FF00\n"
            "(1784533000.125000) slcan0 7E1#FF00\n"
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("status: CAPTURED", result.stdout)
        self.assertIn("request_frames_0x667: 1", result.stdout)
        self.assertIn("response_frames_0x7E1: 1", result.stdout)

    def test_one_way_exchange_is_incomplete(self) -> None:
        result = self.run_summary("(1784533000.100000) slcan0 667#FF00\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("status: INCOMPLETE", result.stdout)

    def test_unrelated_frames_do_not_count(self) -> None:
        result = self.run_summary("(1784533000.100000) slcan0 200#01020304\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("request_frames_0x667: 0", result.stdout)
        self.assertIn("response_frames_0x7E1: 0", result.stdout)


if __name__ == "__main__":
    unittest.main()
