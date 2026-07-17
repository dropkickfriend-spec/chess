#!/usr/bin/env python3
"""Fit strategy weights to game outcomes — the optimal pre-game calibration.

Instead of nudging weights and normalising by their average, fit them
directly: take the engine's raw per-strategy scores (evalfens) for every
logged quiet position, and find the weight vector that best predicts the
game results those positions came from (Texel objective, logistic link).
The result is the best linear calibration of the strategies the data can
justify — no averaging heuristic, no hand-set adjustment factors.

Usage:
  python3 tools/fit_strategy_weights.py [--apply] [dataset]

  dataset defaults to data/texel_dataset.txt ("FEN;result" lines).
  --apply writes strategy_weights.txt (floored at 0.05, mean-scaled to 1.0
  so ratios are explicit) and uploads the snapshot to Supabase.
"""
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

STRATS = ["DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
          "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE",
          "DEFENDER_LOGISTICS", "KING_ACTIVITY", "PAWN_PROMOTION",
          "OPPOSITION", "MATERIAL", "COORDINATION", "GAME_PLAN", "SQUARE_VALUE"]
N = len(STRATS)


def load_dataset(path):
    fens, results = [], []
    with open(path) as f:
        for line in f:
            if ";" not in line:
                continue
            fen, res = line.rsplit(";", 1)
            fens.append(fen.strip())
            results.append(float(res))
    return fens, results


def strategy_features(fens):
    """Raw per-strategy scores (White POV) for each FEN via evalfens."""
    proc = subprocess.run([os.path.join(ROOT, "chess"), "evalfens"],
                          input="\n".join(fens) + "\n",
                          capture_output=True, text=True, cwd=ROOT)
    rows, cur = [], {}
    for line in proc.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "TOTAL":
            rows.append([float(cur.get(s, 0.0)) for s in STRATS])
            cur = {}
        elif len(parts) == 3:
            cur[parts[0]] = int(parts[1])
    return rows


def sigmoid(k, e):
    return 1.0 / (1.0 + 10.0 ** (-k * e / 400.0))


def mse(w, k, X, y):
    err = 0.0
    for x, r in zip(X, y):
        e = sum(wi * xi for wi, xi in zip(w, x))
        d = r - sigmoid(k, e)
        err += d * d
    return err / len(X)


def fit(X, y, iters=600, lr=0.4):
    # Standardise features so one learning rate serves every strategy
    # (MATERIAL raw spans thousands of cp, OPPOSITION a few dozen).
    std = []
    for j in range(N):
        col = [x[j] for x in X]
        mu = sum(col) / len(col)
        var = sum((v - mu) ** 2 for v in col) / len(col)
        std.append(math.sqrt(var) if var > 1e-9 else 1.0)
    Xs = [[x[j] / std[j] for j in range(N)] for x in X]

    # Start from the current uniform calibration (real weight 1.0 each;
    # in standardised space that is w'_j = std_j), then fit K on it.
    w = [std[j] for j in range(N)]

    best_k, best_e = 0.5, None
    for ki in range(31):
        k = 0.5 + ki * 0.05
        e = mse(w, k, Xs, y)
        if best_e is None or e < best_e:
            best_k, best_e = k, e
    print(f"K = {best_k:.2f}, start E = {best_e:.6f} ({len(Xs)} positions)")

    c = math.log(10.0) * best_k / 400.0
    g2 = [1e-8] * N                       # Adagrad accumulators
    for it in range(iters):
        grad = [0.0] * N
        for x, r in zip(Xs, y):
            e = sum(wi * xi for wi, xi in zip(w, x))
            s = sigmoid(best_k, e)
            common = -2.0 * (r - s) * s * (1.0 - s) * c
            for j in range(N):
                grad[j] += common * x[j]
        for j in range(N):
            g = grad[j] / len(Xs)
            g2[j] += g * g
            w[j] -= lr * g / math.sqrt(g2[j])
        if (it + 1) % 200 == 0:
            print(f"iter {it + 1}: E = {mse(w, best_k, Xs, y):.6f}")

    final_e = mse(w, best_k, Xs, y)
    # Undo the standardisation: E = sum(w'_j * x_j/std_j), so the real
    # per-strategy weight is w'_j / std_j.
    real_w = [w[j] / std[j] for j in range(N)]
    return real_w, best_k, final_e


def main():
    apply = "--apply" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dataset = args[0] if args else os.path.join(ROOT, "data", "texel_dataset.txt")

    fens, results = load_dataset(dataset)
    print(f"dataset: {len(fens)} positions")
    X = strategy_features(fens)
    if len(X) != len(results):
        sys.exit(f"feature rows {len(X)} != results {len(results)}")

    w, k, err = fit(X, results)

    # Only ratios matter to the engine's relative reweighting: floor
    # negatives (a harmful signal is silenced, not inverted) and scale
    # the mean to 1.0 so the numbers read as ratios.
    w = [max(v, 0.0) for v in w]
    mean = sum(w) / len(w)
    if mean > 1e-9:
        w = [v / mean for v in w]
    w = [max(v, 0.05) for v in w]

    print(f"\nfitted weights (E = {err:.6f}):")
    for name, v in sorted(zip(STRATS, w), key=lambda t: -t[1]):
        print(f"  {name:20s} {v:.4f}")

    if apply:
        path = os.path.join(ROOT, "strategy_weights.txt")
        with open(path, "w") as f:
            for i, v in enumerate(w):
                f.write(f"weight {i} {v}\n")
        print(f"applied -> {path}")
        try:
            from analyze_game import upload_weights_to_supabase
            upload_weights_to_supabase([], dict(zip(STRATS, w)))
            print("uploaded snapshot to Supabase")
        except Exception as e:
            print(f"supabase upload skipped: {e}")


if __name__ == "__main__":
    main()
