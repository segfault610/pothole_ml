"""
Record a labelled session from esp32_collector over USB serial.

GUIDED (recommended): the script tells you what to do and when, shows a
live meter, checks every repetition and repeats the ones that missed.
Accepted repetitions are labelled automatically - no counting, no buttons.

    python ml/collect.py --label pothole --guided            # 20 good reps
    python ml/collect.py --label speed_hump --guided --reps 30
    python ml/collect.py --label normal --guided
    python ml/collect.py --label pothole --guided --out data/bench   # hand-motion practice

FREE (for real drives): records continuously until Ctrl+C; a passenger
presses ENTER (or the ESP32 BOOT button) after each pothole / hump.

    python ml/collect.py --label pothole
    python ml/collect.py --label normal --minutes 10

Output (<out>/<label>/<YYYYmmdd_HHMMSS>.*):
    .csv       seq,x,y,z           every accelerometer sample (g, 100 Hz)
    .marks     sample index of each confirmed event (guided) / key press (free)
    .gps.csv   seq,fix,lat,lon,speed_kmh,sats,hdop   (1 Hz)
"""

import argparse
import datetime
import os
import queue
import random
import sys
import threading
import time

import numpy as np
import serial

import road_event as re

LABELS = ["normal", "speed_hump", "pothole"]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- terminal

TTY = sys.stdout.isatty()


def c(text, code):
    return f"\033[{code}m{text}\033[0m" if TTY else text


def bold(t): return c(t, "1")
def green(t): return c(t, "1;32")
def red(t): return c(t, "1;31")
def yellow(t): return c(t, "1;33")
def cyan(t): return c(t, "1;36")
def dim(t): return c(t, "2")


def bell():
    if TTY:
        sys.stdout.write("\a")
        sys.stdout.flush()


def meter(v, full=1.5, width=30):
    """Centred bar: left = down, right = up."""
    half = width // 2
    n = int(min(abs(v) / full, 1.0) * half)
    left = " " * half
    right = " " * half
    if v < 0:
        left = " " * (half - n) + "#" * n
    else:
        right = "#" * n + " " * (half - n)
    return f"down [{left}|{right}] up  {v:+.2f} g"


# ---------------------------------------------------------------- recorder

class Recorder(threading.Thread):
    """
    Reads the serial stream in the background: writes every sample to
    disk, runs the firmware's own detector on it, and reports events.
    """

    def __init__(self, ser, base):
        super().__init__(daemon=True)
        self.ser = ser
        self.base = base
        self.det = re.Detector()
        self.events = queue.Queue()
        self.button = queue.Queue()
        self.seq = 0                # ESP32 sample counter (keeps counting since boot)
        self.samples = 0            # rows written = index used by the detector/.marks
        self.first_seq = None
        self.lost = 0
        self.vertical = 0.0         # latest vertical dyn. accel (for the meter)
        self.peak = 0.0             # largest |vertical| since reset_peak()
        self.gps = "no GPS data yet"
        self.warnings = []
        self.stop = threading.Event()
        self._expected = None
        self._g = None

    def row_of(self, device_seq):
        """File row (= detector index) of an ESP32 sample counter value."""
        if self.first_seq is None:
            return 0
        return max(device_seq - self.first_seq - self.lost, 0)

    def reset_peak(self):
        self.peak = 0.0

    def run(self):
        with open(self.base + ".csv", "w") as fd, open(self.base + ".gps.csv", "w") as fg:
            fd.write("seq,x,y,z\n")
            fg.write("seq,fix,lat,lon,speed_kmh,sats,hdop\n")

            while not self.stop.is_set():
                try:
                    line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                except serial.SerialException as e:
                    self.warnings.append(f"serial error: {e}")
                    return

                if line.startswith("D,"):
                    self._sample(line, fd)
                elif line.startswith("G,"):
                    self._gps(line, fg)
                elif line.startswith("M,"):
                    try:
                        self.button.put(self.row_of(int(line.split(",")[1])))
                    except (IndexError, ValueError):
                        pass
                elif line.startswith("W,"):
                    self.warnings.append(line[2:])

    def _sample(self, line, fd):
        p = line.split(",")
        if len(p) != 5:
            return
        try:
            seq = int(p[1])
            x, y, z = float(p[2]), float(p[3]), float(p[4])
        except ValueError:
            return

        if self._expected is not None and seq != self._expected:
            self.lost += max(seq - self._expected, 0)
        self._expected = seq + 1
        if self.first_seq is None:
            self.first_seq = seq
        self.seq = seq
        self.samples += 1
        fd.write(f"{seq},{p[2]},{p[3]},{p[4]}\n")

        # live meter: same vertical projection idea as the detector
        a = np.array([x, y, z])
        self._g = a if self._g is None else self._g + 0.01 * (a - self._g)
        gm = np.linalg.norm(self._g)
        self.vertical = float(a @ self._g / gm - gm) if gm > 0 else 0.0
        self.peak = max(self.peak, abs(self.vertical))

        r = self.det.push(x, y, z)
        if r == re.WINDOW_READY:
            window, g, trig = self.det.window()
            f = dict(zip(re.FEATURE_NAMES, re.features(window, g)))
            self.events.put({"trigger": trig, "features": f, "severity": re.severity(window, g)})

    def _gps(self, line, fg):
        p = line.split(",")
        if len(p) == 7 and p[1] != "nodata":
            fg.write(f"{self.seq},{p[1]},{p[2]},{p[3]},{p[4]},{p[5]},{p[6]}\n")
            self.gps = (f"GPS fix {float(p[2]):.5f},{float(p[3]):.5f} sats={p[5]}"
                        if p[1] == "1" else f"GPS searching (sats={p[5]})")
        else:
            self.gps = "GPS: no data"


# ---------------------------------------------------------------- guided mode

MOVES = {
    "pothole": {
        "title": "POTHOLE",
        "do": "quick DROP of 2-3 cm, then a sharp JOLT UP (tap the board's edge on the table)",
        "expect": -1,       # first big excursion should be downwards
        "wrong": "that went UP first - for a pothole DROP first, then jolt up",
    },
    "speed_hump": {
        "title": "SPEED HUMP",
        "do": "smooth LIFT of 5-8 cm (~0.4 s), then smoothly back DOWN, small settle",
        "expect": +1,
        "wrong": "that went DOWN first - for a hump LIFT first, then come down",
    },
}

NORMAL_MOVES = [
    "a firm KNOCK of the board on the table (one thud)",
    "1 s of fast, strong SHAKING (rough patch)",
    "a quick sharp SIDE-TO-SIDE wiggle",
    "a double knock: thud-thud (expansion joint)",
]

ACTION_WINDOW_S = 3.0      # time allowed for the movement after "GO"
SETTLE_S = 1.5             # hold still between reps (lets gravity settle)


def countdown(rec, label):
    for n in ("3", "2", "1"):
        sys.stdout.write(f"\r   {bold(n)}...   {dim(rec.gps)}          ")
        sys.stdout.flush()
        time.sleep(0.6)
    bell()
    sys.stdout.write("\r" + " " * 70 + "\r")
    print(f"   {green('>>> GO! <<<')}  {label}")


def wait_for_event(rec, start_seq, timeout_s):
    """Live meter while waiting; returns the first event triggered after start_seq."""
    rec.reset_peak()
    deadline = time.time() + timeout_s
    found = None

    while time.time() < deadline or found is None and time.time() < deadline + 1.6:
        try:
            ev = rec.events.get(timeout=0.08)
            if ev["trigger"] >= start_seq - 20:
                found = ev
                break
        except queue.Empty:
            pass
        if time.time() < deadline:
            sys.stdout.write(f"\r   {meter(rec.vertical)}   ")
            sys.stdout.flush()

    sys.stdout.write("\r" + " " * 78 + "\r")
    return found


def drain(q):
    while True:
        try:
            q.get_nowait()
        except queue.Empty:
            return


def guided(rec, label, reps, marks):
    tries = 0
    move = MOVES.get(label)

    print(bold(f"\nGuided session: {label.upper()} - {reps} good repetitions"))
    if move:
        print(f"Each time you see GO: {cyan(move['do'])}.")
    else:
        print("Keep a gentle cruising sway the whole time (1-2 cm, 1-2x per second).")
        print("At each GO, do the road disturbance shown - firm, but NOT a pothole-like drop.")
    print("Between repetitions " + ("hold the board still" if move else "keep the gentle sway") +
          ", always at the same angle.")
    print(dim("Ctrl+C stops early; everything recorded so far is kept.\n"))

    input(bold("Hold the board still and press ENTER to start... "))
    print(dim("   settling..."))
    while rec.samples < re.WIN + 60:          # detector warm-up
        time.sleep(0.1)
    time.sleep(SETTLE_S)

    while len(marks) < reps:
        tries += 1
        what = move["do"] if move else random.choice(NORMAL_MOVES)
        print(f"\n{bold(f'[{len(marks) + 1}/{reps}]')}  {what}")

        drain(rec.events)
        countdown(rec, what if not move else move["title"])
        start = rec.samples          # same units as the detector's trigger index
        ev = wait_for_event(rec, start, ACTION_WINDOW_S)

        if ev is None:
            bell()
            print(f"   {red('MISSED')} - peak only {rec.peak:.2f} g (needs > {re.TRIGGER_G:.2f} g). "
                  f"Do it {bold('harder / sharper')}. Repeating.")
        else:
            f = ev["features"]
            fp = f["first_peak"]
            shape = "drop first" if fp < 0 else "lift first"
            info = f"peak-to-peak {f['v_p2p']:.2f} g, {shape}"

            if move and np.sign(fp) != move["expect"]:
                print(f"   {yellow('WRONG SHAPE')} ({info}) - {move['wrong']}. Repeating.")
            else:
                marks.append(ev["trigger"])
                print(f"   {green('GOOD')} ({info})   {len(marks)}/{reps} done")

        if rec.warnings:
            print("   ESP32:", "; ".join(rec.warnings))
            rec.warnings.clear()

        sys.stdout.write(dim("   hold still..." if move else "   keep swaying gently..."))
        sys.stdout.flush()
        time.sleep(SETTLE_S)
        sys.stdout.write("\r" + " " * 30 + "\r")

    print(green(f"\nDone: {len(marks)} good repetitions in {tries} tries."))


# ---------------------------------------------------------------- free mode

def free(rec, label, minutes, marks):

    def keyboard():
        while not rec.stop.is_set():
            try:
                line = sys.stdin.readline()
            except (EOFError, ValueError):
                return
            if rec.stop.is_set() or line == "":
                return
            marks.append(rec.samples)
            print(f"  >>> MARK #{len(marks)} (ENTER) at sample {rec.samples}", flush=True)

    threading.Thread(target=keyboard, daemon=True).start()

    if label != "normal":
        print(f"Press ENTER (or the ESP32's BOOT button) every time you go over a {label.replace('_', ' ')}.")
    print("Ctrl+C to stop.\n")

    t0 = time.time()
    n_events = 0
    last_status = 0

    while not (minutes and time.time() - t0 > minutes * 60):
        try:
            ev = rec.events.get(timeout=0.2)
            n_events += 1
            print(f"  event #{n_events} at sample {ev['trigger']}: "
                  f"peak-to-peak {ev['features']['v_p2p']:.2f} g", flush=True)
        except queue.Empty:
            pass

        while not rec.button.empty():
            marks.append(rec.button.get())
            print(f"  >>> MARK #{len(marks)} (BOOT button) at sample {marks[-1]}", flush=True)

        if time.time() - last_status > 5:
            last_status = time.time()
            print(dim(f"  {rec.samples / 100:6.0f} s  events={n_events}  marks={len(marks)}  "
                      f"lost={rec.lost}  | {rec.gps}"), flush=True)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, choices=LABELS)
    ap.add_argument("--guided", action="store_true", help="step-by-step prompts with live checks")
    ap.add_argument("--reps", type=int, default=20, help="guided: good repetitions to collect")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--minutes", type=float, default=0, help="free: stop automatically (0 = Ctrl+C)")
    ap.add_argument("--out", default=os.path.join(ROOT, "data", "raw"))
    args = ap.parse_args()

    folder = os.path.join(args.out, args.label)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: could not open {args.port}: {e}")
        print("Is the ESP32 plugged in, flashed with esp32_collector, and no other monitor open?")
        raise SystemExit(1)

    print(f"Connected to {args.port}. Waiting for the sensor stream...")

    # the ESP32 resets when the port opens; wait for the stream
    deadline = time.time() + 20
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if line.startswith("# accelerometer:"):
            print("Sensor:", line.split(":", 1)[1].strip())
        if line == "DATA_BEGIN" or line.startswith("D,"):
            break
        if line.startswith("W,"):
            print("ESP32:", line[2:])
    else:
        print("No data from the ESP32. Is esp32_collector flashed and the sensor wired? (docs/01-hardware.md)")
        raise SystemExit(1)

    rec = Recorder(ser, base)
    rec.start()
    print(f"Recording to {os.path.relpath(base, ROOT)}.csv")

    marks = []      # filled in place, so Ctrl+C keeps what was collected
    try:
        if args.guided:
            guided(rec, args.label, args.reps, marks)
        else:
            free(rec, args.label, args.minutes, marks)
    except KeyboardInterrupt:
        print("\nStopped early - keeping everything recorded so far.")

    rec.stop.set()
    rec.join(timeout=2)
    ser.close()

    with open(base + ".marks", "w") as fm:
        fm.write("".join(f"{m}\n" for m in marks))

    print(f"\nSaved {rec.samples} samples ({rec.samples / 100:.0f} s), {len(marks)} marks, {rec.lost} lost samples")
    print(f"  {os.path.relpath(base, ROOT)}.csv / .marks / .gps.csv")
    print("Next: another session, or  python ml/train.py" +
          (f" --data {os.path.relpath(args.out, ROOT)}" if os.path.abspath(args.out) != os.path.join(ROOT, "data", "raw") else ""))


if __name__ == "__main__":
    main()
