# Beginner Guide

This guide is for people who can follow careful steps but are not yet comfortable with Python, ESP32 flashing, Tuya local keys, or HomeKit development.

Do not skip Phase 1. The Python test proves whether your Tuya plug can be controlled locally. If Phase 1 fails, the ESP32 and HomeKit steps will also fail.

## What You Are Building

You will keep the original Tuya / Smart Life plug firmware. Nothing is flashed to the plug.

The ESP32 becomes a small bridge:

```text
Apple Home app -> ESP32 HomeKit accessory -> Tuya plug over local Wi-Fi
```

The Tuya cloud is only used once to get the plug's `local_key`. After that, on/off control is local on your LAN.

## What You Need

- A Tuya / Smart Life Wi-Fi plug already added in the Smart Life or Tuya mobile app
- An ESP32 development board
- A USB cable that supports data, not only charging
- A Mac, Linux computer, or Windows computer with Python and Arduino IDE
- A 2.4 GHz Wi-Fi network used by the plug and ESP32
- A small safe test load, such as a lamp
- Apple Home app on iPhone, iPad, or Mac

Recommended:

- Reserve a fixed IP address for the plug in your router
- Reserve a fixed IP address for the ESP32 after the first successful test

## Important Safety Rules

Test with a low-power lamp first.

Do not use this project for heaters, pumps, medical devices, unattended appliances, or anything that could be dangerous if it turns on or off unexpectedly.

## Words You Will See

- `Device ID`: Tuya's identifier for your plug.
- `Device IP`: the plug's local network address, for example `192.168.1.123`.
- `local_key`: a 16-character secret used by the local Tuya protocol.
- `Tuya protocol version`: usually `3.3`, `3.4`, or `3.5`; this project currently targets `3.4` on ESP32.
- `DPS`: Tuya datapoint. For the tested plug, relay on/off is DPS `1`.
- `secrets.h`: a local Arduino file containing Wi-Fi and Tuya secrets. It must not be committed.

## Phase 0: Download the Project

Open Terminal and clone the repository:

```bash
git clone https://github.com/Lisejnik/tuya-esp32-homekit-bridge.git
cd tuya-esp32-homekit-bridge
```

If you downloaded a ZIP from GitHub instead, unzip it and open Terminal inside the unzipped folder.

## Phase 1: Prepare Python

Create a Python virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

On Windows PowerShell, activation is usually:

```powershell
.\.venv\Scripts\Activate.ps1
```

You should now be able to run:

```bash
python --version
```

## Phase 2: Find the Plug on Your Network

If you know the plug IP address from your router, run:

```bash
python scripts/scan_devices.py --target-ip 192.168.1.123
```

Replace `192.168.1.123` with your plug IP.

Write down:

- plug IP address
- Device ID
- Tuya protocol version

If no device is found:

- make sure your computer and plug are on the same network
- close the Smart Life / Tuya app and try again
- check that the plug is powered on
- check the plug IP address in your router

## Phase 3: Get the Tuya local_key

You need a Tuya IoT Platform account. The Smart Life mobile-app account alone is not enough.

High-level steps:

1. Open [Tuya IoT Platform](https://iot.tuya.com/).
2. Create a cloud project.
3. Link your Smart Life / Tuya app account to the project.
4. Open API Explorer in the project.
5. Use `IoT Core` -> `Device Management` -> `Query Device Details`.
6. Enter the Device ID from the scan.
7. Copy `result.local_key`.

Treat the `local_key`, generated API Explorer `curl` command, access tokens, and Tuya Access Secret as passwords.

## Phase 4: Create .env

Copy the example file:

```bash
cp .env.example .env
```

Open `.env` in a text editor and fill your values:

```text
DEVICE_ID=your_device_id
DEVICE_IP=192.168.1.123
LOCAL_KEY=your_16_character_local_key
DEVICE_VERSION=3.4
```

For the ESP32 sketches in this repository, `DEVICE_VERSION` must be `3.4`.

## Phase 5: Test Local Tuya Control from Python

Read status first:

```bash
python scripts/test_plug.py status
```

If status works, test switching:

```bash
python scripts/test_plug.py on
python scripts/test_plug.py off
```

Stop here if status or switching fails. Fix Python local control before continuing.

Most common causes:

- wrong `LOCAL_KEY`
- wrong `DEVICE_VERSION`
- wrong plug IP address
- computer and plug are not on the same LAN
- plug uses a different relay DPS than `1`

## Phase 6: Install Arduino IDE Tools

Install:

- [Arduino IDE](https://www.arduino.cc/en/software)
- ESP32 board support in Arduino IDE
- HomeSpan library in Arduino IDE Library Manager

The first ESP32 POC does not need HomeSpan, but the final HomeKit sketch does.

## Phase 7: Generate ESP32 secrets.h

From the project root, run:

```bash
python scripts/export_esp32_secrets.py
```

This creates:

```text
esp32/tuya_local_poc/secrets.h
esp32/homespan_tuya_outlet/secrets.h
```

Open both generated files later if needed, but do not commit them.

## Phase 8: Flash the ESP32 Tuya Local POC

In Arduino IDE, open:

```text
esp32/tuya_local_poc/tuya_local_poc.ino
```

Edit the generated file:

```text
esp32/tuya_local_poc/secrets.h
```

Fill your Wi-Fi:

```cpp
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASSWORD "your_wifi_password"
```

Select your ESP32 board and port, then upload the sketch.

Open Serial Monitor at `115200` baud and type:

```text
status
on
off
```

Only continue when all three commands work reliably.

## Phase 9: Flash the HomeKit Outlet Sketch

In Arduino IDE, open:

```text
esp32/homespan_tuya_outlet/homespan_tuya_outlet.ino
```

Edit:

```text
esp32/homespan_tuya_outlet/secrets.h
```

Fill Wi-Fi and optionally rename the accessory:

```cpp
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASSWORD "your_wifi_password"
#define HOMEKIT_ACCESSORY_NAME "Tuya HomeKit Outlet"
```

Upload the sketch and open Serial Monitor at `115200`.

## Phase 10: Set HomeKit Pairing Code

In Serial Monitor, set your own 8-digit pairing code:

```text
S 11223344
```

Use your own code, not the example. Write it down.

Apple Home displays HomeKit codes with hyphens. For example, `S 11223344` becomes:

```text
112-23-344
```

## Phase 11: Add to Apple Home

Open Apple Home and add a new accessory.

If there is no QR code, choose the option to add manually and enter your HomeKit code.

After pairing:

- turn the outlet on from Apple Home
- turn it off from Apple Home
- press the physical plug button and wait up to 30 seconds for HomeKit state to update

## When to Open a GitHub Issue

Open an issue if Python control works but ESP32 or HomeKit control does not.

Include:

- plug model
- ESP32 board type
- Tuya protocol version
- relay DPS if known
- sanitized Serial Monitor logs

Do not include:

- `local_key`
- Wi-Fi password
- Tuya Access Secret
- generated API Explorer `curl` command
- screenshots showing tokens or secrets
