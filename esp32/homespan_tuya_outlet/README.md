# HomeSpan Tuya Outlet

This sketch exposes an existing Tuya / Smart Life plug as an Apple HomeKit `Outlet` using HomeSpan.

It builds on the already verified ESP32 Tuya local POC:

- Tuya protocol: `3.4`
- Relay datapoint: `1` by default
- Local TCP port: `6668`

## Setup Wizard

This sketch does not require editing `secrets.h`.

On first boot, or when no saved configuration exists, the ESP32 starts a setup Wi-Fi access point:

```text
TuyaHomeKit-Setup
```

Connect to that network from a phone or computer and open:

```text
http://192.168.4.1/
```

The setup Wi-Fi password is randomly generated for each setup session and printed in Serial Monitor.

The setup page stores these values in ESP32 flash using `Preferences`:

- Wi-Fi SSID
- Wi-Fi password
- Tuya plug IP address
- Tuya device ID
- Tuya local key
- Tuya protocol version, default `3.4`
- relay DPS, default `1`
- HomeKit accessory name
- polling interval in seconds, default `30`

The setup page uses plain HTTP on the temporary ESP32 setup network. Configure it near the ESP32 and do not leave setup mode running longer than needed. Saved Wi-Fi passwords and Tuya local keys are stored in ESP32 Preferences in plaintext, but they are not shown back in the form; leave those fields blank to keep existing saved values when reconfiguring. Use an IoT or guest Wi-Fi network if possible.

Use **Test Tuya connection** before saving if you want to verify the values from the ESP32.

After **Save and restart**, the ESP32 loads the saved configuration, connects to Wi-Fi, starts HomeSpan, and exposes the plug as a HomeKit outlet.

If Wi-Fi connection fails repeatedly during boot, the sketch falls back to setup mode and periodically retries the saved Wi-Fi. If the network comes back, the ESP32 restarts into normal HomeSpan mode.

## Clear Bridge Configuration

Hold GPIO0 / BOOT while the ESP32 starts to clear saved bridge configuration and force setup mode. On some dev boards, holding BOOT before reset enters the bootloader; if that happens, press BOOT just after reset.

The setup page also has **Clear saved config** while setup mode is active. This does not clear HomeSpan pairing data.

## Arduino IDE Setup

Install:

- ESP32 board support
- HomeSpan library

For common 4 MB ESP32 boards, select a partition with at least a 2 MB app slot:

```text
Partition Scheme: No OTA (2MB APP/2MB SPIFFS)
```

The default ESP32 partition is usually too small for HomeSpan plus the setup web wizard.

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

## Why Outlet

HomeKit `Outlet` is the correct service for a physical mains plug. It gives the Home app an outlet accessory instead of a generic switch.

This sketch maps:

- HomeKit `Characteristic::On` -> configured Tuya relay DPS
- HomeKit `Characteristic::OutletInUse` -> same value as relay state

`OutletInUse` is not true power-consumption detection. It currently means "relay is on".

## Behavior

- Home app ON sends a local Tuya ON command.
- Home app OFF sends a local Tuya OFF command.
- The sketch polls Tuya status at the configured interval.
- If the plug state changes outside HomeKit, HomeKit is updated on the next poll.

Test with a safe low-power load first.
