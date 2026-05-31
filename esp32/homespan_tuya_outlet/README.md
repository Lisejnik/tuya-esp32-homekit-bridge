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

The v2.1 setup page is a lightweight step-based wizard:

1. Wi-Fi setup
2. Find Tuya device
3. Enter Tuya credentials
4. Test connection
5. HomeKit settings
6. Save and reboot

The setup page stores these values in ESP32 flash using `Preferences`:

- Wi-Fi SSID
- Wi-Fi password
- Tuya plug IP address
- Tuya device ID
- Tuya local key
- Tuya protocol version, default `3.4`
- relay DPS, default `1`
- HomeKit accessory name
- HomeKit type, default `Outlet`
- optional HomeKit pairing code
- polling interval in seconds, default `30`

The setup page uses plain HTTP on the temporary ESP32 setup network. Configure it near the ESP32 and do not leave setup mode running longer than needed. Saved Wi-Fi passwords and Tuya local keys are stored in ESP32 Preferences in plaintext, but they are not shown back in the form; leave those fields blank to keep existing saved values when reconfiguring. Use an IoT or guest Wi-Fi network if possible.

Use **Test Tuya connection** before saving if you want structured diagnostics from the ESP32. The result shows whether the IP/port is reachable, which protocol version is used, whether authentication looks valid, whether the configured relay DPS was found, current relay state if available, response latency, and practical suggestions.

The **Find Tuya devices** button is experimental. It scans the ESP32 local subnet for open Tuya-like TCP ports `6668` and `6669`, then labels results as `possible Tuya device` or `likely Tuya device`. It cannot prove that a device is Tuya and it cannot discover the `local_key`.

The **Scan DPS** button is also experimental and read-only. It queries the status payload, lists returned Tuya datapoints, labels obvious boolean values as possible relay/switch candidates, and lets you copy a boolean datapoint into the relay DPS field. It does not toggle unknown datapoints.

After **Save and restart**, the ESP32 loads the saved configuration, connects to Wi-Fi, starts HomeSpan, and exposes the plug as a HomeKit outlet.

After normal boot, Serial Monitor prints an admin URL with the ESP32's LAN IP address and port `8080`. Open that URL from the same Wi-Fi network to edit saved settings, test Tuya connectivity, inspect DPS values, restart the ESP32, clear bridge configuration, or clear HomeKit pairing on the ESP32.

The admin dashboard shows:

- HomeKit accessory name, type, relay state and polling interval
- Tuya IP, protocol version, relay DPS, last response status, last latency and failed poll count
- Wi-Fi SSID, ESP32 IP, RSSI, uptime and free heap
- recent diagnostics events from a small in-memory log

The admin page is plain local HTTP without login, so use it only on a trusted LAN or IoT network. Do not expose it to the internet.

If Wi-Fi connection fails repeatedly during boot, the sketch falls back to setup mode and periodically retries the saved Wi-Fi. If the network comes back, the ESP32 restarts into normal HomeSpan mode.

## Reset Options

- To edit settings, open the admin URL and save new values.
- To clear only Wi-Fi and Tuya settings, use **Clear saved config** on the admin/setup page.
- To clear HomeKit pairing, use **Clear HomeKit pairing** on the admin page and remove the accessory in Apple Home too.
- To restart the ESP32, press **EN**.
- To start fully from scratch, hold the ESP32 **BOOT / GPIO0** button for about 8 seconds while it is running. This clears bridge configuration, HomeKit pairing data, and the HomeKit device ID, then restarts into setup mode.

Holding BOOT while the ESP32 starts still forces setup mode by clearing bridge configuration. On some dev boards, holding BOOT before reset enters the bootloader; if that happens, release BOOT and reset again.

## Status LED

- Setup mode: fast blink.
- Wi-Fi connected successfully: 10 slow blinks, then off.
- Normal running, paired with HomeKit, no current error: steady dim light around 1% brightness.
- Wi-Fi disconnected or Tuya plug not responding: dim SOS blink pattern.

The default status LED pin is `GPIO2`, which matches many ESP32 Dev Module boards. If your board uses another LED pin, change `STATUS_LED_PIN` in the sketch. If the built-in LED is wired differently, adjust `STATUS_LED_ON`.

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

Before pairing, enter your own 8-digit HomeKit pairing code in the setup page and write it down. The HomeKit pairing code is separate from the setup AP password and from your Wi-Fi password. If the web field is left blank and HomeSpan's code has never been changed, HomeSpan uses its default setup code `466-37-726`.

You can also set the code through the HomeSpan Serial CLI:

```text
S 11223344
```

The code must be exactly 8 digits. Type it without hyphens in Serial Monitor and write it down. In Apple Home, enter the same code in HomeKit format, for example `112-23-344` for `S 11223344`.

Use your own code, not the example above.

Then pair it in Apple Home as a new accessory.

If pairing data from an older HomeSpan sketch is already stored on the ESP32, use the HomeSpan Serial CLI or the admin page **Clear HomeKit pairing** action before pairing again. Also remove the accessory in Apple Home.

## HomeKit Type

HomeKit `Outlet` is the correct default service for a physical mains plug. Use `Light` only when the plug controls a lamp, or `Switch` for a generic on/off device. If you change the HomeKit type later, remove the accessory in Apple Home, clear HomeKit pairing on the ESP32, restart, and add it again.

This sketch maps:

- HomeKit `Characteristic::On` -> configured Tuya relay DPS
- HomeKit `Characteristic::OutletInUse` -> same value as relay state when type is `Outlet`

`OutletInUse` is not true power-consumption detection. It currently means "relay is on".

## Behavior

- Home app ON sends a local Tuya ON command.
- Home app OFF sends a local Tuya OFF command.
- The sketch polls Tuya status at the configured interval.
- If the plug state changes outside HomeKit, HomeKit is updated on the next poll.

Test with a safe low-power load first.
