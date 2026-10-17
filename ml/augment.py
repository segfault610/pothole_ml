"""
Grow a small set of real recordings into a balanced synthetic dataset.

    python ml/augment.py --src data/bench --out data/bench_aug --per-class 300

* Real events (from --src, via dataset.py) are re-used with random
  rotation (mounting angle), amplitude scaling, time stretch and fresh
  sensor noise.
* Background between events is real "quiet / sway" signal cut from the
  source's normal sessions, so the detector sees realistic streams.
* Classes with few or no real examples are filled with parametric events
  shaped like the real motion (pothole: drop -> sharp jolt -> ringing;
  normal: knocks, double knocks, shaking bursts), scaled to the magnitude
  of the recorded events.

The output has the same session format as collect.py (csv + .marks), so
train.py treats it like any other data. It is synthetic: the trained
firmware model is flagged MODEL_IS_SYNTHETIC.
"""

import argparse
import os

import numpy as np

import dataset
import road_event as re
from simulate import random_rotation

FS = re.FS_HZ
EVENTS_PER_SESSION = 30
GAP_S = (2.5, 4.0)          # quiet time between events (> mark tolerance)


# ------------------------------------------------------------ helpers

def unit(v):
    return v / np.linalg.norm(v)


def perpendicular(u, rng):
    r = rng.normal(size=3)
    r -= r @ u * u
    return unit(r)


def stretch(dyn, factor):
    """Time-stretch a (n, 3) signal around index PRE, keeping its length."""
    n = len(dyn)
    t = (np.arange(n) - re.PRE) / factor + re.PRE
    return np.stack([np.interp(t, np.arange(n), dyn[:, k], left=0, right=0) for k in range(3)], 1)


def hann(n):
    return np.hanning(max(int(n), 3))


def place(sig, start, pulse, direction):
    end = min(start + len(pulse), len(sig))
    sig[start:end] += np.outer(pulse[: end - start], direction)


# ------------------------------------------------------------ event makers

def augment_real(ev, rng):
    """Dynamic part (WIN, 3) of a real event, randomly transformed, in g."""
    g = ev["g"].astype(float)
    dyn = ev["window"].astype(float) - g
    dyn = stretch(dyn, rng.uniform(0.85, 1.2)) * rng.uniform(0.7, 1.35)
    dyn = np.roll(dyn, rng.integers(-5, 6), axis=0)
    return dyn, unit(g)


def make_pothole(u, scale, rng):
    """Drop, then a sharp jolt up, then ringing - along gravity u."""
    dyn = np.zeros((re.WIN, 3))
    t0 = re.PRE - rng.integers(0, 4)
    drop_len = rng.integers(8, 16)
    place(dyn, t0, -rng.uniform(0.4, 1.0) * scale * hann(drop_len), u)
    hit = t0 + drop_len - rng.integers(0, 3)
    jolt = rng.uniform(1.5, 3.5) * scale
    place(dyn, hit, jolt * hann(rng.integers(3, 6)), u)
    side = perpendicular(u, rng)
    place(dyn, hit, rng.uniform(0.15, 0.5) * jolt * hann(4), side)
    t = np.arange(re.WIN - hit) / FS
    ring = jolt * 0.3 * np.sin(2 * np.pi * rng.uniform(8, 15) * t) * np.exp(-t / rng.uniform(0.05, 0.15))
    place(dyn, hit + 3, ring, u)
    if rng.random() < 0.5:      # board comes back up to where it was
        place(dyn, hit + rng.integers(15, 30), rng.uniform(0.1, 0.3) * scale * hann(20), u)
    return dyn


def make_hump(u, scale, rng):
    """Smooth lift, then smoothly back down, small settle - along gravity u."""
    dyn = np.zeros((re.WIN, 3))
    t0 = re.PRE - rng.integers(2, 8)
    up = rng.integers(20, 45)
    a = rng.uniform(0.4, 1.2) * scale
    place(dyn, t0, a * hann(up), u)
    place(dyn, t0 + up, -rng.uniform(0.5, 0.9) * a * hann(rng.integers(20, 45)), u)
    place(dyn, t0 + 2 * up, 0.15 * a * hann(15), u)
    return dyn


def make_normal(u, scale, rng):
    """Knock, double knock, or a burst of shaking - no drop-first shape."""
    dyn = np.zeros((re.WIN, 3))
    kind = rng.choice(["knock", "double", "shake"])
    side = perpendicular(u, rng)
    t0 = re.PRE - rng.integers(0, 3)

    if kind in ("knock", "double"):
        for k in range(1 if kind == "knock" else 2):
            at = t0 + k * rng.integers(10, 25)
            a = rng.uniform(0.6, 2.5) * scale * rng.choice([1, 1, -1])
            place(dyn, at, a * hann(rng.integers(3, 5)), u)
            place(dyn, at, rng.uniform(0.2, 0.8) * abs(a) * hann(3), side)
            t = np.arange(25) / FS
            place(dyn, at + 2, 0.3 * a * np.sin(2 * np.pi * rng.uniform(10, 20) * t) * np.exp(-t / 0.05), u)
    else:
        n = rng.integers(40, 100)
        burst = rng.normal(0, rng.uniform(0.3, 0.9) * scale, (n, 3))
        burst *= np.hanning(n)[:, None]
        dyn[t0:t0 + n] += burst[: re.WIN - t0]
    return dyn


# ------------------------------------------------------------ sessions

def background_pool(src):
    """Dynamic signal (n, 3) of the source's normal sessions, minus events."""
    chunks = []
    for label, path in dataset.list_sessions([src]):
        if label != "normal":
            continue
        a = dataset.load_samples(path).astype(float)
        g = a[:200].mean(axis=0)
        dyn = a - g
        quiet = np.linalg.norm(dyn, axis=1) < 0.2
        chunks.append(dyn[quiet])
    if not chunks:
        return np.zeros((2000, 3))
    return np.concatenate(chunks)


def write_session(path, samples, marks):
    with open(path + ".csv", "w") as f:
        f.write("seq,x,y,z\n")
        for k, (x, y, z) in enumerate(samples):
            f.write(f"{k},{x:.4f},{y:.4f},{z:.4f}\n")
    with open(path + ".marks", "w") as f:
        f.write("".join(f"{m}\n" for m in marks))


def build_session(label, real, bg, scale, rng):
    rot = random_rotation(rng, 60)
    g = rot @ np.array([0.0, 0.0, rng.uniform(0.98, 1.06)])
    u = unit(g)
    parts, marks, pos = [], [], 0

    def background(n):
        i = rng.integers(0, max(len(bg) - n, 1))
        chunk = bg[i:i + n]
        if len(chunk) < n:
            chunk = np.resize(chunk, (n, 3))
        return chunk @ random_rotation(rng, 180).T * rng.uniform(0.5, 1.2)

    lead = background(int(FS * 3))
    parts.append(lead)
    pos += len(lead)

    for _ in range(EVENTS_PER_SESSION):
        use_real = real and (label != "pothole") and rng.random() < 0.7
        if use_real:
            dyn, u_src = augment_real(real[rng.integers(len(real))], rng)
            # rotate the recorded event so its gravity matches this session's
            v = np.cross(u_src, u)
            c = float(u_src @ u)
            k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
            align = np.eye(3) + k + k @ k / (1 + c) if c > -0.999 else -np.eye(3)
            dyn = dyn @ align.T
        else:
            maker = {"pothole": make_pothole, "speed_hump": make_hump, "normal": make_normal}[label]
            dyn = maker(u, scale, rng)

        dyn = dyn + background(re.WIN) * 0.5
        parts.append(dyn)
        marks.append(pos + re.PRE)
        pos += re.WIN

        gap = background(int(FS * rng.uniform(*GAP_S)))
        parts.append(gap)
        pos += len(gap)

    dyn_all = np.concatenate(parts)
    acc = g + dyn_all + rng.normal(0, 0.003, dyn_all.shape)
    return np.clip(np.round(acc / (1 / 4096)) * (1 / 4096), -8, 8), marks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default="data/bench")
    ap.add_argument("--out", default="data/bench_aug")
    ap.add_argument("--per-class", type=int, default=300)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    _, y, _, events = dataset.build([args.src], verbose=False)
    by_label = {c: [e for e in events if e["label"] == c] for c in dataset.LABELS}
    bg = background_pool(args.src)

    real_p2p = [re.features(e["window"], e["g"])[2] for e in events]
    scale = float(np.median(real_p2p)) / 2.5 if real_p2p else 1.0
    print("real events:", {c: len(v) for c, v in by_label.items()}, f"| magnitude scale {scale:.2f}")

    for label in dataset.LABELS:
        folder = os.path.join(args.out, label)
        os.makedirs(folder, exist_ok=True)
        n_sessions = int(np.ceil(args.per_class / EVENTS_PER_SESSION))
        for i in range(n_sessions):
            acc, marks = build_session(label, by_label[label], bg, scale, rng)
            write_session(os.path.join(folder, f"aug_{label}_{i:02d}"), acc, marks)
        print(f"  {label:10s} {n_sessions} sessions x {EVENTS_PER_SESSION} events "
              f"({'real-based' if by_label[label] and label != 'pothole' else 'parametric'})")


if __name__ == "__main__":
    main()
