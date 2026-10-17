# 2. Firmware (ESP-IDF)

Three ESP-IDF projects share the drivers in [`components/`](../components):

| Project | Use it for | Needs Wi-Fi/AWS |
|---|---|---|
| `esp32_collector` | recording training data with `ml/collect.py` | no |
| `esp32_inference` | bench test of wiring + model, prints every event | no |
| `esp32_iot` | the real thing: detect → classify → publish with GPS to AWS IoT | yes |

| Component | What it does |
|---|---|
| `accel` | I2C bus + auto-detected ADXL345 or MPU-6050; 100 Hz, ±8 g, **hardware FIFO** (timing comes from the sensor clock); bus scan on failure |
| `neo7m_gps` | UART2 reader task, NMEA RMC/GGA parser with checksum, UBX commands for 5 Hz |
| `road_event` | gravity tracking, event trigger, 1.5 s window, 20 features (also used by the Python trainer) |
| `pothole_model` | `pothole_model.h`, **generated** by `ml/train.py` |
| `board` | `board_pins.h`, all pin numbers |

Tested with ESP-IDF **v6.1** (also expected to work on v5.3+).

## One-time setup

```bash
. ~/esp-idf/export.sh            # or wherever your ESP-IDF lives
```

Each project has a small `sdkconfig.defaults`; `sdkconfig` and `build/` are
generated on the first build (both git-ignored). If you move the project
folder, delete `build/` (or `idf.py fullclean`) — CMake refuses a build
directory configured for another path. Notable defaults:

- `CONFIG_FREERTOS_HZ=1000`
- `esp32_iot`: `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (the app is ~0.95 MB; the default 1 MB partition was 91 % full)

If you ever change `sdkconfig.defaults`, delete `sdkconfig` (or run
`idf.py fullclean`) so the defaults are applied again.

## Build & flash

```bash
cd esp32_inference            # or esp32_collector / esp32_iot
idf.py set-target esp32       # first time only
idf.py -p /dev/ttyUSB0 flash monitor
```

Exit the monitor with `Ctrl+]`. The serial port must be free before running
`ml/collect.py`.

## esp32_iot: credentials

1. **Wi-Fi + endpoint** — copy and edit:
   ```bash
   cp esp32_iot/main/secrets.example.h esp32_iot/main/secrets.h
   ```
   `secrets.h` is git-ignored. The endpoint is in AWS IoT Core → *Settings* →
   *Device data endpoint*. In a car, a phone hotspot works fine.
2. **Certificates** — put these in `esp32_iot/certs/` (git-ignored):
   `aws_root_ca.pem`, `certificate.pem.crt`, `private.pem.key`.
   See [06-security.md](06-security.md) for creating a fresh certificate.

### What it publishes

Topic `pothole/alerts` (change `AWS_IOT_TOPIC` in `esp32_iot/main/main.c`), QoS 1:

```json
{"device_id":"pothole-a1b2c3","event":"POTHOLE","confidence":0.87,"severity_g":1.42,
 "coordinates":[12.9716012,79.1588021],"lat":12.9716012,"lon":79.1588021,
 "gps":{"fix":true,"sats":8,"hdop":1.1,"speed_kmh":32.4,"age_ms":120},
 "utc":"2026-10-17T10:22:33.400Z","uptime_ms":123456,"model":"forest_n10_d8"}
```

- The position is captured **at the trigger instant** (when the wheel hit
  it), not 1.5 s later when classification finishes.
- `coordinates` is kept so the existing MQTT logger keeps working.
- No fix → `coordinates`, `lat`, `lon` and `utc` are `null`, `gps.fix` is `false`.
- `SPEED_HUMP` events are published too (`PUBLISH_SPEED_HUMPS`); `NORMAL` never is.
- Events below `MIN_CONFIDENCE` (0.6) are not published.
- While offline, up to 32 events are queued and sent after reconnecting.

Tunables at the top of `esp32_iot/main/main.c`:
`PUBLISH_SPEED_HUMPS`, `MIN_CONFIDENCE`, `GPS_MAX_AGE_MS`, `EVENT_QUEUE_LEN`.
Detector tunables (trigger level, window) are in
`components/road_event/include/road_event.h` — changing them requires
retraining.

## Expected serial output (esp32_iot)

```
I (512) POTHOLE_IOT: device pothole-a1b2c3, model forest_n10_d8 (10 trees)
I (520) GPS: sent UBX config: 5 Hz, GGA+RMC only
I (560) ADXL345: ready: 100 Hz, +/-8 g, FIFO stream
I (2310) POTHOLE_IOT: Wi-Fi connected
I (4100) POTHOLE_IOT: MQTT connected to AWS IoT
I (30500) POTHOLE_IOT: status: gps=fix sats=9 mqtt=up queued=0
I (41210) POTHOLE_IOT: event: POTHOLE  p=[0.05 0.02 0.93]
W (41210) POTHOLE_IOT: POTHOLE DETECTED (93%, 1.87 g)
I (41260) POTHOLE_IOT: Published (msg_id=4): {"device_id":...}
```

## Why the old firmware misbehaved

- `vTaskDelay(pdMS_TO_TICKS(delay_us / 1000))` with `CONFIG_FREERTOS_HZ=100`
  rounds every delay below 10 ms to **0 ticks**, so sampling ran in bursts
  and read the same ADXL345 value several times (up to 92 % duplicate
  samples in `data/legacy/v2/normal.csv`). The new driver lets the
  sensor's own 100 Hz clock fill its FIFO.
- Classification ran on fixed 1 s windows after a countdown, so in a car
  the pothole was usually not in the window or cut in half. Now sampling is
  continuous and a window is centred on each event.
- The MQTT payload used fixed mock coordinates.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `#error "Copy esp32_iot/main/secrets.example.h..."` | create `secrets.h` (above) |
| build error about `../certs/...pem` | put the three certificate files in `esp32_iot/certs/` |
| `MQTT disconnected` loop | wrong endpoint, inactive certificate, or the certificate has no policy attached (06-security.md) |
| `FIFO overflow` warnings | something blocks the sensor task > 300 ms; don't add slow code to it |
| `static assertion failed: features changed since training` | you edited `road_event` — run `python ml/train.py` |
| no events at all while driving | check that the mount is rigid; lower `RE_TRIGGER_G` (0.25 g) and retrain |
