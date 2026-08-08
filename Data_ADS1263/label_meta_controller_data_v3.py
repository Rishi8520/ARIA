"""
ARIA Meta-Controller: Data-driven labeling script (v3).

Fixes over v2:
  1. Filters out cold-start artifact rows (latency_us anomalously high,
     signal_delta/confidence_trend spiked because static state variables
     reset to zero on MCU reboot between sessions).
  2. Uses abs(signal_delta) for thresholding -- a big drop in signal energy
     is just as meaningful as a big rise; the old signed-value approach
     was funneling large negative swings into "Fast" incorrectly.

Usage:
    python3 label_meta_controller_data_v3.py

Expects steady_features.csv, disturbance_features.csv, transition_features.csv
in the same directory.

Output: meta_controller_training_data.csv
Labels: 0 = Fast, 1 = Balanced, 2 = Accurate
"""

import csv
import numpy as np

FILES = {
    "steady": "steady_features.csv",
    "disturbance": "disturbance_features.csv",
    "transition": "transition_features.csv",
}

LATENCY_ARTIFACT_THRESHOLD_US = 190.0  # cold-start rows showed ~196us vs normal ~185-186us


def load_csv(path):
    rows = []
    try:
        with open(path, "r", newline="") as f:
            reader = csv.DictReader(f)
            for r in reader:
                row = {
                    "confidence_avg": float(r["confidence_avg"]),
                    "confidence_trend": float(r["confidence_trend"]),
                    "signal_delta": float(r["signal_delta"]),
                    "latency_us": float(r["latency_us"]),
                }
                if row["latency_us"] > LATENCY_ARTIFACT_THRESHOLD_US:
                    continue  # skip cold-start artifact row
                rows.append(row)
    except FileNotFoundError:
        print(f"  [skip] {path} not found")
    return rows


def main():
    steady_rows = load_csv(FILES["steady"])
    disturbance_rows = load_csv(FILES["disturbance"])
    transition_rows = load_csv(FILES.get("transition", ""))

    print(f"steady: {len(steady_rows)} rows (after artifact filter)")
    print(f"disturbance: {len(disturbance_rows)} rows (after artifact filter)")
    print(f"transition: {len(transition_rows)} rows (after artifact filter)")

    if not steady_rows or not disturbance_rows:
        print("Need at least steady + disturbance rows. Aborting.")
        return

    steady_abs_delta = np.array([abs(r["signal_delta"]) for r in steady_rows])
    dist_abs_delta = np.array([abs(r["signal_delta"]) for r in disturbance_rows])
    steady_conf = np.array([r["confidence_avg"] for r in steady_rows])
    dist_conf = np.array([r["confidence_avg"] for r in disturbance_rows])

    delta_fast_thresh = (np.percentile(steady_abs_delta, 75) + np.percentile(dist_abs_delta, 25)) / 2
    delta_accurate_thresh = np.percentile(dist_abs_delta, 50)
    conf_fast_thresh = (np.percentile(steady_conf, 25) + np.percentile(dist_conf, 75)) / 2

    print("\nComputed thresholds (abs(signal_delta)-based):")
    print(f"  |signal_delta| -> Fast below {delta_fast_thresh:.6f}, Accurate at/above {delta_accurate_thresh:.6f}")
    print(f"  confidence_avg -> escalation trigger around {conf_fast_thresh:.6f}")

    def label_row(r):
        sd = abs(r["signal_delta"])
        ca = r["confidence_avg"]
        if sd >= delta_accurate_thresh or ca <= conf_fast_thresh * 0.5:
            return 2  # Accurate
        elif sd >= delta_fast_thresh or ca <= conf_fast_thresh:
            return 1  # Balanced
        else:
            return 0  # Fast

    all_rows = []
    for r in steady_rows + disturbance_rows + transition_rows:
        r["label"] = label_row(r)
        all_rows.append(r)

    out_path = "meta_controller_training_data.csv"
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["confidence_avg", "confidence_trend", "signal_delta", "latency_us", "label"]
        )
        writer.writeheader()
        writer.writerows(all_rows)

    label_counts = {0: 0, 1: 0, 2: 0}
    for r in all_rows:
        label_counts[r["label"]] += 1

    print(f"\nWrote {len(all_rows)} labeled rows to {out_path}")
    print(f"  Fast: {label_counts[0]}, Balanced: {label_counts[1]}, Accurate: {label_counts[2]}")


if __name__ == "__main__":
    main()
