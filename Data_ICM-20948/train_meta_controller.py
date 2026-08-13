"""
ARIA IMU Meta-Controller training script with session-aware validation.

This version:
- uses complete recording sessions as validation groups;
- automatically selects hidden size by leave-one-session-out (LOSO) accuracy;
- keeps a row-level 80/20 split only as a secondary sanity check;
- trains the final deployed model on all selected data;
- exports meta_controller_weights.h.

Important:
With only one Fast/still session, a true held-out Fast-session test is not
possible because holding it out would leave no Fast examples in training.
"""

import csv
import numpy as np
from collections import Counter, defaultdict

from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split
from sklearn.metrics import confusion_matrix

FILES = {
    "meta_log_still.csv":      0,  # Fast
    "meta_log_still_2.csv":    0,  # Fast
    "meta_log_gentle.csv":     1,  # Balanced
    "meta_log_gentle_2.csv":   1,  # Balanced
    #"meta_log_gentle_3.csv":   1,  # Balanced
    #"meta_log_strong_1.csv":   2,  # Accurate
    "meta_log_strong_2.csv":   2,  # Accurate
    #"meta_log_strong_3.csv":   2,  # Accurate
    "meta_log_strong_4.csv":   2,  # Accurate
    "meta_log_strong_5.csv":   2,  # Accurate
    "meta_log_strong_6.csv":   2,  # Accurate

}

FEATURE_COLS = [
    "confidence_avg",
    "confidence_trend",
    "signal_delta",
    "signal_rms",
]

HIDDEN_CANDIDATES = [8, 12, 16, 20, 24, 32]
ALPHA = 1e-3
RANDOM_STATE = 42

CLASS_NAMES = {
    0: "Fast",
    1: "Balanced",
    2: "Accurate",
}


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
                        f"Expected columns: {FEATURE_COLS}. "
                        f"Got: {reader.fieldnames}"
                    )

                rows.append(feat + [label])

    except FileNotFoundError:
        print(f"  [skip] {path} not found -- edit FILES if needed")

    return rows


def build_dataset():
    all_rows = []
    all_groups = []

    print("\n--- PER-SESSION FEATURE MEANS ---")

    for path, label in FILES.items():
        rows = load_labeled_csv(path, label)

        if not rows:
            continue

        arr = np.asarray(rows, dtype=np.float64)

        print(
            f"{path:28s} label={label} n={len(arr):3d}  "
            f"conf_avg={arr[:,0].mean():.3f}  "
            f"signal_rms={arr[:,3].mean():.3f}"
        )

        all_rows.extend(rows)
        all_groups.extend([path] * len(rows))

    if not all_rows:
        raise SystemExit("No data loaded. Check FILES.")

    data = np.asarray(all_rows, dtype=np.float64)
    X = data[:, :4]
    y = data[:, 4].astype(int)
    groups = np.asarray(all_groups, dtype=object)

    print("\n--- LOADED SESSIONS ---")
    for session in np.unique(groups):
        mask = groups == session
        label = int(np.unique(y[mask])[0])
        print(f"{session:28s}: {mask.sum():3d} rows -> {CLASS_NAMES[label]}")

    counts = Counter(y)
    print(
        "\nClass balance: "
        f"Fast={counts.get(0,0)}, "
        f"Balanced={counts.get(1,0)}, "
        f"Accurate={counts.get(2,0)}"
    )

    return X, y, groups


def make_model(hidden):
    return MLPClassifier(
        hidden_layer_sizes=(hidden,),
        activation="relu",
        solver="adam",
        max_iter=5000,
        random_state=RANDOM_STATE,
        alpha=ALPHA,
    )


def get_session_info(y, groups):
    session_labels = {}
    class_sessions = defaultdict(list)

    for session in np.unique(groups):
        mask = groups == session
        labels_here = np.unique(y[mask])

        if len(labels_here) != 1:
            raise RuntimeError(
                f"Session {session} contains multiple labels: {labels_here}"
            )

        label = int(labels_here[0])
        session_labels[session] = label
        class_sessions[label].append(session)

    eligible = []
    skipped = []

    for session, label in session_labels.items():
        if len(class_sessions[label]) >= 2:
            eligible.append(session)
        else:
            skipped.append(
                (
                    session,
                    label,
                    "only session for this class; holdout would remove "
                    "the class from training",
                )
            )

    return eligible, skipped, session_labels


def run_one_holdout(X, y, groups, hidden, held_session):
    val_mask = groups == held_session
    train_mask = ~val_mask

    scaler = StandardScaler()
    X_train_s = scaler.fit_transform(X[train_mask])
    X_val_s = scaler.transform(X[val_mask])

    clf = make_model(hidden)
    clf.fit(X_train_s, y[train_mask])

    pred = clf.predict(X_val_s)
    truth = y[val_mask]

    return truth, pred


def evaluate_hidden_loso(X, y, groups, hidden, eligible_sessions):
    session_acc = []
    pooled_true = []
    pooled_pred = []

    for session in eligible_sessions:
        truth, pred = run_one_holdout(
            X, y, groups, hidden, session
        )

        session_acc.append(float(np.mean(pred == truth)))
        pooled_true.extend(truth.tolist())
        pooled_pred.extend(pred.tolist())

    pooled_true = np.asarray(pooled_true, dtype=int)
    pooled_pred = np.asarray(pooled_pred, dtype=int)

    return {
        "mean_session_acc": float(np.mean(session_acc)),
        "std_session_acc": float(np.std(session_acc)),
        "pooled_acc": float(np.mean(pooled_true == pooled_pred)),
        "y_true": pooled_true,
        "y_pred": pooled_pred,
    }


def print_confusion_and_class_accuracy(y_true, y_pred, title_prefix):
    cm = confusion_matrix(y_true, y_pred, labels=[0, 1, 2])

    print(f"\n--- {title_prefix} CONFUSION MATRIX ---")
    print("             Fast  Bal   Acc")

    for i, name in enumerate(["Fast", "Bal ", "Acc "]):
        print(f"{name:5s}  {cm[i]}")

    print(f"\n--- {title_prefix} PER-CLASS ACCURACY ---")

    for c in [0, 1, 2]:
        mask = y_true == c

        if mask.sum() == 0:
            print(
                f"  {CLASS_NAMES[c]:9s}: N/A "
                f"(no valid held-out session)"
            )
        else:
            acc = float(np.mean(y_pred[mask] == c))
            print(
                f"  {CLASS_NAMES[c]:9s}: "
                f"{acc:.3f} ({mask.sum()} rows)"
            )


def print_feature_analysis(X, y, groups):
    """
    Print per-session and per-class feature statistics, plus simple
    class-overlap diagnostics for each feature.
    """
    feature_names = FEATURE_COLS

    print("\n--- PER-SESSION FEATURE STATISTICS ---")

    for session in np.unique(groups):
        mask = groups == session
        label = int(np.unique(y[mask])[0])
        arr = X[mask]

        print(
            f"\n{session}  class={CLASS_NAMES[label]}  n={arr.shape[0]}"
        )

        for i, name in enumerate(feature_names):
            vals = arr[:, i]
            print(
                f"  {name:18s} "
                f"mean={vals.mean(): .6f}  "
                f"std={vals.std(): .6f}  "
                f"min={vals.min(): .6f}  "
                f"max={vals.max(): .6f}"
            )

    print("\n--- PER-CLASS FEATURE STATISTICS ---")

    class_stats = {}

    for c in [0, 1, 2]:
        mask = y == c
        arr = X[mask]
        class_stats[c] = {}

        print(
            f"\n{CLASS_NAMES[c]}  n={arr.shape[0]}"
        )

        for i, name in enumerate(feature_names):
            vals = arr[:, i]

            stats = {
                "mean": float(vals.mean()),
                "std": float(vals.std()),
                "min": float(vals.min()),
                "max": float(vals.max()),
                "q10": float(np.percentile(vals, 10)),
                "q25": float(np.percentile(vals, 25)),
                "q50": float(np.percentile(vals, 50)),
                "q75": float(np.percentile(vals, 75)),
                "q90": float(np.percentile(vals, 90)),
            }

            class_stats[c][name] = stats

            print(
                f"  {name:18s} "
                f"mean={stats['mean']: .6f}  "
                f"std={stats['std']: .6f}  "
                f"q10={stats['q10']: .6f}  "
                f"q50={stats['q50']: .6f}  "
                f"q90={stats['q90']: .6f}"
            )

    print("\n--- FEATURE SEPARATION / OVERLAP ---")
    print(
        "Overlap uses each class's central 80% interval [q10, q90].\n"
        "0.000 means no overlap. Larger values mean more class collision."
    )

    pairs = [
        (0, 1),
        (1, 2),
        (0, 2),
    ]

    for name in feature_names:
        print(f"\n{name}")

        for a, b in pairs:
            sa = class_stats[a][name]
            sb = class_stats[b][name]

            lo = max(sa["q10"], sb["q10"])
            hi = min(sa["q90"], sb["q90"])

            overlap = max(0.0, hi - lo)

            span_lo = min(sa["q10"], sb["q10"])
            span_hi = max(sa["q90"], sb["q90"])
            total_span = span_hi - span_lo

            overlap_ratio = (
                overlap / total_span
                if total_span > 0.0
                else 0.0
            )

            mean_gap = abs(sa["mean"] - sb["mean"])

            pooled_std = np.sqrt(
                0.5 * (
                    sa["std"] * sa["std"] +
                    sb["std"] * sb["std"]
                )
            )

            effect_size = (
                mean_gap / pooled_std
                if pooled_std > 1e-12
                else 0.0
            )

            print(
                f"  {CLASS_NAMES[a]:9s} vs {CLASS_NAMES[b]:9s}: "
                f"overlap_ratio={overlap_ratio:.3f}  "
                f"mean_gap={mean_gap:.6f}  "
                f"effect_size={effect_size:.3f}"
            )

    print("\n--- SESSION-TO-CLASS DISTANCE (STANDARDIZED FEATURE SPACE) ---")
    print(
        "Each session mean is compared to each class mean after global "
        "standardization. Lower distance means more similar."
    )

    scaler = StandardScaler()
    Xs = scaler.fit_transform(X)

    class_centers = {}
    for c in [0, 1, 2]:
        class_centers[c] = Xs[y == c].mean(axis=0)

    for session in np.unique(groups):
        mask = groups == session
        label = int(np.unique(y[mask])[0])
        center = Xs[mask].mean(axis=0)

        distances = {
            c: float(np.linalg.norm(center - class_centers[c]))
            for c in [0, 1, 2]
        }

        nearest = min(distances, key=distances.get)

        print(
            f"{session:28s} "
            f"true={CLASS_NAMES[label]:9s}  "
            f"dFast={distances[0]:.3f}  "
            f"dBal={distances[1]:.3f}  "
            f"dAcc={distances[2]:.3f}  "
            f"nearest={CLASS_NAMES[nearest]}"
        )

    print("\n--- FEATURE CORRELATION MATRIX ---")
    corr = np.corrcoef(X, rowvar=False)

    print("                 " + "  ".join(f"{n[:8]:>8s}" for n in feature_names))
    for i, name in enumerate(feature_names):
        row = "  ".join(f"{corr[i,j]:8.3f}" for j in range(len(feature_names)))
        print(f"{name[:16]:16s} {row}")

def main():
    X, y, groups = build_dataset()

    print_feature_analysis(X, y, groups)

    eligible_sessions, skipped_sessions, session_labels =         get_session_info(y, groups)

    print("\n--- SESSION-WISE VALIDATION SETUP ---")

    for session in eligible_sessions:
        print(
            f"  HOLDOUT OK : {session:28s} "
            f"({CLASS_NAMES[session_labels[session]]})"
        )

    for session, label, reason in skipped_sessions:
        print(
            f"  SKIP       : {session:28s} "
            f"({CLASS_NAMES[label]}) -- {reason}"
        )

    if not eligible_sessions:
        raise SystemExit(
            "No valid session-wise holdouts are possible."
        )

    print("\n--- HIDDEN UNIT SWEEP (LEAVE-ONE-SESSION-OUT) ---")

    sweep = {}

    for hidden in HIDDEN_CANDIDATES:
        res = evaluate_hidden_loso(
            X, y, groups, hidden, eligible_sessions
        )
        sweep[hidden] = res

        print(
            f"hidden={hidden:2d}: "
            f"mean session acc={res['mean_session_acc']:.3f} "
            f"(+/- {res['std_session_acc']:.3f}), "
            f"pooled row acc={res['pooled_acc']:.3f}"
        )

    best_hidden = sorted(
        HIDDEN_CANDIDATES,
        key=lambda h: (-sweep[h]["mean_session_acc"], h)
    )[0]

    print(
        f"\nSELECTED HIDDEN_UNITS={best_hidden} "
        f"(best mean LOSO session accuracy)"
    )
        # ================================================================
    # FEATURE-SUBSET ABLATION
    # ================================================================

    print(
        f"\n--- FEATURE-SUBSET ABLATION "
        f"(LOSO, HIDDEN_UNITS={best_hidden}) ---"
    )

    FEATURE_SUBSETS = {
        "all_4": [
            0, 1, 2, 3
        ],
        "no_conf_trend": [
            0, 2, 3
        ],
        "signal_delta+rms": [
            2, 3
        ],
        "conf_avg+rms": [
            0, 3
        ],
        "conf_avg+signal_delta": [
            0, 2
        ],
    }

    feature_subset_results = {}

    for subset_name, feature_indices in FEATURE_SUBSETS.items():
        session_scores = []
        pooled_true = []
        pooled_pred = []

        X_sub = X[:, feature_indices]

        for held_session in eligible_sessions:
            val_mask = groups == held_session
            train_mask = ~val_mask

            X_train = X_sub[train_mask]
            y_train = y[train_mask]

            X_val = X_sub[val_mask]
            y_val = y[val_mask]

            scaler = StandardScaler()

            X_train_s = scaler.fit_transform(X_train)
            X_val_s = scaler.transform(X_val)

            clf = MLPClassifier(
                hidden_layer_sizes=(best_hidden,),
                activation="relu",
                solver="adam",
                max_iter=5000,
                random_state=RANDOM_STATE,
                alpha=ALPHA,
            )

            clf.fit(X_train_s, y_train)

            pred = clf.predict(X_val_s)

            acc = float(np.mean(pred == y_val))

            session_scores.append(acc)

            pooled_true.extend(y_val.tolist())
            pooled_pred.extend(pred.tolist())

        pooled_true = np.asarray(
            pooled_true,
            dtype=int
        )

        pooled_pred = np.asarray(
            pooled_pred,
            dtype=int
        )

        mean_session_acc = float(
            np.mean(session_scores)
        )

        std_session_acc = float(
            np.std(session_scores)
        )

        pooled_acc = float(
            np.mean(
                pooled_true == pooled_pred
            )
        )

        cm = confusion_matrix(
            pooled_true,
            pooled_pred,
            labels=[0, 1, 2],
        )

        class_acc = {}

        for c in [0, 1, 2]:
            mask = pooled_true == c

            if mask.sum() == 0:
                class_acc[c] = float("nan")
            else:
                class_acc[c] = float(
                    np.mean(
                        pooled_pred[mask] == c
                    )
                )

        feature_subset_results[subset_name] = {
            "mean_session_acc": mean_session_acc,
            "std_session_acc": std_session_acc,
            "pooled_acc": pooled_acc,
            "class_acc": class_acc,
            "confusion_matrix": cm,
        }

        feature_names_used = [
            FEATURE_COLS[i]
            for i in feature_indices
        ]

        print(
            f"\n{subset_name}"
        )

        print(
            "  features: "
            + ", ".join(feature_names_used)
        )

        print(
            f"  mean session acc = "
            f"{mean_session_acc:.3f} "
            f"(+/- {std_session_acc:.3f})"
        )

        print(
            f"  pooled row acc   = "
            f"{pooled_acc:.3f}"
        )

        print(
            f"  Fast acc         = "
            f"{class_acc[0]:.3f}"
            if not np.isnan(class_acc[0])
            else
            "  Fast acc         = N/A"
        )

        print(
            f"  Balanced acc     = "
            f"{class_acc[1]:.3f}"
        )

        print(
            f"  Accurate acc     = "
            f"{class_acc[2]:.3f}"
        )

    best_subset_name = max(
        feature_subset_results,
        key=lambda name:
            feature_subset_results[name][
                "mean_session_acc"
            ]
    )

    best_subset = feature_subset_results[
        best_subset_name
    ]

    print(
        "\n--- BEST FEATURE SUBSET ---"
    )

    print(
        f"subset             = "
        f"{best_subset_name}"
    )

    print(
        f"mean session acc   = "
        f"{best_subset['mean_session_acc']:.3f}"
    )

    print(
        f"pooled row acc     = "
        f"{best_subset['pooled_acc']:.3f}"
    )

    print(
        f"Balanced acc       = "
        f"{best_subset['class_acc'][1]:.3f}"
    )

    print(
        f"Accurate acc       = "
        f"{best_subset['class_acc'][2]:.3f}"
    )
    print(
        f"\n--- DETAILED LEAVE-ONE-SESSION-OUT RESULTS "
        f"(HIDDEN_UNITS={best_hidden}) ---"
    )

    pooled_true = []
    pooled_pred = []

    for session in eligible_sessions:
        truth, pred = run_one_holdout(
            X, y, groups, best_hidden, session
        )

        acc = float(np.mean(pred == truth))
        label = int(np.unique(truth)[0])

        print(
            f"{session:28s} "
            f"class={CLASS_NAMES[label]:9s} "
            f"n={len(truth):3d} "
            f"acc={acc:.3f}"
        )

        pooled_true.extend(truth.tolist())
        pooled_pred.extend(pred.tolist())

    pooled_true = np.asarray(pooled_true, dtype=int)
    pooled_pred = np.asarray(pooled_pred, dtype=int)

    print_confusion_and_class_accuracy(
        pooled_true,
        pooled_pred,
        "POOLED SESSION-HOLDOUT",
    )

    print("\n--- SECONDARY ROW-LEVEL 80/20 SANITY CHECK ---")
    print(
        "(Do NOT use this as the main generalization estimate; "
        "rows from the same session are correlated.)"
    )

    X_train, X_val, y_train, y_val = train_test_split(
        X,
        y,
        test_size=0.2,
        stratify=y,
        random_state=RANDOM_STATE,
    )

    scaler_check = StandardScaler()
    X_train_s = scaler_check.fit_transform(X_train)
    X_val_s = scaler_check.transform(X_val)

    print(
        f"\n--- SCALER SANITY CHECK "
        f"(HIDDEN_UNITS={best_hidden}) ---"
    )

    for name, mean, scale in zip(
        FEATURE_COLS,
        scaler_check.mean_,
        scaler_check.scale_,
    ):
        flag = (
            "  <-- WARNING: near-zero variance!"
            if scale < 0.01
            else ""
        )

        print(
            f"  {name:18s} "
            f"mean={mean:12.6f} "
            f"scale={scale:12.6f}"
            f"{flag}"
        )

    clf_check = make_model(best_hidden)
    clf_check.fit(X_train_s, y_train)

    print(
        f"\nRow-level train acc: "
        f"{clf_check.score(X_train_s, y_train):.3f}"
    )
    print(
        f"Row-level val acc:   "
        f"{clf_check.score(X_val_s, y_val):.3f}"
    )

    row_pred = clf_check.predict(X_val_s)

    print_confusion_and_class_accuracy(
        y_val,
        row_pred,
        "ROW-LEVEL",
    )

    print(
        f"\n--- TRAINING FINAL DEPLOYED MODEL ON ALL "
        f"{len(y)} ROWS ---"
    )
    print(f"HIDDEN_UNITS={best_hidden}")

    scaler_final = StandardScaler()
    X_all_s = scaler_final.fit_transform(X)

    clf_final = make_model(best_hidden)
    clf_final.fit(X_all_s, y)

    final_train_acc = clf_final.score(X_all_s, y)

    print(
        f"Final model accuracy on full training set: "
        f"{final_train_acc:.3f}"
    )
    print(
        "(This is NOT a generalization estimate -- "
        "use the session-wise results above.)"
    )

    W1 = clf_final.coefs_[0]
    B1 = clf_final.intercepts_[0]
    W2 = clf_final.coefs_[1]
    B2 = clf_final.intercepts_[1]

    def fmt_array(arr):
        return ", ".join(f"{v:.8f}f" for v in arr)

    def fmt_matrix(mat):
        return ", ".join(
            "{ " + fmt_array(row) + " }"
            for row in mat
        )

    header = f"""/* Auto-generated by train_meta_controller_session_cv.py
 *
 * Trained on {len(y)} labeled IMU meta-feature rows.
 * Final model fit on the FULL selected dataset.
 *
 * Hidden size selected by leave-one-session-out validation:
 *   HIDDEN_UNITS = {best_hidden}
 *
 * Architecture:
 *   4 inputs -> {best_hidden} hidden (ReLU) -> 3 outputs
 *
 * Labels:
 *   0 = VARIANT_FAST
 *   1 = VARIANT_BALANCED
 *   2 = VARIANT_ACCURATE
 *
 * Features:
 *   confidence_avg, confidence_trend, signal_delta, signal_rms
 *
 * latency_us intentionally excluded.
 * Full-data train accuracy: {final_train_acc:.3f}
 */

#ifndef META_CONTROLLER_WEIGHTS_H
#define META_CONTROLLER_WEIGHTS_H

#define META_HIDDEN_UNITS ({best_hidden})

static const float META_SCALER_MEAN[4] = {{ {fmt_array(scaler_final.mean_)} }};
static const float META_SCALER_SCALE[4] = {{ {fmt_array(scaler_final.scale_)} }};

static const float META_W1[4][META_HIDDEN_UNITS] = {{ {fmt_matrix(W1)} }};
static const float META_B1[META_HIDDEN_UNITS] = {{ {fmt_array(B1)} }};
static const float META_W2[META_HIDDEN_UNITS][3] = {{ {fmt_matrix(W2)} }};
static const float META_B2[3] = {{ {fmt_array(B2)} }};

static inline int meta_controller_predict(
    float confidence_avg,
    float confidence_trend,
    float signal_delta,
    float signal_rms)
{{
    float x[4] = {{
        confidence_avg,
        confidence_trend,
        signal_delta,
        signal_rms
    }};

    float xs[4];

    for (int i = 0; i < 4; i++)
    {{
        xs[i] =
            (x[i] - META_SCALER_MEAN[i]) /
            META_SCALER_SCALE[i];
    }}

    float h[META_HIDDEN_UNITS];

    for (int j = 0; j < META_HIDDEN_UNITS; j++)
    {{
        float sum = META_B1[j];

        for (int i = 0; i < 4; i++)
        {{
            sum += xs[i] * META_W1[i][j];
        }}

        h[j] = (sum > 0.0f) ? sum : 0.0f;
    }}

    float out[3];

    for (int k = 0; k < 3; k++)
    {{
        float sum = META_B2[k];

        for (int j = 0; j < META_HIDDEN_UNITS; j++)
        {{
            sum += h[j] * META_W2[j][k];
        }}

        out[k] = sum;
    }}

    int best = 0;

    for (int k = 1; k < 3; k++)
    {{
        if (out[k] > out[best])
        {{
            best = k;
        }}
    }}

    return best;
}}

#endif /* META_CONTROLLER_WEIGHTS_H */
"""

    with open("meta_controller_weights.h", "w") as f:
        f.write(header)

    print("\nWrote meta_controller_weights.h")
    print(
        f"Selected HIDDEN_UNITS={best_hidden} "
        f"from session-wise validation."
    )

    if any(label == 0 for _, label, _ in skipped_sessions):
        print(
            "\nIMPORTANT: Fast/still session generalization is NOT "
            "measured because only one still session exists."
        )
        print(
            "Collect meta_log_still_2.csv later for a true held-out "
            "Fast-session test."
        )


if __name__ == "__main__":
    main()
