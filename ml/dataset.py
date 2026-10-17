"""
Turn raw recording sessions into a labelled event dataset.

Layout (written by ml/collect.py, or ml/simulate.py):

    <data_dir>/<label>/<session>.csv          seq,x,y,z  (g, 100 Hz)
    <data_dir>/<label>/<session>.marks        optional: sample index of each
                                              moment you pressed ENTER
    <data_dir>/<label>/<session>.labels.json  optional: per-event overrides
                                              written by ml/review.py

Labelling rules
---------------
* The firmware's detector (same C code) finds the events.
* "normal" sessions: every detected event is "normal" (rough patches,
  joints, manholes - exactly the things the model must NOT call a pothole).
* "speed_hump" / "pothole" sessions:
    - with a .marks file: an event is kept only if a mark falls within
      MARK_BEFORE..MARK_AFTER seconds of its trigger; unmarked events are
      dropped (we do not know what they were).
    - without marks: every event gets the session label.
* .labels.json overrides everything ("discard" removes the event).
"""

import glob
import json
import os

import numpy as np
import pandas as pd

import road_event as re

LABELS = ["normal", "speed_hump", "pothole"]

MARK_BEFORE_S = 1.0     # mark may come this long before the trigger
MARK_AFTER_S = 2.0      # ...or this long after (human reaction time)


def list_sessions(data_dirs):
    sessions = []

    for data_dir in data_dirs:
        for label in LABELS:
            for path in sorted(glob.glob(os.path.join(data_dir, label, "*.csv"))):
                if path.endswith(".gps.csv"):
                    continue
                sessions.append((label, path))

    return sessions


def load_samples(path):
    df = pd.read_csv(path)
    return df[["x", "y", "z"]].to_numpy(dtype=np.float32)


def load_marks(path):
    marks_path = path[:-4] + ".marks"

    if not os.path.exists(marks_path):
        return None

    return np.loadtxt(marks_path, dtype=np.int64, ndmin=1)


def load_overrides(path):
    labels_path = path[:-4] + ".labels.json"

    if not os.path.exists(labels_path):
        return {}

    with open(labels_path) as f:
        return {int(k): v for k, v in json.load(f).items()}


def session_events(label, path):
    """Detected events of one session with their resolved labels."""
    samples = load_samples(path)
    marks = load_marks(path)
    overrides = load_overrides(path)

    out = []

    for ev in re.detect_events(samples):
        trig = ev["trigger"]
        ev_label = label

        if label != "normal" and marks is not None and len(marks) > 0:
            near = (marks >= trig - MARK_BEFORE_S * re.FS_HZ) & (marks <= trig + MARK_AFTER_S * re.FS_HZ)
            if not near.any():
                ev_label = None

        if trig in overrides:
            ev_label = None if overrides[trig] == "discard" else overrides[trig]

        ev["label"] = ev_label
        ev["session"] = path
        out.append(ev)

    return out


def build(data_dirs, verbose=True):
    """Returns X (n, F) float32, y (n,) str, groups (n,) session path, events list."""
    X, y, groups, events = [], [], [], []

    for label, path in list_sessions(data_dirs):
        evs = session_events(label, path)
        kept = [e for e in evs if e["label"] is not None]

        if verbose:
            print(f"  {label:10s} {os.path.basename(path):40s} events={len(evs):3d} kept={len(kept):3d}")

        for e in kept:
            X.append(re.features(e["window"], e["g"]))
            y.append(e["label"])
            groups.append(path)
            events.append(e)

    if not X:
        return np.zeros((0, re.NUM_FEATURES), np.float32), np.array([]), np.array([]), []

    return np.stack(X), np.array(y), np.array(groups), events
