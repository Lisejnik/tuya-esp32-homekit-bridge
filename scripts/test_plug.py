#!/usr/bin/env python3
"""Read and locally control a Tuya smart plug using TinyTuya."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ENV_PATH = PROJECT_ROOT / ".env"
REQUIRED_ENV = ("DEVICE_ID", "DEVICE_IP", "LOCAL_KEY", "DEVICE_VERSION")


class ConfigError(RuntimeError):
    """Invalid local configuration."""


def mask_secret(value: str) -> str:
    if not value:
        return "<empty>"
    return f"<redacted> ({len(value)} chars)"


def load_config() -> dict[str, str]:
    from dotenv import load_dotenv

    if not ENV_PATH.exists():
        raise ConfigError(
            f"Missing {ENV_PATH}. Create it from .env.example and fill values locally."
        )

    load_dotenv(ENV_PATH)
    config = {name: os.getenv(name, "").strip() for name in REQUIRED_ENV}
    missing = [name for name, value in config.items() if not value]
    if missing:
        raise ConfigError(
            "Missing required variable(s) in .env: " + ", ".join(missing)
        )

    try:
        float(config["DEVICE_VERSION"])
    except ValueError as exc:
        raise ConfigError(
            "DEVICE_VERSION must be a Tuya protocol number such as 3.1, 3.3, 3.4, or 3.5."
        ) from exc

    return config


def make_device(config: dict[str, str]) -> Any:
    import tinytuya

    return tinytuya.OutletDevice(
        dev_id=config["DEVICE_ID"],
        address=config["DEVICE_IP"],
        local_key=config["LOCAL_KEY"],
        version=float(config["DEVICE_VERSION"]),
        connection_timeout=5,
        connection_retry_limit=1,
        connection_retry_delay=1,
    )


def print_status(data: Any) -> None:
    print("Device status response:")
    print(json.dumps(data, indent=2, ensure_ascii=False, sort_keys=True, default=str))
    if isinstance(data, dict) and "Error" in data:
        print("\nTinyTuya returned an error. Check DEVICE_VERSION and LOCAL_KEY.")


def run_command(device: tinytuya.OutletDevice, command: str) -> Any:
    if command == "status":
        return device.status()
    if command == "on":
        print("Sending local command: turn plug ON")
        return device.turn_on()
    if command == "off":
        print("Sending local command: turn plug OFF")
        return device.turn_off()
    raise ValueError(f"Unsupported command: {command}")


def explain_failure(exc: Exception, config: dict[str, str]) -> None:
    message = str(exc)
    print("TinyTuya operation failed.", file=sys.stderr)
    print(f"Device IP: {config['DEVICE_IP']}", file=sys.stderr)
    print(f"Device version: {config['DEVICE_VERSION']}", file=sys.stderr)
    print(f"Local key: {mask_secret(config['LOCAL_KEY'])}", file=sys.stderr)

    if isinstance(exc, (TimeoutError, socket.timeout)):
        print(
            "The device did not respond before timeout. Check IP address, Wi-Fi, "
            "and whether the Smart Life/Tuya app has an active local connection.",
            file=sys.stderr,
        )
    elif isinstance(exc, OSError):
        print(
            "Network connection failed. Confirm the Mac and plug are on the same LAN.",
            file=sys.stderr,
        )
    elif "decrypt" in message.lower() or "payload" in message.lower():
        print(
            "This often means LOCAL_KEY or DEVICE_VERSION is wrong.",
            file=sys.stderr,
        )
    elif "version" in message.lower() or "protocol" in message.lower():
        print(
            "The configured Tuya protocol version may not be supported by this device.",
            file=sys.stderr,
        )

    print(f"Original error: {type(exc).__name__}: {message}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description="Test local Tuya plug control.")
    parser.add_argument("command", choices=("status", "on", "off"))
    args = parser.parse_args()

    try:
        config = load_config()
    except ConfigError as exc:
        print(exc, file=sys.stderr)
        return 2

    print(
        "Using local config: "
        f"DEVICE_ID={config['DEVICE_ID']}, "
        f"DEVICE_IP={config['DEVICE_IP']}, "
        f"DEVICE_VERSION={config['DEVICE_VERSION']}, "
        f"LOCAL_KEY={mask_secret(config['LOCAL_KEY'])}"
    )

    try:
        device = make_device(config)
        data = run_command(device, args.command)
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
        return 130
    except Exception as exc:  # TinyTuya surfaces transport/decrypt errors broadly.
        explain_failure(exc, config)
        return 1

    print_status(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
