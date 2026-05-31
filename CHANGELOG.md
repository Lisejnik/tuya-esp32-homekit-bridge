# Changelog

## Unreleased

- Add configurable local dashboard hostname, default `tuya-homekit`.
- Advertise the admin dashboard with mDNS at `http://tuya-homekit.local:8080/`.
- Show the friendly dashboard management URL in the web UI and Serial Monitor.
- Add PWA manifest and local SVG icon endpoints for adding the dashboard to a phone home screen.
- Improve setup captive portal behavior with wildcard DNS and common OS captive-portal probe routes.

## v2.1 - Smart Setup & Diagnostics

- Modernize setup into a mobile-friendly step-based wizard.
- Add experimental LAN scan for possible Tuya devices on ports `6668` and `6669`.
- Add structured Tuya connection diagnostics with latency, port, authentication, relay DPS and suggestion details.
- Add experimental read-only DPS inspector with boolean relay-candidate selection.
- Add admin dashboard metrics and an in-memory diagnostics log.
- Update docs for v2.1 setup, dashboard, diagnostics, security, compatibility and troubleshooting.

## v2.0 - ESP32 Web Setup Wizard

- Add first ESP32 setup Wi-Fi access point and web configuration wizard.
- Store HomeSpan sketch configuration in ESP32 flash using Preferences.
- Add setup form fields for Wi-Fi, Tuya plug, HomeKit name, HomeKit type, HomeKit pairing code, relay DPS, and polling interval.
- Add LAN admin page on port `8080` after normal Wi-Fi connection.
- Add setup/admin Tuya connection test, configuration reset, and HomeKit pairing reset actions.
- Add GPIO0 / BOOT long-hold factory reset path.
- Add status LED feedback for setup, Wi-Fi success, paired idle state, and Wi-Fi/Tuya errors.
- Keep ESP32 local Tuya POC on generated `secrets.h` while removing `secrets.h` from the HomeSpan setup flow.

## v0.1.0 - Initial Experimental Release

- Add Python TinyTuya scripts for LAN discovery and local plug control.
- Add ESP32 Tuya local POC for protocol `3.4`.
- Add ESP32 HomeSpan sketch exposing the plug as a HomeKit `Outlet`.
- Add generated `secrets.h` workflow from local `.env`.
- Document safety, secret handling, HomeKit pairing, and troubleshooting.
- Add beginner guide and release checklist.
