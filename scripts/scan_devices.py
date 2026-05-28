#!/usr/bin/env python3
"""Scan the local network for Tuya devices without printing local keys."""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any

from dotenv import load_dotenv
import tinytuya


DEFAULT_TARGET_IP = "192.168.1.123"
SENSITIVE_KEYS = {
    "key",
    "local_key",
    "localkey",
    "localKey",
    "secret",
    "apiKey",
    "apiSecret",
}
SENSITIVE_KEYS_LOWER = {key.lower() for key in SENSITIVE_KEYS}


def sanitize(value: Any) -> Any:
    """Remove known secret fields from nested TinyTuya scan data."""
    if isinstance(value, dict):
        sanitized: dict[str, Any] = {}
        for key, item in value.items():
            if key in SENSITIVE_KEYS or key.lower() in SENSITIVE_KEYS_LOWER:
                sanitized[key] = "<redacted>"
            else:
                sanitized[key] = sanitize(item)
        return sanitized
    if isinstance(value, list):
        return [sanitize(item) for item in value]
    return value


def normalize_devices(raw_devices: Any) -> list[dict[str, Any]]:
    """Normalize TinyTuya scan output into a list of dictionaries."""
    if isinstance(raw_devices, dict):
        devices = []
        for device_id, details in raw_devices.items():
            if isinstance(details, dict):
                item = dict(details)
                item.setdefault("id", device_id)
                devices.append(item)
            else:
                devices.append({"id": device_id, "value": details})
        return devices
    if isinstance(raw_devices, list):
        return [item for item in raw_devices if isinstance(item, dict)]
    return []


def pick(device: dict[str, Any], *keys: str) -> Any:
    for key in keys:
        if key in device and device[key] not in (None, ""):
            return device[key]
    return None


def print_device(device: dict[str, Any], target_ip: str) -> None:
    ip_address = pick(device, "ip", "ip_address", "address", "gwId")
    device_id = pick(device, "id", "dev_id", "devId", "device_id")
    version = pick(device, "version", "ver", "protocol", "protocol_version")
    product = pick(device, "product_name", "product", "product_id", "productKey")
    marker = " <-- TARGET IP" if ip_address == target_ip else ""

    print(f"- IP: {ip_address or 'nezjisteno'}{marker}")
    print(f"  Device ID: {device_id or 'nezjisteno'}")
    print(f"  Tuya protocol version: {version or 'nezjisteno'}")
    print(f"  Product: {product or 'nezjisteno'}")

    extra = sanitize(device)
    print("  Raw scan data without secrets:")
    print(json.dumps(extra, indent=4, ensure_ascii=False, sort_keys=True))


def main() -> int:
    load_dotenv()
    default_target_ip = os.getenv("DEVICE_IP", DEFAULT_TARGET_IP)
    parser = argparse.ArgumentParser(
        description="Scan LAN for Tuya devices and highlight the target plug IP."
    )
    parser.add_argument(
        "--target-ip",
        default=default_target_ip,
        help=f"IP address to highlight. Default: DEVICE_IP from .env or {DEFAULT_TARGET_IP}.",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=15,
        help="TinyTuya scan retry count. Default: 15.",
    )
    args = parser.parse_args()

    print("Scanning local network for Tuya devices...")
    print("Target plug IP:", args.target_ip)
    print("No local keys will be printed by this script.")

    try:
        try:
            raw_devices = tinytuya.deviceScan(False, args.retries)
        except TypeError:
            raw_devices = tinytuya.deviceScan(maxretry=args.retries)
    except KeyboardInterrupt:
        print("\nScan interrupted by user.")
        return 130
    except OSError as exc:
        print(f"Network scan failed: {exc}", file=sys.stderr)
        print(
            "Check that macOS firewall allows local network traffic and that "
            "the computer is on the same 2.4 GHz LAN as the plug.",
            file=sys.stderr,
        )
        return 2
    except Exception as exc:  # TinyTuya raises several transport-specific errors.
        print(f"TinyTuya scan failed: {exc}", file=sys.stderr)
        return 2

    devices = normalize_devices(raw_devices)
    if not devices:
        print("No Tuya devices were found on the LAN.")
        print(
            "Close the Smart Life/Tuya app, verify the plug is powered on at "
            f"{args.target_ip}, and ensure UDP 6666/6667/7000 plus TCP 6668 are not blocked."
        )
        return 1

    print(f"\nFound {len(devices)} Tuya device(s):")
    found_target = False
    for device in devices:
        if pick(device, "ip", "ip_address", "address", "gwId") == args.target_ip:
            found_target = True
        print_device(device, args.target_ip)

    if not found_target:
        print(f"\nNo scanned Tuya device matched target IP {args.target_ip}.")
        print("Confirm the plug IP in your router/DHCP lease table and run the scan again.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
