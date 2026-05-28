#pragma once

// Copy to secrets.h or run scripts/export_esp32_secrets.py from the project root.
// secrets.h is ignored by git.
#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"

#define HOMEKIT_PAIRING_CODE "11122333"
#define HOMEKIT_ACCESSORY_NAME "Tuya HomeKit Outlet"
#define HOMEKIT_MANUFACTURER "Tuya Local Bridge"
#define HOMEKIT_MODEL "Tuya Plug via ESP32"

#define TUYA_DEVICE_ID "bf6ecdd9c12f764d17xqbb"
#define TUYA_DEVICE_IP IPAddress(192, 168, 1, 123)
#define TUYA_LOCAL_KEY "CHANGE_ME"
#define TUYA_RELAY_DPS "1"
