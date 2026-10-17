# 4. ML pipeline

```
 collect.py ──► data/raw/<label>/*.csv (+ .marks)
                        │
 dataset.py   firmware's C detector + C features (via ctypes)
                        │
 train.py     trees / forests, session-grouped CV, pick best
                        │
              components/pothole_model/include/pothole_model.h ──► rebuild firmware
```

```bash
python ml/train.py                         # uses data/raw
python ml/train.py --data data/raw data/synthetic   # mix (not recommended for deployment)
python -m pytest ml/tests -q               # host tests of the C code + pipeline
```

`train.py` prints a comparison table and writes the header plus
[`model/report.md`](../model/report.md) (confusion matrix, feature importance).
No manual copying is needed anymore: all three firmware projects compile
the same generated header.

## What was wrong with v1

Measured on the old recordings in `data/legacy/`:

| Problem | Evidence | Effect |
|---|---|---|
| **Labels did not match the signal.** `collect.py` slept 4 s per window (countdown + gap) while the ESP32 kept streaming; the next "1 s window" was then read from the stale serial buffer. | Median window span was **4–4.5 s** instead of 1 s, up to 12 s, with a multi-second jump inside the window. | The model learnt from data recorded *before* the pothole/hump. |
| **Duplicate samples.** `vTaskDelay(<10 ms)` = 0 ticks at `FREERTOS_HZ=100`. | 92 % of consecutive samples identical in `normal.csv`, 43 % in `speed_hump.csv`. | Effective sample rate far below 100 Hz, aliasing. |
| **Event not aligned in the window.** Fixed 1 s windows after a countdown. | — | The same pothole looks completely different depending on where it falls in the window. |
| **Orientation-dependent features.** `x/y/z` mean/min/max depend on how the sensor is mounted and tilted. | `z_mean` ≈ 0.87 g, `x_mean` ≈ 0.1 g → the sensor was tilted ~10–30°. | Thresholds such as `x_mean <= 0.083` learn the mounting, not the road. |
| **No shape information.** Min/max/std cannot tell *which came first*. | — | A pothole (drop, then impact) and a hump (lift, then drop) have similar statistics. |
| **Leaky evaluation.** Random split of windows from the same session; 20 windows per class. | — | Accuracy numbers looked fine and did not transfer to the road. |

## v2 design

### Event detection (`components/road_event`)

1. A slow low-pass filter (τ ≈ 1 s) tracks the **gravity vector**, so any
   mounting angle works. During an event it adapts 10× slower.
2. Each sample is split into **vertical** `v = a·ĝ − |g|` and **horizontal**
   `h = |a − g − v ĝ|` dynamic acceleration.
3. `|v| > 0.25 g` triggers. The window is **0.5 s before + 1.0 s after** the
   trigger (150 samples), so it contains the run-up, the front wheel and the
   rear wheel (wheelbase/speed later) at a fixed position.
4. A 0.3 s cooldown follows. Flat road never triggers → never classified →
   no false positives from it, and nearly no CPU.

### Features (20)

| # | Feature | Why it helps |
|---|---|---|
| 0–4 | `v_max`, `v_min`, `v_p2p`, `v_std`, `v_mean` | size of the event |
| 5 | `first_peak` | **sign of the first big excursion: drop first = pothole, lift first = hump** |
| 6 | `t_max_minus_t_min` | ordering of the extremes (> 0 when the dip comes first) |
| 7 | `active_time` | humps last longer than potholes at the same speed |
| 8 | `jerk_max` | potholes have sharp edges |
| 9 | `hf_ratio` | high-frequency share of the energy |
| 10 | `crest` | spiky vs. smooth |
| 11–13 | `h_max`, `h_std`, `h_v_ratio` | single-wheel potholes cause roll; humps are usually full-width |
| 14 | `zcr` | oscillation rate |
| 15 | `pre_rms` | roughness of the road just before → separates rough patches |
| 16–17 | `neg_area`, `pos_area` | how much "down" vs. "up" |
| 18–19 | `skew`, `kurtosis` | asymmetry / peakedness |

The Python side never re-implements these: `ml/road_event.py` compiles the
firmware's `road_event.c` into a shared library and calls it with ctypes.

### Model

Candidates: decision trees (depth 3–8), random forests and extra-trees
(10/25 trees, depth 4–8), all with balanced class weights. Selection by
**macro-F1 under session-grouped cross-validation** (no session in both
train and test). The smallest model within 0.01 of the best score wins.

Export: flat node arrays in flash; leaf class probabilities quantised to
`uint8` (this is the "quantisation" step — trees need no other). A 10-tree
depth-8 forest is ~660 nodes ≈ 8 KB and runs in a few microseconds. The
exporter checks that the C version reproduces scikit-learn exactly.

### Results so far (synthetic data only)

From the quarter-car simulator (`ml/simulate.py`: random car, speed,
mounting angle, ISO 8608 road roughness, joints, manholes, rough patches),
24 sessions, 534 events, session-grouped 5-fold CV:

| Pipeline | macro-F1 |
|---|---|
| v1 features (18 axis stats) + decision tree | 0.20 |
| v1 features + random forest | 0.28 |
| v2 features + best decision tree | 0.84 |
| **v2 features + random forest (10 × depth 8)** | **0.92** |

Most important features: `t_max_minus_t_min`, `first_peak`, `hf_ratio`,
`jerk_max` — the shape cues v1 did not have. The v1 features score below
chance because, with random mounting angles, axis statistics mostly encode
the mount.

These numbers say the pipeline works; they do **not** predict real-road
accuracy. Retrain on real drives ([03-data-collection.md](03-data-collection.md))
and read `model/report.md`.

### Growing a small dataset (`ml/augment.py`)

```bash
python ml/augment.py --src data/bench --out data/bench_aug --per-class 300
python ml/train.py --data data/synthetic data/bench_aug data/bench
```

Real events are re-used with random rotation, amplitude scaling (0.7–1.35×),
time stretch (0.85–1.2×) and fresh noise, embedded in real background
signal; classes without enough real examples are filled with parametric
events (pothole: drop → sharp jolt → ringing; hump: smooth lift → down;
normal: knocks, double knocks, shaking bursts) sized to the recorded
motions. The current firmware model was trained this way (car simulator +
augmented bench + 144 real bench events): session-grouped CV macro-F1 0.90;
trained without the real recordings it classifies them with 95 % accuracy
(the augmented data derives from those events, so this is not a fully
independent test, and potholes have no clean real recordings yet).

### Tuning

| Knob | Where | Effect |
|---|---|---|
| `RE_TRIGGER_G` (0.25 g) | `road_event.h` | lower = catches small potholes / slow humps, more normal events |
| `RE_PRE`, `RE_POST` | `road_event.h` | window; POST must cover wheelbase/speed at your slowest speed |
| `MIN_CONFIDENCE` (0.6) | `esp32_iot/main/main.c` | higher = fewer false alerts, more misses |
| candidates | `ml/train.py: candidates()` | add models; keep them small |

After changing anything in `road_event`, run `python ml/train.py` (the
firmware refuses to compile if the feature count changed).

## Why not Q-learning?

Q-learning (reinforcement learning) learns a **policy**: which **action** to
take in a **state** so that the **long-term reward** is maximised, where
actions change future states (games, robot control, routing). Classifying a
road event is not that:

- The "action" (calling it pothole/hump/normal) does not influence the next
  road event — there is no sequence to optimise, so Q-learning collapses to a
  one-step contextual bandit.
- We **have the correct labels**. RL would throw that away and only learn
  from a reward for the chosen answer — strictly less information than
  supervised learning gets.
- Tabular Q-learning needs a discretised state; binning 20 continuous
  features destroys the information that separates the classes.

So it would be less accurate, not more, and harder to train. The accuracy
problem in v1 was **data and features**, which v2 fixes. Places where RL
*could* make sense later: adaptively tuning the trigger threshold per road
type, or duty-cycling the GPS/Wi-Fi to save battery — both optional.

If you want something stronger than a forest after collecting real data, the
natural next step is a small 1-D CNN on the 150-sample vertical/horizontal
window (still supervised), exported with TFLite Micro / ESP-NN. Only worth it
with several hundred real events per class.

## Hardware alternatives (Pico 1 WH, PyTorch on Cortex-M)

**Recommendation: stay on the ESP32.**

| | ESP32 (WROOM-32) | Pico 1 WH (RP2040) | Pico 2 W (RP2350) |
|---|---|---|---|
| CPU | 2× Xtensa LX6 240 MHz, **FPU** | 2× Cortex-M0+ 133 MHz, **no FPU**, no DSP | 2× Cortex-M33 150 MHz, FPU + DSP |
| RAM | 520 KB | 264 KB | 520 KB |
| Wi-Fi + TLS to AWS IoT | mature (ESP-IDF esp-mqtt) | possible (CYW43 + lwIP + mbedTLS), tighter on RAM | possible |
| This project | everything works today | port drivers + network stack | port drivers + network stack |

- PyTorch's embedded runtime (**ExecuTorch**) does have an Arm Cortex-M
  backend, but it targets int8 neural networks using CMSIS-NN, and is
  optimised for cores with DSP/Helium extensions (M33/M55/M85, Ethos-U).
  The RP2040's Cortex-M0+ has neither DSP nor an FPU, so it is the weakest
  target for it.
- The model here is a ~8 KB tree ensemble that runs in microseconds; there
  is no neural network for PyTorch to run, and even a small CNN fits on the
  ESP32 via TFLite Micro + ESP-NN.
- Accuracy does not depend on the microcontroller. Switching would cost a
  driver + network-stack port and buy nothing for this problem.

If you have to use a Pico, prefer a **Pico 2 W**. `road_event.c`,
`nmea.c` and the generated model are plain C99 and would port unchanged;
only `adxl345.c`, `gps_neo7m.c` and the MQTT/Wi-Fi code are ESP-IDF
specific.
