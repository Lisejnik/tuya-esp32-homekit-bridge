# ESP32 Tuya Local POC

This is the first ESP32 step: prove that the ESP32 can talk directly to the existing Tuya plug on the LAN using Tuya protocol `3.4`.

It intentionally does not use HomeSpan yet. HomeKit will be added only after this POC can reliably run:

- `status`
- `on`
- `off`

over Serial Monitor.

## Known Device Values

- Device IP: read from generated `secrets.h`
- Protocol: `3.4` on the tested plug
- Relay datapoint: `1` on the tested plug
- TCP port: `6668`

The real `LOCAL_KEY` is not stored in this repository.

## Generate secrets.h

From the project root:

```bash
python scripts/export_esp32_secrets.py
```

Then edit the generated, ignored file:

```bash
nano esp32/tuya_local_poc/secrets.h
```

Fill:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASSWORD "..."
```

Do not commit `secrets.h`.

## Arduino IDE Setup

The sketch uses ESP32 Arduino's bundled Wi-Fi and mbedTLS headers for AES-ECB and HMAC-SHA256. It does not require HomeSpan or ArduinoJson yet.

Open:

```text
esp32/tuya_local_poc/tuya_local_poc.ino
```

Select an ESP32 board, flash it, then open Serial Monitor at `115200`.

Type one command per line:

```text
status
on
off
```

Test only with a safe low-power load.

## Current Risk

This is an experimental Tuya `3.4` local protocol implementation based on TinyTuya's observed message flow:

- session key negotiation on commands `3`, `4`, `5`
- encrypted `55AA` packets
- AES-ECB with PKCS#7 padding
- HMAC-SHA256 packet signing
- `CONTROL_NEW` command `0x0d`
- `DP_QUERY_NEW` command `0x10`

If this POC fails, the next fallback is to compare ESP32 packet logs against TinyTuya debug output before adding HomeSpan.
