#!/usr/bin/env python3
"""Generate an ignored ESP32 secrets.h file from local .env values."""

from __future__ import annotations

import os
import re
from pathlib import Path

from dotenv import load_dotenv


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ENV_PATH = PROJECT_ROOT / ".env"
OUT_PATHS = (
    PROJECT_ROOT / "esp32" / "tuya_local_poc" / "secrets.h",
    PROJECT_ROOT / "esp32" / "homespan_tuya_outlet" / "secrets.h",
)


def require(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise SystemExit(f"Missing {name} in {ENV_PATH}")
    return value


def ip_initializer(ip_address: str) -> str:
    parts = ip_address.split(".")
    if len(parts) != 4:
        raise SystemExit(f"DEVICE_IP is not an IPv4 address: {ip_address}")
    octets = []
    for part in parts:
        try:
            number = int(part)
        except ValueError as exc:
            raise SystemExit(f"Invalid DEVICE_IP octet: {part}") from exc
        if number < 0 or number > 255:
            raise SystemExit(f"Invalid DEVICE_IP octet: {part}")
        octets.append(str(number))
    return ", ".join(octets)


def c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def read_define(path: Path, name: str, default: str) -> str:
    if not path.exists():
        return default
    match = re.search(
        rf'^\s*#define\s+{re.escape(name)}\s+"([^"]*)"\s*$',
        path.read_text(errors="ignore"),
        flags=re.MULTILINE,
    )
    if not match:
        return default
    value = match.group(1)
    return value if value else default


def render_content(path: Path, device_id: str, device_ip: str, local_key: str) -> str:
    wifi_ssid = read_define(path, "WIFI_SSID", "CHANGE_ME")
    wifi_password = read_define(path, "WIFI_PASSWORD", "CHANGE_ME")
    accessory_name = read_define(path, "HOMEKIT_ACCESSORY_NAME", "Tuya HomeKit Outlet")
    manufacturer = read_define(path, "HOMEKIT_MANUFACTURER", "Tuya Local Bridge")
    model = read_define(path, "HOMEKIT_MODEL", "Tuya Plug via ESP32")
    return (
        "#pragma once\n\n"
        "// This file is generated from .env and is ignored by git.\n"
        "// Existing Wi-Fi credentials and accessory metadata are preserved.\n"
        f'#define WIFI_SSID "{c_string(wifi_ssid)}"\n'
        f'#define WIFI_PASSWORD "{c_string(wifi_password)}"\n\n'
        f'#define HOMEKIT_ACCESSORY_NAME "{c_string(accessory_name)}"\n'
        f'#define HOMEKIT_MANUFACTURER "{c_string(manufacturer)}"\n'
        f'#define HOMEKIT_MODEL "{c_string(model)}"\n\n'
        f'#define TUYA_DEVICE_ID "{c_string(device_id)}"\n'
        f"#define TUYA_DEVICE_IP IPAddress({ip_initializer(device_ip)})\n"
        f'#define TUYA_LOCAL_KEY "{c_string(local_key)}"\n'
        '#define TUYA_RELAY_DPS "1"\n'
    )


def main() -> int:
    if not ENV_PATH.exists():
        raise SystemExit(f"Missing {ENV_PATH}")

    load_dotenv(ENV_PATH)
    device_id = require("DEVICE_ID")
    device_ip = require("DEVICE_IP")
    local_key = require("LOCAL_KEY")
    device_version = require("DEVICE_VERSION")
    if device_version != "3.4":
        raise SystemExit("This ESP32 POC only targets Tuya protocol 3.4.")

    for path in OUT_PATHS:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(render_content(path, device_id, device_ip, local_key))
        path.chmod(0o600)
        print(f"Wrote {path} with restricted permissions; secrets not displayed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
