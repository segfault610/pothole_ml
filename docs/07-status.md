# 7. Project status

*Last updated: 2026-10-22*

## Where things stand

| Area | Status |
|---|---|
| Hardware | ✅ ESP32 NodeMCU + **MPU-6050 (GY-521)** + NEO-7M working on the bench. GPS fix with 6–8 satellites, HDOP ≈ 1–3. |
| Accelerometer stream | ✅ 100 Hz, 0 lost / 0 duplicate samples, ~2 mg noise at rest |
| Firmware | ✅ `esp32_iot` flashed: continuous detection → classification → MQTT to AWS IoT with real GPS coordinates; no false events at rest |
| Model | ⚠️ `forest_n10_d8` (10 trees, ~17 KB). Works on desk motions; **not yet validated on a road**; potholes are synthetic |
| Data collection | ✅ guided mode (`ml/collect.py --guided`) works after two bug fixes |
| Cloud: MQTT ingest | ✅ device publishes to `pothole/alerts` on the project's own AWS account (ap-south-1), provisioned with `cloud/provision_device.sh` |
| Cloud: storage + map | ✅ **deployed** 2026-10-22 (SAM stack `pothole-map`); real pothole and hump reports from the board appear on the live map — see [08-aws-deployment.md](08-aws-deployment.md) |
| Security | ⚠️ Wi-Fi password from the first commit is still in the public history — change it ([06-security.md](06-security.md)) |

## The deployed model

Trained by:

```bash
python ml/simulate.py --out data/synthetic --sessions 8 --segments 40   # car physics
python ml/augment.py  --src data/bench --out data/bench_aug             # from real desk recordings
python ml/train.py    --data data/synthetic data/bench_aug data/bench
```

| | |
|---|---|
| Training events | normal 398 · speed hump 660 · pothole 567 (58 sessions) |
| Session-grouped CV macro-F1 | 0.88 |
| Held-out real motions (never seen in any form) | drop-then-lift → **91 % pothole**; lift-then-down → **80 % speed hump** |
| Desk test on the device | pothole, speed hump and normal all recognised |
| Flash / RAM | ~17 KB model, uint8-quantised leaf probabilities, C export = scikit-learn on 100 % of events |

Real data behind it (`data/bench/`, hand motion, MPU-6050 mounted upright):
2 normal sessions (15 events) and 2 speed-hump sessions (87 clean lift-first
events after discarding 42 failed attempts via `.labels.json`). There is
**no clean real pothole recording** — pothole examples are the real humps
mirrored vertically plus parametric drop → jolt → ringing events.

Limits: desk motion is not a car. Before relying on it on a road, record
drives ([03-data-collection.md](03-data-collection.md)) and retrain.

## What went wrong today, and the fixes

Kept here so the same traps are not hit again.

| Problem | Symptom | Fix (commit) |
|---|---|---|
| Stale `build/` folders from when the project lived at `CloudComputing/pothole_ml` | `idf.py build` refused: "configured for project ... not ..." | deleted; never commit/move `build/` (`f38bad2`) |
| ADXL345 (GY-291) never answered on I2C, even with CS→3V3, pull-ups and power-cycling | bus scan: no device, SDA=SCL=1 | module presumed dead; switched to GY-521. Firmware auto-detects either (`b6b17f6`) |
| Guided collection rejected every rep as MISSED | 71 hump / 37 pothole events detected, 0 accepted | events were numbered from file row 0 but GO was timed with the ESP32's counter (>10 000 since boot) (`5d8e835`) |
| Guided marks matched failed attempts nearby | 40 "potholes" from 20 reps | exact matching for guided sessions (`46cf45f`) |
| MPU-6050 FIFO desynchronised | a whole session of random ±8 g values; model learnt "noise = pothole" | read each sample on DATA_RDY instead of the byte FIFO; live warning in `collect.py`; `dataset.py` drops implausible events (`e58f811`) |
| MPU-6050 returns 0 g as its first sample after reset | a false SPEED_HUMP every 1.3 s at rest (~20 were published to AWS before the fix) | fast gravity convergence during warm-up (`991a709`) |
| Pothole never predicted on the device | drop-then-lift → SPEED HUMP | 35 % of hump labels were drop-first failed attempts; cleaned, and potholes now built from mirrored real humps (`97b96fc`) |
| Board still on a previous account (2026-10-22) | `Wi-Fi disconnected`, `couldn't get hostname for ...ap-southeast-2...` | new certs/endpoint via `cloud/provision_device.sh`, new `secrets.h` |
| Potholes "missing" on the map (2026-10-22) | only hump markers visible | desk events are metres apart; hump marker covered the pothole one — `map.html` draws potholes on top |
| `idf.py flash` failing | "Could not open /dev/ttyUSB0, the port is busy" | close `idf.py monitor` (Ctrl+]) or any other serial program first |

## Next steps

1. ~~Deploy the cloud backend~~ — done 2026-10-22 ([08-aws-deployment.md](08-aws-deployment.md)).
   Replace the root access key used for it with an IAM user key.
2. **Road data** — a few guided/free sessions in a car, then retrain on
   `data/raw` (+ synthetic) and drop the bench data from the road model.
3. **Rotate the leaked Wi-Fi password.**
