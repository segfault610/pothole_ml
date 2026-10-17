# Documentation

| # | Guide | Read when |
|---|---|---|
| 1 | [Hardware](01-hardware.md) | wiring the ESP32, ADXL345, NEO-7M; mounting; bring-up checks |
| 2 | [Firmware](02-firmware.md) | building/flashing the three ESP-IDF projects, credentials, MQTT payload |
| 3 | [Data collection](03-data-collection.md) | recording labelled drives with `ml/collect.py` |
| 4 | [ML pipeline](04-ml-pipeline.md) | how detection/features/training work, v1 problems, Q-learning & Pico answers |
| 5 | [Cloud & GIS](05-cloud-gis.md) | deploying the AWS backend and the live pothole map |
| 6 | [Security](06-security.md) | leaked Wi-Fi password, certificate rotation, IoT policy |

Typical order for a new setup: 1 → 2 (flash `esp32_inference`, check wiring)
→ 3 (collect with `esp32_collector`) → 4 (train) → 2 (flash `esp32_iot`) → 5.
