import os

import numpy as np
import pandas as pd

from sklearn.tree import DecisionTreeClassifier
from sklearn.model_selection import train_test_split, StratifiedKFold, cross_val_score
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
)
from sklearn.tree import export_text


# ============================================================
# CONFIGURATION
# ============================================================

DATA_DIR = "data_old"
MODEL_DIR = "model"

WINDOW_SAMPLES = 100

RANDOM_STATE = 42

FEATURE_NAMES = [
    "x_mean",
    "x_std",
    "x_min",
    "x_max",

    "y_mean",
    "y_std",
    "y_min",
    "y_max",

    "z_mean",
    "z_std",
    "z_min",
    "z_max",

    "mag_mean",
    "mag_std",
    "mag_min",
    "mag_max",

    "mag_rms",
    "mag_range",
]

CLASS_NAMES = [
    "normal",
    "speed_hump",
    "pothole",
]


# ============================================================
# LOAD DATA
# ============================================================

def load_dataset():

    all_windows = []

    all_labels = []


    for class_name in CLASS_NAMES:

        filename = os.path.join(
            DATA_DIR,
            f"{class_name}.csv"
        )

        print(
            f"Loading {filename}"
        )


        df = pd.read_csv(
            filename
        )


        required_columns = [
            "class",
            "window",
            "sample",
            "timestamp",
            "x",
            "y",
            "z",
        ]


        for column in required_columns:

            if column not in df.columns:

                raise ValueError(
                    f"{filename} is missing "
                    f"column '{column}'"
                )


        windows = sorted(
            df["window"].unique()
        )


        print(
            f"  samples : {len(df)}"
        )

        print(
            f"  windows : {len(windows)}"
        )


        for window_id in windows:

            window_df = df[
                df["window"] == window_id
            ].sort_values(
                "sample"
            )


            if len(window_df) != WINDOW_SAMPLES:

                print(
                    f"WARNING: "
                    f"{class_name} window "
                    f"{window_id} contains "
                    f"{len(window_df)} samples"
                )

                continue


            x = window_df["x"].to_numpy(
                dtype=np.float64
            )

            y = window_df["y"].to_numpy(
                dtype=np.float64
            )

            z = window_df["z"].to_numpy(
                dtype=np.float64
            )


            features = extract_features(
                x,
                y,
                z
            )


            all_windows.append(
                features
            )

            all_labels.append(
                class_name
            )


    X = np.asarray(
        all_windows,
        dtype=np.float64
    )

    y = np.asarray(
        all_labels
    )


    return X, y


# ============================================================
# FEATURE EXTRACTION
#
# MUST MATCH ESP32 inference main.c
# ============================================================

def extract_features(x, y, z):

    magnitude = np.sqrt(
        x * x +
        y * y +
        z * z
    )


    features = [

        np.mean(x),
        np.std(x),
        np.min(x),
        np.max(x),

        np.mean(y),
        np.std(y),
        np.min(y),
        np.max(y),

        np.mean(z),
        np.std(z),
        np.min(z),
        np.max(z),

        np.mean(magnitude),
        np.std(magnitude),
        np.min(magnitude),
        np.max(magnitude),

        np.sqrt(
            np.mean(
                magnitude * magnitude
            )
        ),

        np.max(magnitude) -
        np.min(magnitude),
    ]


    return features


# ============================================================
# GENERATE C DECISION TREE
# ============================================================

def generate_c_header(
    model,
    filename
):

    tree = model.tree_


    left = tree.children_left
    right = tree.children_right

    feature = tree.feature
    threshold = tree.threshold


    def generate_node(node, depth):

        indent = "    " * depth


        if left[node] == right[node]:

            values = tree.value[node][0]

            class_index = int(
                np.argmax(values)
            )


            return (
                f"{indent}return "
                f"CLASS_{CLASS_NAMES[class_index].upper()};\n"
            )


        feature_index = feature[node]

        threshold_value = threshold[node]


        feature_name = FEATURE_NAMES[
            feature_index
        ]


        text = ""

        text += (
            f"{indent}/* {feature_name} */\n"
        )

        text += (
            f"{indent}if "
            f"(f[{feature_index}] <= "
            f"{threshold_value:.8f}f)\n"
        )

        text += (
            f"{indent}{{\n"
        )

        text += generate_node(
            left[node],
            depth + 1
        )

        text += (
            f"{indent}}}\n"
        )

        text += (
            f"{indent}else\n"
        )

        text += (
            f"{indent}{{\n"
        )

        text += generate_node(
            right[node],
            depth + 1
        )

        text += (
            f"{indent}}}\n"
        )


        return text


    code = generate_node(
        0,
        1
    )


    header = f"""/*
 * AUTO-GENERATED DECISION TREE
 *
 * Generated by train.py
 *
 * DO NOT EDIT MANUALLY.
 */

#ifndef POTHOLE_MODEL_H
#define POTHOLE_MODEL_H

#define CLASS_NORMAL       0
#define CLASS_SPEED_HUMP   1
#define CLASS_POTHOLE      2

static inline int pothole_predict(
    const float *f
)
{{
{code}}}

#endif
"""


    with open(
        filename,
        "w"
    ) as file:

        file.write(
            header
        )


# ============================================================
# MAIN
# ============================================================

def main():

    os.makedirs(
        MODEL_DIR,
        exist_ok=True
    )


    print()
    print("========================================")
    print("       POTHOLE ML TRAINING")
    print("========================================")
    print()


    # --------------------------------------------------------
    # Load dataset
    # --------------------------------------------------------

    X, y = load_dataset()


    print()
    print(
        f"Feature matrix: {X.shape}"
    )

    print(
        f"Labels        : {y.shape}"
    )

    print()


    for class_name in CLASS_NAMES:

        count = np.sum(
            y == class_name
        )

        print(
            f"{class_name:12s}: "
            f"{count} windows"
        )


    print()


    # --------------------------------------------------------
    # Train/test split
    # --------------------------------------------------------

    X_train, X_test, y_train, y_test = (
        train_test_split(
            X,
            y,
            test_size=0.25,
            random_state=RANDOM_STATE,
            stratify=y
        )
    )


    print(
        f"Training windows: "
        f"{len(X_train)}"
    )

    print(
        f"Testing windows : "
        f"{len(X_test)}"
    )


    # --------------------------------------------------------
    # Compare tree depths
    # --------------------------------------------------------

    print()
    print("========================================")
    print("       TREE DEPTH COMPARISON")
    print("========================================")
    print()


    depths = [
        2,
        3,
        4,
        5,
        6,
        None,
    ]


    cv = StratifiedKFold(
        n_splits=5,
        shuffle=True,
        random_state=RANDOM_STATE
    )


    results = []


    for depth in depths:

        clf = DecisionTreeClassifier(
            max_depth=depth,
            criterion="gini",
            random_state=RANDOM_STATE
        )


        scores = cross_val_score(
            clf,
            X,
            y,
            cv=cv,
            scoring="accuracy"
        )


        mean_score = np.mean(scores)

        std_score = np.std(scores)


        results.append(
            (
                depth,
                mean_score
            )
        )


        print(
            f"depth={str(depth):4s} : "
            f"{mean_score * 100:.2f}% "
            f"+/- "
            f"{std_score * 100:.2f}%"
        )


    # --------------------------------------------------------
    # Select best depth
    # --------------------------------------------------------

    best_depth, best_cv_score = max(
        results,
        key=lambda item: item[1]
    )


    print()
    print(
        f"Selected depth: {best_depth}"
    )

    print(
        f"Cross-validation accuracy: "
        f"{best_cv_score * 100:.2f}%"
    )


    # --------------------------------------------------------
    # Final model
    # --------------------------------------------------------

    model = DecisionTreeClassifier(
        max_depth=best_depth,
        criterion="gini",
        random_state=RANDOM_STATE
    )


    model.fit(
        X_train,
        y_train
    )


    # --------------------------------------------------------
    # Predictions
    # --------------------------------------------------------

    train_pred = model.predict(
        X_train
    )

    test_pred = model.predict(
        X_test
    )


    train_accuracy = accuracy_score(
        y_train,
        train_pred
    )

    test_accuracy = accuracy_score(
        y_test,
        test_pred
    )


    # --------------------------------------------------------
    # Results
    # --------------------------------------------------------

    print()
    print("========================================")
    print("       FINAL MODEL PERFORMANCE")
    print("========================================")
    print()


    print(
        f"Training accuracy       : "
        f"{train_accuracy * 100:.2f}%"
    )

    print(
        f"Test accuracy           : "
        f"{test_accuracy * 100:.2f}%"
    )

    print(
        f"5-fold CV accuracy      : "
        f"{best_cv_score * 100:.2f}%"
    )


    print()
    print("Classification report:")
    print()


    print(
        classification_report(
            y_test,
            test_pred,
            labels=CLASS_NAMES,
            zero_division=0
        )
    )


    print(
        "Confusion matrix:"
    )

    print(
        confusion_matrix(
            y_test,
            test_pred,
            labels=CLASS_NAMES
        )
    )


    # --------------------------------------------------------
    # Decision tree
    # --------------------------------------------------------

    print()
    print("========================================")
    print("       DECISION TREE")
    print("========================================")
    print()


    print(
        export_text(
            model,
            feature_names=FEATURE_NAMES
        )
    )


    # --------------------------------------------------------
    # Feature importance
    # --------------------------------------------------------

    print()
    print("========================================")
    print("       FEATURE IMPORTANCE")
    print("========================================")
    print()


    importance = model.feature_importances_


    sorted_indices = np.argsort(
        importance
    )[::-1]


    for index in sorted_indices:

        if importance[index] == 0:
            continue

        print(
            f"{FEATURE_NAMES[index]:12s} "
            f"{importance[index]:.4f}"
        )


    # --------------------------------------------------------
    # Save sklearn model
    # --------------------------------------------------------

    model_file = os.path.join(
        MODEL_DIR,
        "decision_tree.pkl"
    )


    import joblib

    joblib.dump(
        model,
        model_file
    )


    # --------------------------------------------------------
    # Save feature list
    # --------------------------------------------------------

    feature_file = os.path.join(
        MODEL_DIR,
        "features.txt"
    )


    with open(
        feature_file,
        "w"
    ) as file:

        for index, name in enumerate(
            FEATURE_NAMES
        ):

            file.write(
                f"{index}: {name}\n"
            )


    # --------------------------------------------------------
    # Generate C header
    # --------------------------------------------------------

    header_file = os.path.join(
        MODEL_DIR,
        "pothole_model.h"
    )


    generate_c_header(
        model,
        header_file
    )


    # --------------------------------------------------------
    # Complete
    # --------------------------------------------------------

    print()
    print("========================================")
    print("       MODEL FILES GENERATED")
    print("========================================")
    print()


    print(
        model_file
    )

    print(
        feature_file
    )

    print(
        header_file
    )


    print()
    print(
        "To deploy the trained model:"
    )

    print()
    print(
        "cp model/pothole_model.h "
        "esp32_inference/main/pothole_model.h"
    )

    print()


if __name__ == "__main__":

    main()
