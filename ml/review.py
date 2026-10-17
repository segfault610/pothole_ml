"""
Review (and fix) the labels of the events in a recorded session.

    python ml/review.py data/raw/pothole/20261017_101500.csv
    python ml/review.py data/raw/pothole/20261017_101500.csv --plot

For every detected event it shows the label dataset.py would give it,
the vertical-acceleration trace (ASCII, or a matplotlib window with
--plot) and asks:

    ENTER keep   n normal   h speed_hump   p pothole   d discard   q save+quit

Answers are stored in <session>.labels.json and override everything else.
"""

import argparse
import json
import os

import numpy as np

import dataset
import road_event as re

BARS = " .:-=+*#%@"


def vertical(window, g):
    gm = np.linalg.norm(g)
    return (window - g) @ (g / gm)


def ascii_trace(v, width=75):
    idx = np.linspace(0, len(v) - 1, width).astype(int)
    vv = v[idx]
    top = "".join(BARS[min(int(max(x, 0) / 1.5 * 9), 9)] for x in vv)
    bot = "".join(BARS[min(int(max(-x, 0) / 1.5 * 9), 9)] for x in vv)
    trig = " " * int(re.PRE / len(v) * width) + "^ trigger"
    return f"  up   |{top}|\n  down |{bot}|\n        {trig}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--plot", action="store_true", help="show a matplotlib plot per event")
    args = ap.parse_args()

    label = os.path.basename(os.path.dirname(os.path.abspath(args.session)))
    if label not in dataset.LABELS:
        raise SystemExit(f"session must be inside a {dataset.LABELS} folder")

    events = dataset.session_events(label, args.session)
    labels_path = args.session[:-4] + ".labels.json"
    overrides = {}
    if os.path.exists(labels_path):
        with open(labels_path) as f:
            overrides = {int(k): v for k, v in json.load(f).items()}

    keys = {"n": "normal", "h": "speed_hump", "p": "pothole", "d": "discard"}

    if args.plot:
        import matplotlib.pyplot as plt

    print(f"{len(events)} events in {args.session}\n")

    for i, ev in enumerate(events):
        v = vertical(ev["window"], ev["g"])
        current = ev["label"] or "(unmarked -> ignored)"
        print(f"[{i + 1}/{len(events)}] sample {ev['trigger']}  t={ev['trigger'] / re.FS_HZ:.1f} s  "
              f"label: {current}  peak +{v.max():.2f} / {v.min():.2f} g")
        print(ascii_trace(v))

        if args.plot:
            t = (np.arange(len(v)) - re.PRE) / re.FS_HZ
            plt.figure(figsize=(8, 3))
            plt.plot(t, v)
            plt.axvline(0, color="k", ls="--")
            plt.title(f"event {i + 1}: {current}")
            plt.xlabel("s from trigger")
            plt.ylabel("vertical accel (g)")
            plt.tight_layout()
            plt.show(block=False)

        answer = input("  ENTER keep / n / h / p / d / q > ").strip().lower()

        if args.plot:
            plt.close("all")

        if answer == "q":
            break
        if answer in keys:
            overrides[ev["trigger"]] = keys[answer]
        elif ev["label"] is not None:
            overrides[ev["trigger"]] = ev["label"]

    with open(labels_path, "w") as f:
        json.dump({str(k): v for k, v in sorted(overrides.items())}, f, indent=1)

    print(f"\nSaved {len(overrides)} labels to {labels_path}")


if __name__ == "__main__":
    main()
