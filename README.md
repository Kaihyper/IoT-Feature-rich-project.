# IoT TinyML Safety Monitor

A two-node home environmental safety and monitoring system combining deterministic safety logic with an on-device TinyML anomaly layer.

## What it does

One board acts as a continuous fire/gas safety hub with an always-on hard-limit check — flame or gas over threshold triggers an immediate buzzer alarm, with no ML in that path. It's backed by a TinyML anomaly layer that catches subtler multi-sensor patterns a simple threshold would miss or falsely trigger on — like gas rising alongside heat with no one around, versus the same rise while someone's clearly cooking.

A second, battery-friendly board sits in another room purely for ambient monitoring, spending almost all its time in deep sleep and waking on a timer or button press.

The two boards talk directly to each other over **ESP-NOW** (no router or internet needed for that link), so the safety hub gets real multi-room context for its anomaly detection, and can flag if the second node goes silent — separately from anything environmental. Both also report to a shared **Blynk** dashboard for remote visibility and phone alerts.

## Hardware

**ESP32-S3 (primary / safety hub)**
- DHT22 (temperature/humidity)
- MQ-2 gas sensor (behind a voltage divider)
- Flame sensor
- PIR motion sensor
- OLED display
- Passive buzzer

**ESP32 (secondary / satellite node)**
- DHT22 (or DHT11, configurable)
- MH-series analog light sensor
- Manual wake button

**Supporting passives:** 10kΩ pull-up resistors, MQ-2 voltage divider (10kΩ/20kΩ)

## Software / platform

- Arduino framework in C++, built via **PlatformIO**
- **FreeRTOS** explicit task structure on the S3 — safety check, TinyML/display, and Blynk networking run as three separate tasks pinned across both cores, so a slow network call can never delay the alarm response
- **TensorFlow Lite Micro** (via Edge Impulse) for the on-device anomaly model — an autoencoder approach so it learns "normal" per-room patterns without needing labeled failure data
- **ESP-NOW** for the direct board-to-board link
- **Blynk IoT** for the cloud dashboard and push notifications

## Standout features

- **Two-tier safety design** — a deterministic rule-based check handles anything truly time-critical; ML is deliberately kept out of that fast path and used only as a secondary safeguard.
- **Real power-awareness** — interrupt-driven digital inputs instead of polling, WiFi modem sleep, and full deep-sleep dual-wake (timer + button) on the satellite node.
- **Local resilience** — the ESP-NOW link and buzzer/alarm logic keep working even if the internet or Blynk connection drops, which matters specifically because this is a safety device.

## Repository structure

```
iot-tinyml-safety-monitor/
├── primary_node/          PlatformIO project for the ESP32-S3 safety hub
│   ├── platformio.ini
│   └── src/main.cpp
├── secondary_node/        PlatformIO project for the ESP32 satellite node
│   ├── platformio.ini
│   └── src/main.cpp
└── utilities/             Small standalone test sketches used during bring-up
    ├── get_mac_address/   Prints a board's MAC address (needed for ESP-NOW pairing)
    └── dht_sensor_test/   Isolates DHT sensor wiring issues from the rest of the firmware
```

Each subfolder (`primary_node`, `secondary_node`, and each utility) is its own independent PlatformIO project — open whichever one you're working on directly in PlatformIO/VS Code.

## Setup

1. **Get MAC addresses first.** Flash `utilities/get_mac_address` onto the ESP32-S3, note the printed address.
2. **Fill in credentials.** In both `primary_node/src/main.cpp` and `secondary_node/src/main.cpp`, replace the WiFi SSID/password and Blynk template ID/name/auth token placeholders. In `secondary_node/src/main.cpp`, also set `primaryMacAddress[]` to the ESP32-S3's MAC from step 1.
3. **Flash the primary node** (ESP32-S3) first, and leave its serial monitor open.
4. **Flash the secondary node** (ESP32). On its next wake (timer or button), it sends its reading over both Blynk and ESP-NOW.
5. Watch the primary node's serial monitor for incoming `Room 2 update` messages confirming the ESP-NOW link is working.

## Known placeholders to calibrate

- `GAS_HARD_LIMIT_RAW` in `primary_node/src/main.cpp` — set after the MQ-2's 24–48h burn-in period, calibrated against your own baseline readings.
- `runAnomalyCheck()` in `primary_node/src/main.cpp` — currently a simple heuristic stand-in. Replace with the real Edge Impulse–exported inference call once the TinyML model is trained.
- Flame sensor polarity in `onFlameChange()` — verify against your specific module's datasheet.

## License

MIT — see [LICENSE](LICENSE).
