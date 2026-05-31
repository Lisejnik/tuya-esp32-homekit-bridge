# Release Checklist

Use this checklist to prepare the next GitHub release. The project should still be described honestly: a working reference implementation for one verified plug, with a first web setup wizard, not a universal Tuya bridge.

## Suggested Version

`v2.0`

Reason: the HomeSpan sketch changes from source-code configuration to a user-facing ESP32 setup wizard with LAN admin, HomeKit pairing controls, factory reset, and LED status feedback.

## Pre-release Checks

Run from the repository root:

```bash
git status --short
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python scripts/scan_devices.py --help
python scripts/test_plug.py --help
python scripts/export_esp32_secrets.py
```

The last command requires a local `.env` and now generates `secrets.h` only for the ESP32 Tuya local POC.

Also verify:

- `git status --ignored --short` does not show tracked secret files
- Arduino IDE can compile `esp32/tuya_local_poc/tuya_local_poc.ino`
- Arduino IDE can compile `esp32/homespan_tuya_outlet/homespan_tuya_outlet.ino`
- HomeSpan sketch uses `Partition Scheme: No OTA (2MB APP/2MB SPIFFS)` or larger
- first boot starts `TuyaHomeKit-Setup`
- random setup AP password is printed in Serial Monitor
- setup form saves config and restarts
- **Test Tuya connection** reports success with known-good values
- normal boot prints an admin URL on port `8080`
- Wi-Fi failure falls back to setup mode and retries saved Wi-Fi
- **Clear saved config** clears bridge config
- **Clear HomeKit pairing** clears HomeKit pairing on the ESP32
- GPIO0 / BOOT long-hold factory reset is documented with the bootloader caveat
- status LED behavior is documented
- README and beginner guide match the current setup flow

## Suggested Git Commands

After committing release changes:

```bash
git tag -a v2.0 -m "Release v2.0"
git push origin main
git push origin v2.0
```

Then create the GitHub release from tag `v2.0`.

## Suggested GitHub Release Title

```text
v2.0 - ESP32 web setup wizard
```

## Suggested GitHub Release Notes

```markdown
Second public release of Tuya ESP32 HomeKit Bridge.

This release adds a first ESP32 web setup wizard and LAN admin page so users no longer need to edit the HomeSpan sketch source code or create `secrets.h` for normal HomeKit setup.

New:

- first-boot setup Wi-Fi access point: `TuyaHomeKit-Setup`
- random setup AP password printed in Serial Monitor
- plain local setup page at `http://192.168.4.1/`
- LAN admin page on port `8080` after Wi-Fi connection
- ESP32 Preferences storage for Wi-Fi, Tuya, HomeKit name, HomeKit type, relay DPS, and polling interval
- HomeKit pairing code field
- setup-page **Test Tuya connection** action
- setup-page **Clear saved config** action
- admin-page **Clear HomeKit pairing** action
- Wi-Fi failure fallback to setup mode with periodic retry
- BOOT / GPIO0 long-hold factory reset
- configurable HomeKit type, relay DPS, and polling interval
- status LED feedback for setup, Wi-Fi success, paired idle state, and Wi-Fi/Tuya errors

Still verified with:

- Tesla Smart Plug / Tuya Smart Life Wi-Fi plug
- Tuya protocol `3.4`
- relay datapoint `1`
- ESP32 development board
- HomeSpan HomeKit Outlet sketch

Important notes:

- HomeSpan sketch requires an ESP32 partition with at least a 2 MB app slot, for example `No OTA (2MB APP/2MB SPIFFS)`.
- The setup page uses plain HTTP on the temporary ESP32 setup network.
- The LAN admin page uses plain local HTTP without login and should only be used on a trusted LAN or IoT network.
- Wi-Fi passwords and Tuya local keys are stored in ESP32 Preferences in plaintext.
- The ESP32 Tuya local POC still uses generated `secrets.h`; the HomeSpan sketch does not.
- This is still experimental and not intended for critical loads.
```

## Good Follow-up Issues

- Add screenshots of the setup wizard
- Add a compatibility table for tested plugs
- Add optional captive portal DNS redirect
- Extract Tuya protocol logic into a reusable C++ class
- Investigate Tuya protocol versions beyond `3.4`
- Add optional admin password or disable admin after setup
