#!/usr/bin/env python3
"""Summarize the OpenBLT request/response frames captured by candump -L."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


FRAME_RE = re.compile(
    r"^\((?P<timestamp>\d+\.\d+)\)\s+"
    r"(?P<interface>\S+)\s+"
    r"(?P<can_id>[0-9A-Fa-f]+)#(?P<data>[0-9A-Fa-f]*)$"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    requests = []
    responses = []
    malformed = 0

    for raw_line in args.capture.read_text(encoding="utf-8").splitlines():
        match = FRAME_RE.match(raw_line.strip())
        if not match:
            if raw_line.strip():
                malformed += 1
            continue

        frame = match.groupdict()
        can_id = int(frame["can_id"], 16)
        if can_id == 0x667:
            requests.append(frame)
        elif can_id == 0x7E1:
            responses.append(frame)

    all_frames = sorted(requests + responses, key=lambda item: float(item["timestamp"]))
    duration = 0.0
    if len(all_frames) >= 2:
        duration = float(all_frames[-1]["timestamp"]) - float(all_frames[0]["timestamp"])

    status = "CAPTURED" if requests and responses else "INCOMPLETE"
    lines = [
        f"status: {status}",
        f"request_frames_0x667: {len(requests)}",
        f"response_frames_0x7E1: {len(responses)}",
        f"malformed_lines: {malformed}",
        f"exchange_duration_seconds: {duration:.3f}",
    ]
    if all_frames:
        lines.extend(
            [
                f"first_frame_epoch: {all_frames[0]['timestamp']}",
                f"last_frame_epoch: {all_frames[-1]['timestamp']}",
            ]
        )

    rendered = "\n".join(lines) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if status == "CAPTURED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
