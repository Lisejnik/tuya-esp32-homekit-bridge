# ESP32 Phase

Phase 1 confirmed local Tuya control from macOS:

- Tuya protocol: `3.4` on the tested plug
- Relay datapoint: `1` on the tested plug
- Device IP comes from your local `.env`

The local Tuya ESP32 POC is in:

```text
esp32/tuya_local_poc/
```

This POC exposes only a Serial Monitor interface:

- `status`
- `on`
- `off`

HomeSpan is intentionally not included in the POC. Use it first to prove local Tuya control before flashing the HomeKit sketch.

The HomeSpan Outlet sketch is in:

```text
esp32/homespan_tuya_outlet/
```

It exposes the plug as a HomeKit `Outlet` and maps HomeKit `On` to Tuya DPS `1`.

Checklist after the first HomeSpan test:

- confirm pairing and control from Apple Home,
- confirm state sync when the plug is changed outside HomeKit,
- decide whether `OutletInUse` should remain relay-based or later use measured power DPS,
- refactor the Tuya protocol code into a reusable C++ class once the HomeSpan sketch is proven.

If the ESP32 POC cannot reliably support Tuya protocol `3.4`, the project must not pretend support exists. The fallback is to compare ESP32 packets with TinyTuya debug output and either fix the local protocol implementation or choose a different hardware path.
