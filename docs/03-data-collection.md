# 3. Collecting training data

> **The model shipped in the repo is trained on simulated data.** It is a
> placeholder that proves the pipeline end-to-end; the firmware prints a
> warning while it is in use. Real accuracy only comes from real drives
> with *your* car, *your* mount and *your* roads. Plan for ~1–2 hours of
> driving in total.

## Setup

```bash
pip install -r ml/requirements.txt         # or the conda env you use
cd esp32_collector && idf.py -p /dev/ttyUSB0 flash && cd ..
```

Laptop in the car, ESP32 on USB, sensor **rigidly mounted** in its final
position (see [01-hardware.md](01-hardware.md#mounting--this-matters-as-much-as-the-ml)).
You need a driver **and a passenger** — the passenger handles the laptop.

## Recording a session

```bash
python ml/collect.py --label pothole        # Ctrl+C when done
python ml/collect.py --label speed_hump
python ml/collect.py --label normal --minutes 10
```

Recording is continuous. The terminal shows live progress, GPS state, and
every event the on-device detector triggers on:

```
RECORDING 'pothole' -> data/raw/pothole/20261017_101500.csv
Press ENTER every time you go over a pothole.

  event #1 at sample 4120: device model says pothole (0.81)
  >>> MARK #1 at sample 4190
       50 s  samples=5000  events=3  marks=1  lost=0  | GPS 12.97163,79.15881 23.4 km/h sats=9
```

**The passenger presses ENTER (or the ESP32's BOOT button) right after each pothole / hump.** That
"mark" is how the trainer knows which detected events are real. Unmarked
events in a pothole or hump session are ignored (they were probably a
rough patch). Late presses are fine — anything within 1 s before to 2 s
after the trigger counts.

For **normal** sessions no marks are needed: drive normal roads, including
rough patches, expansion joints, railway crossings, manhole covers, gravel,
braking and accelerating. Every event detected there becomes a "normal"
example — these are exactly the things the model must learn *not* to call a
pothole, so do not avoid them.

## How much data

| Class | Target | Tips |
|---|---|---|
| pothole | **≥ 150 events** over **≥ 5 sessions** | different sizes, depths, one wheel vs. both, 10–50 km/h |
| speed_hump | **≥ 150 events** over **≥ 5 sessions** | different hump types (wide tables, narrow rubber humps, rumble strips), speeds |
| normal | **≥ 150 events** over **≥ 5 sessions** | rough roads, joints, gravel — anything that triggers but is not a pothole/hump |

Several sessions per class matter more than many events in one session:
the trainer cross-validates **by session**, so it can only give honest
numbers when there are at least 3 sessions per class.

Driving the same street back and forth is fine — vary speed and lane
position. Stay safe: the driver drives, the passenger operates the laptop,
obey speed limits; nobody needs to hit a pothole fast.

## Reviewing labels (optional, recommended)

```bash
python ml/review.py data/raw/pothole/20261017_101500.csv          # ASCII
python ml/review.py data/raw/pothole/20261017_101500.csv --plot   # matplotlib
```

For each event: `ENTER` keep, `n`/`h`/`p` relabel, `d` discard, `q` save
and quit. Decisions go to `<session>.labels.json` and override the marks.

## Files

```
data/raw/<label>/<YYYYmmdd_HHMMSS>.csv          seq,x,y,z  (every sample, g)
data/raw/<label>/<YYYYmmdd_HHMMSS>.marks        ENTER presses (sample index)
data/raw/<label>/<YYYYmmdd_HHMMSS>.gps.csv      1 Hz GPS log
data/raw/<label>/<YYYYmmdd_HHMMSS>.labels.json  review.py overrides
```

Raw recordings are small (≈ 1.8 MB per 10 min) and worth committing — they
let you retrain whenever the features change. `data/legacy/` holds the old
v1/v2 recordings; they are **not** used (wrong timing, see
[04-ml-pipeline.md](04-ml-pipeline.md)).

## Bench sessions (no car) — rehearse the workflow

Hand motion is not a car, so bench data goes to `data/bench/`, **not**
`data/raw/`, and is never mixed into the road model. It is useful for
checking the wiring, the detector, marks and training end-to-end, and for a
desk demo. Hold the breadboard flat, sensor mounted firmly on it, and keep
it at the same angle throughout.

| Session | Command | What to do |
|---|---|---|
| normal (2 min) | `python ml/collect.py --label normal --out data/bench --minutes 2` | "cruising": slow, gentle up-down sway (1–2 Hz, 1–2 cm) like a car body; every ~10 s a light knock on the table (road joint) or 2 s of fine jitter (rough patch). No button presses. |
| speed hump (≈2 min, 20×) | `python ml/collect.py --label speed_hump --out data/bench --minutes 2` | every ~5 s: smooth **lift** 5–8 cm over ~0.4 s, come back down over ~0.4 s, small settle bounce. Press **BOOT** right after. |
| pothole (≈2 min, 20×) | `python ml/collect.py --label pothole --out data/bench --minutes 2` | every ~5 s: quick small **drop** (2–3 cm, ~0.1 s), then a sharp **jolt up** (tap the board's edge on the table), settle. Press **BOOT** right after. |

Then `python ml/train.py --data data/bench --header /tmp/bench_model.h`
to see how separable your motions are, without overwriting the firmware's model.

## Simulator

The simulator produces car-like data without any hardware:

```bash
python ml/simulate.py --out data/synthetic --sessions 8 --segments 40
python ml/train.py --data data/synthetic
```


Next: [04-ml-pipeline.md](04-ml-pipeline.md) → `python ml/train.py`.
