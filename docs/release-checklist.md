# Release Checklist

Use this checklist to prepare the GitHub release. The first public release should be honest about scope: working reference implementation for one verified plug, not a universal Tuya bridge.

## Suggested Version

`v0.1.0`

Reason: the prototype is verified end to end, but compatibility is intentionally narrow and experimental.

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

The last command requires a local `.env`. If you do not want to use real values during release prep, create a temporary local `.env` with dummy-looking but structurally valid values and delete it afterwards. Never commit `.env` or generated `secrets.h`.

Also verify:

- `git status --ignored --short` does not show tracked secret files
- Arduino IDE can compile `esp32/tuya_local_poc/tuya_local_poc.ino`
- Arduino IDE can compile `esp32/homespan_tuya_outlet/homespan_tuya_outlet.ino`
- README links work
- beginner guide is understandable without already knowing the project

## Suggested Git Commands

After committing release documentation:

```bash
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin main
git push origin v0.1.0
```

Then create the GitHub release from tag `v0.1.0`.

## Suggested GitHub Release Title

```text
v0.1.0 - ESP32 HomeKit bridge for a Tuya Wi-Fi plug
```

## Suggested GitHub Release Notes

```markdown
First experimental release of Tuya ESP32 HomeKit Bridge.

This release turns one verified Tuya / Smart Life Wi-Fi plug into a local Apple HomeKit Outlet through an ESP32 running HomeSpan. The Tuya plug keeps its original firmware. The ESP32 acts as a local LAN bridge between HomeKit and the plug's local Tuya protocol.

Verified setup:

- Tesla Smart Plug / Tuya Smart Life Wi-Fi plug
- Tuya protocol `3.4`
- relay datapoint `1`
- ESP32 development board
- Python + TinyTuya local control
- HomeSpan HomeKit Outlet sketch

Included:

- Python scripts to scan and test local Tuya control
- ESP32 local Tuya POC sketch
- ESP32 HomeSpan Outlet sketch
- generated `secrets.h` workflow
- beginner step-by-step guide
- safety and secret-handling notes

Limitations:

- experimental
- not a universal Tuya bridge
- ESP32 sketch currently targets Tuya protocol `3.4`
- only relay DPS `1` is verified
- not intended for critical loads or safety-critical switching

Start here:

- README: https://github.com/Lisejnik/tuya-esp32-homekit-bridge
- Beginner Guide: https://github.com/Lisejnik/tuya-esp32-homekit-bridge/blob/main/docs/beginner-guide.md
```

## Good GitHub Repository Settings

Before announcing the release:

- Add repository topics: `esp32`, `homekit`, `homespan`, `tuya`, `smart-life`, `tinytuya`, `iot`, `arduino`
- Add a short repository description: `Local ESP32 HomeKit bridge for Tuya / Smart Life Wi-Fi plugs`
- Enable Issues
- Consider adding Discussions only after there is enough user interest

## After Release

Good first follow-up issues:

- Add tested device compatibility table
- Add screenshots or wiring-free setup photos
- Add Arduino compile verification notes
- Extract Tuya protocol logic into a reusable C++ class
- Support configurable relay DPS
- Investigate Tuya protocol versions beyond `3.4`
