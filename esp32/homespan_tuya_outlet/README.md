# HomeSpan Tuya Outlet

This sketch exposes the existing Tuya plug as an Apple HomeKit `Outlet` using HomeSpan.

It builds on the already verified ESP32 Tuya local POC:

- Tuya protocol: `3.4`
- Plug IP: read from generated `secrets.h`
- Relay datapoint: `1`
- Local TCP port: `6668`

## Why Outlet

HomeKit `Outlet` is the correct service for a physical mains plug. It gives the Home app an outlet accessory instead of a generic switch.

This sketch maps:

- HomeKit `Characteristic::On` -> Tuya DPS `1`
- HomeKit `Characteristic::OutletInUse` -> same value as DPS `1`

`OutletInUse` is not true power-consumption detection. It currently means "relay is on".

## Generate secrets.h

From the project root:

```bash
python scripts/export_esp32_secrets.py
```

Then edit the generated, ignored file:

```bash
nano esp32/homespan_tuya_outlet/secrets.h
```

Fill:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASSWORD "..."
#define HOMEKIT_ACCESSORY_NAME "Tuya HomeKit Outlet"
```

Do not commit `secrets.h`.

## Arduino IDE Setup

Install:

- ESP32 board support
- HomeSpan library

Open:

```text
esp32/homespan_tuya_outlet/homespan_tuya_outlet.ino
```

Flash the ESP32 and open Serial Monitor at `115200`.

Before pairing, set your own HomeKit pairing code through the HomeSpan Serial CLI:

```text
S 11223344
```

The code must be exactly 8 digits. Type it without hyphens in Serial Monitor and write it down. In Apple Home, enter the same code in HomeKit format, for example `112-23-344` for `S 11223344`.

Use your own code, not the example above.

Then pair it in Apple Home as a new accessory.

If pairing data from an older HomeSpan sketch is already stored on the ESP32, use the HomeSpan Serial CLI to clear pairing data before pairing again.

## Behavior

- Home app ON sends a local Tuya ON command.
- Home app OFF sends a local Tuya OFF command.
- The sketch polls Tuya status every 30 seconds.
- If the plug state changes outside HomeKit, HomeKit is updated on the next poll.

Test with a safe low-power load first.
