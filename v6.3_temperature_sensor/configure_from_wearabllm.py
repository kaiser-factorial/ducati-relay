#!/usr/bin/env python3
"""Generate ignored v6.3 Wi-Fi config from a local WearabLLM sdkconfig."""

from __future__ import annotations

import argparse
import json
import os
import re
import urllib.request
from pathlib import Path


KEYS = {
    "TEMP_SENSOR_WIFI_SSID": "CONFIG_WEARABLLM_WIFI_SSID",
    "TEMP_SENSOR_WIFI_PASSWORD": "CONFIG_WEARABLLM_WIFI_PASSWORD",
    "TEMP_SENSOR_BRIDGE_BASE_URL": "CONFIG_WEARABLLM_BRIDGE_URL",
    "TEMP_SENSOR_BRIDGE_TOKEN": "CONFIG_WEARABLLM_BRIDGE_AUTH_TOKEN",
}
DEFAULT_ROOT_CA_URL = "https://www.amazontrust.com/repository/AmazonRootCA1.pem"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdkconfig", type=Path, help="Existing WearabLLM firmware sdkconfig")
    parser.add_argument(
        "--root-ca-url",
        default=DEFAULT_ROOT_CA_URL,
        help="HTTPS URL for the bridge trust anchor (defaults to Amazon Root CA 1)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).with_name("wifi_config.h"),
        help="Ignored output header",
    )
    return parser.parse_args()


def load_sdkconfig(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    for output_key, sdk_key in KEYS.items():
        match = re.search(rf"^{re.escape(sdk_key)}=(.+)$", text, re.MULTILINE)
        if not match:
            raise RuntimeError(f"Missing {sdk_key} in {path}")
        raw = match.group(1).strip()
        values[output_key] = str(json.loads(raw)) if raw.startswith('"') else raw
    values["TEMP_SENSOR_BRIDGE_BASE_URL"] = re.sub(
        r"/v1/query/?$", "", values["TEMP_SENSOR_BRIDGE_BASE_URL"]
    ).rstrip("/")
    for key, value in values.items():
        if not value:
            raise RuntimeError(f"{key} is empty")
    return values


def main() -> int:
    args = parse_args()
    values = load_sdkconfig(args.sdkconfig.expanduser().resolve())
    with urllib.request.urlopen(args.root_ca_url, timeout=20) as response:
        root_ca = response.read().decode("ascii").strip()
    if "-----BEGIN CERTIFICATE-----" not in root_ca or "-----END CERTIFICATE-----" not in root_ca:
        raise RuntimeError("Downloaded trust anchor is not a PEM certificate")

    lines = ["#pragma once", "", "// Generated locally; this file is ignored by Git."]
    for key in KEYS:
        lines.append(f"#define {key} {json.dumps(values[key])}")
    lines.extend(
        [
            '#define TEMP_SENSOR_ROOT_CA R"PEM(',
            root_ca,
            ')PEM"',
            "",
        ]
    )
    output = args.output.expanduser().resolve()
    output.write_text("\n".join(lines), encoding="utf-8")
    os.chmod(output, 0o600)
    print(f"Wrote {output} with mode 0600; secrets were not printed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
