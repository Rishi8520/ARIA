"""
ARIA Meta-Controller training script (v5 — configurable hidden size,
cross-validated model selection, final model trained on full dataset).

Workflow:
  1. Loads your 4 session CSVs with file-identity ground truth labels.
  2. Runs 5-fold CV to report an honest accuracy estimate (not a single
     noisy 80/20 split -- your validation set is small enough that one
     split can swing per-class accuracy by ~10 points).
  3. Trains the FINAL deployed model on ALL available data (CV was only
     for choosing HIDDEN_UNITS / sanity-checking -- once you trust the
     architecture, throwing away 20% of your already-small dataset for
     the deployed model is wasteful).
  4. Exports a C header whose forward-pass loops correctly match
     HIDDEN_UNITS (this was hardcoded to 8 in the earlier version --
     fixed here).

Set HIDDEN_UNITS below based on your cross-validation sweep results
BEFORE trusting the final exported header.
"""

import csv
import numpy as np
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import Pipeline
from sklearn.model_selection import StratifiedKFold, cross_val_score, train_test_split
from sklearn.metrics import confusion_matrix
from collections import Counter

FILES = {
    "meta_log_still.csv":    0,  # Fast
    "meta_log_gentle.csv":   1,  # Balanced
    "meta_log_strong_1.csv": 2,  # Accurate (session 1)
    "meta_log_strong_2.csv": 2,  # Accurate (session 2)
    "meta_log_strong_3.csv": 2,  # Accurate (session 3)
}

FEATURE_COLS = ["confidence_avg", "confidence_trend", "signal_delta", "signal_rms"]

# ---- Set this based on your CV sweep before final export ----
HIDDEN_UNITS = 16
ALPHA = 1e-3  # L2 regularization; raise this if larger hidden sizes overfit
RANDOM_STATE = 42


def load_labeled_csv(path, label):
    rows = []
    try:
        with open(path, "r", newline="") as f:
            reader = csv.DictReader(f)
            for r in reader:
                try:
                    feat = [float(r[c]) for c in FEATURE_COLS]
                except KeyError as e:
                    raise SystemExit(
                        f"Column {e} not found in {path}. "
                        f"Expected columns: {FEATURE_COLS}. Got: {reader.fieldnames}"
                    )
                rows.append(feat + [label])
    except FileNotFoundError:
        print(f"  [skip] {path} not found -- edit FILES dict if your filename differs")
    return rows

print("\\n--- PER-SESSION FEATURE MEANS (check consistency within same label) ---")
for path, label in FILES.items():
    rows = load_labeled_csv(path, label)
    if not rows:
        continue
    arr = np.array(rows)
    print(f"{path:28s} label={label}  n={len(arr):3d}  "
          f"conf_avg={arr[:,0].mean():.3f}  signal_rms={arr[:,3].mean():.3f}")
    
def main():
    all_rows = []
    for path, label in FILES.items():
        rows = load_labeled_csv(path, label)
        print(f"{path}: {len(rows)} rows -> label {label}")
        all_rows.extend(rows)

    if not all_rows:
        print("No data loaded. Check FILES dict matches your actual filenames.")
        return

    data = np.array(all_rows)
    X = data[:, :4]
    y = data[:, 4].astype(int)

    counts = Counter(y)
    print(f"\nClass balance: Fast={counts[0]}, Balanced={counts[1]}, Accurate={counts[2]}")

    # ---- Step 1: cross-validated sweep for model selection ----
    print("\n--- HIDDEN UNIT SWEEP (5-fold CV) ---")
    skf = StratifiedKFold(n_splits=5, shuffle=True, random_state=RANDOM_STATE)
    for hidden in [8, 12, 16, 20, 24, 32]:
        pipe_h = Pipeline([
            ("scaler", StandardScaler()),
            ("mlp", MLPClassifier(hidden_layer_sizes=(hidden,), activation="relu",
                                   max_iter=5000, random_state=RANDOM_STATE, alpha=ALPHA)),
        ])
        scores_h = cross_val_score(pipe_h, X, y, cv=skf, scoring="accuracy")
        marker = "  <-- HIDDEN_UNITS setting" if hidden == HIDDEN_UNITS else ""
        print(f"hidden={hidden:2d}: mean CV acc = {scores_h.mean():.3f} (+/- {scores_h.std():.3f}){marker}")

    # ---- Step 2: honest held-out check with chosen HIDDEN_UNITS ----
    X_train, X_val, y_train, y_val = train_test_split(
        X, y, test_size=0.2, stratify=y, random_state=RANDOM_STATE
    )
    scaler_check = StandardScaler()
    X_train_s = scaler_check.fit_transform(X_train)
    X_val_s = scaler_check.transform(X_val)

    print(f"\n--- SCALER SANITY CHECK (HIDDEN_UNITS={HIDDEN_UNITS}) ---")
    for name, mean, scale in zip(FEATURE_COLS, scaler_check.mean_, scaler_check.scale_):
        flag = "  <-- WARNING: near-zero variance!" if scale < 0.01 else ""
        print(f"  {name:18s} mean={mean:12.6f}  scale={scale:12.6f}{flag}")

    clf_check = MLPClassifier(hidden_layer_sizes=(HIDDEN_UNITS,), activation="relu",
                               solver="adam", max_iter=5000, random_state=RANDOM_STATE, alpha=ALPHA)
    clf_check.fit(X_train_s, y_train)
    print(f"\nHeld-out train acc: {clf_check.score(X_train_s, y_train):.3f}")
    print(f"Held-out val acc:   {clf_check.score(X_val_s, y_val):.3f}")

    y_pred = clf_check.predict(X_val_s)
    print("\n--- PER-CLASS VALIDATION ACCURACY ---")
    for c, name in zip([0, 1, 2], ["Fast", "Balanced", "Accurate"]):
        mask = y_val == c
        if mask.sum() == 0:
            continue
        print(f"  {name}: {(y_pred[mask]==c).mean():.3f} ({mask.sum()} val rows)")

    print("\n--- CONFUSION MATRIX (rows=true, cols=predicted) ---")
    print("        Fast  Bal   Acc")
    cm = confusion_matrix(y_val, y_pred, labels=[0, 1, 2])
    for i, name in enumerate(["Fast", "Bal ", "Acc "]):
        print(f"{name}  {cm[i]}")

    # ---- Step 3: train FINAL model on ALL data (not just 80% split) ----
    print(f"\n--- Training final deployed model on all {len(y)} rows (HIDDEN_UNITS={HIDDEN_UNITS}) ---")
    scaler_final = StandardScaler()
    X_all_s = scaler_final.fit_transform(X)
    clf_final = MLPClassifier(hidden_layer_sizes=(HIDDEN_UNITS,), activation="relu",
                               solver="adam", max_iter=5000, random_state=RANDOM_STATE, alpha=ALPHA)
    clf_final.fit(X_all_s, y)
    final_train_acc = clf_final.score(X_all_s, y)
    print(f"Final model accuracy on full training set: {final_train_acc:.3f}")
    print("(This is NOT a generalization estimate -- refer to the CV sweep above for that.)")

    W1 = clf_final.coefs_[0]       # (4, HIDDEN_UNITS)
    B1 = clf_final.intercepts_[0]  # (HIDDEN_UNITS,)
    W2 = clf_final.coefs_[1]       # (HIDDEN_UNITS, 3)
    B2 = clf_final.intercepts_[1]  # (3,)

    def fmt_array(arr):
        return ", ".join(f"{v:.8f}f" for v in arr)

    def fmt_matrix(mat):
        return ", ".join("{ " + fmt_array(row) + " }" for row in mat)

    header = f"""/* Auto-generated by train_meta_controller.py (v5) -- do not hand-edit.
 * Trained on {len(y)} labeled runtime feature rows (file-identity ground truth),
 * fit on the FULL dataset (cross-validation was used only for model selection).
 * Architecture: 4 inputs -> {HIDDEN_UNITS} hidden (ReLU) -> 3 outputs (argmax = variant)
 * Labels: 0 = VARIANT_FAST, 1 = VARIANT_BALANCED, 2 = VARIANT_ACCURATE
 * Features: confidence_avg, confidence_trend, signal_delta, signal_rms
 *           (latency_us intentionally excluded -- near-zero variance, dominated old model)
 * Full-data train acc: {final_train_acc:.3f}
 * See cross-validation sweep in training script output for generalization estimate.
 */
#ifndef META_CONTROLLER_WEIGHTS_H
#define META_CONTROLLER_WEIGHTS_H

#define META_HIDDEN_UNITS ({HIDDEN_UNITS})

static const float META_SCALER_MEAN[4] = {{ {fmt_array(scaler_final.mean_)} }};
static const float META_SCALER_SCALE[4] = {{ {fmt_array(scaler_final.scale_)} }};

static const float META_W1[4][META_HIDDEN_UNITS] = {{ {fmt_matrix(W1)} }};
static const float META_B1[META_HIDDEN_UNITS] = {{ {fmt_array(B1)} }};
static const float META_W2[META_HIDDEN_UNITS][3] = {{ {fmt_matrix(W2)} }};
static const float META_B2[3] = {{ {fmt_array(B2)} }};

static inline int meta_controller_predict(float confidence_avg, float confidence_trend,
                                           float signal_delta, float signal_rms)
{{
    float x[4] = {{ confidence_avg, confidence_trend, signal_delta, signal_rms }};
    float xs[4];
    for (int i = 0; i < 4; i++) {{
        xs[i] = (x[i] - META_SCALER_MEAN[i]) / META_SCALER_SCALE[i];
    }}

    float h[META_HIDDEN_UNITS];
    for (int j = 0; j < META_HIDDEN_UNITS; j++) {{
        float sum = META_B1[j];
        for (int i = 0; i < 4; i++) {{
            sum += xs[i] * META_W1[i][j];
        }}
        h[j] = (sum > 0.0f) ? sum : 0.0f;
    }}

    float out[3];
    for (int k = 0; k < 3; k++) {{
        float sum = META_B2[k];
        for (int j = 0; j < META_HIDDEN_UNITS; j++) {{
            sum += h[j] * META_W2[j][k];
        }}
        out[k] = sum;
    }}

    int best = 0;
    for (int k = 1; k < 3; k++) {{
        if (out[k] > out[best]) best = k;
    }}
    return best;
}}

#endif /* META_CONTROLLER_WEIGHTS_H */
"""

    with open("meta_controller_weights.h", "w") as f:
        f.write(header)

    print("\nWrote meta_controller_weights.h")
    print(f"HIDDEN_UNITS={HIDDEN_UNITS} -- the forward-pass loops now correctly use this value")
    print("(fixed vs. the earlier hardcoded-8 version).")
    print("\nRemember: signal_rms replaces latency_us as the 4th input.")
    print("You MUST update the call site in adc_thread_entry.c to pass")
    print("g_meta_features.signal_rms instead of g_meta_features.latency_us.")


if __name__ == "__main__":
    main()