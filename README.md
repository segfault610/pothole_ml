# Low-power pothole detection

ESP32 + ADXL345 (or MPU-6050) + NEO-7M GPS that detects **potholes** and **speed humps**
while driving, classifies them on the device with a small tree-ensemble
model, and publishes them with their GPS position to **AWS IoT Core**, where
they are aggregated onto a live **map**.

```
ADXL345 ─I2C─┐                                     ┌─► DynamoDB ─► HTTP API ─► Leaflet map / GeoJSON
             ├─ ESP32: detect → classify ─MQTT/TLS─► AWS IoT Core ─ rule ─► Lambda
NEO-7M ─UART─┘   (position at the moment of impact)
```

## Quick start

| Step | Command | Docs |
|---|---|---|
| Wire it | ADXL345 SDA→GPIO21, SCL→GPIO22, CS→3V3, SDO→GND · NEO-7M TX→GPIO16, RX→GPIO17 | [hardware](docs/01-hardware.md) |
| Check it | `cd esp32_inference && idf.py -p /dev/ttyUSB0 flash monitor` | [firmware](docs/02-firmware.md) |
| Record drives | flash `esp32_collector`, then `python ml/collect.py --label pothole` | [data collection](docs/03-data-collection.md) |
| Train | `python ml/train.py` → writes `components/pothole_model/include/pothole_model.h` | [ML pipeline](docs/04-ml-pipeline.md) |
| Deploy | `cp esp32_iot/main/secrets.example.h esp32_iot/main/secrets.h`, add certs, flash `esp32_iot` | [firmware](docs/02-firmware.md) |
| Map | `cd cloud && sam build && sam deploy --guided` | [cloud & GIS](docs/05-cloud-gis.md) |

> **Status (2026-10-17):** hardware + firmware + MQTT publishing with real GPS
> work end-to-end on an MPU-6050 (GY-521). The model (`forest_n10_d8`) is
> trained on simulated + augmented desk recordings and recognises pothole /
> speed hump / normal desk motions; it is not yet validated on a road. The
> map backend is written but not deployed. Details: [docs/07-status.md](docs/07-status.md).

> **Security:** the Wi-Fi password from the first commit is public in the
> history — change it. See [docs/06-security.md](docs/06-security.md).

## Repository layout

```
components/          shared ESP-IDF components (C)
  accel/             accelerometer drivers: ADXL345 or MPU-6050, auto-detected
  neo7m_gps/         GPS UART task + NMEA parser
  road_event/        event detector + 20 features (also used by Python)
  pothole_model/     generated model header
  board/             pin definitions
esp32_collector/     firmware: stream raw data for training
esp32_inference/     firmware: offline bench test
esp32_iot/           firmware: detection + GPS + AWS IoT MQTT
ml/                  collect.py (guided), review.py, dataset.py, augment.py, train.py, simulate.py, tests/
cloud/               AWS SAM backend (Lambda, DynamoDB, IoT rule, HTTP API, map)
model/report.md      latest training report
data/raw/            road recordings · data/bench/ desk recordings (GPS logs not committed)
                     data/legacy/ = old, unusable v1/v2 data
docs/                documentation
```

## Tests

```bash
pip install -r ml/requirements.txt
python -m pytest ml/tests -q                 # C detector/features/NMEA via ctypes, simulator
python cloud/tests/test_potholes.py          # Lambda logic, needs boto3
```
