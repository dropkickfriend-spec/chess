#!/usr/bin/env python3
"""SPSA: tune strategy weights against PLAYING STRENGTH directly.

Why this exists. The Texel-style fitter (tools/fit_strategy_weights.py) fits a
PROXY objective -- "predict the game result from a static eval of quiet
positions" -- and that proxy demonstrably disagrees with strength: unregularised
it halves SQUARE_VALUE, which the ablation harness measured at +103 Elo, and
triples OPPOSITION, a term that only applies in pure pawn endings. Optimising it
harder just converges faster on the wrong target.

SPSA (simultaneous perturbation stochastic approximation) optimises the thing we
actually want. Each iteration:

  1. draw a random direction (each weight kicked +/- c, all at once),
  2. play theta*(1+c*delta) against theta*(1-c*delta) over the SAME openings,
     both colours -- the paired design cancels opening luck, which is the whole
     reason the same position is played twice,
  3. step theta toward whichever side scored better.

Two games per weight is a very noisy gradient, but it is UNBIASED, so it
averages out over iterations -- and unlike the fitter it cannot converge to
something that predicts results well yet plays badly. This is how real engines
tune.

Usage:
  python3 lab/spsa.py --iters 40 --openings-per-iter 8 --depth 5
  python3 lab/spsa.py --start strategy_weights.txt --out tuned_sw.txt
"""
import argparse
import math
import os
import random
import shutil
import sys
import tempfile

import chess
import chess.engine

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_match

N = 16
NAMES = ["DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING", "PIECE_ACTIVITY",
         "ATTACK_POTENTIAL", "PAWN_STRUCTURE", "DEFENDER_LOGISTICS", "KING_ACTIVITY",
         "PAWN_PROMOTION", "OPPOSITION", "MATERIAL", "COORDINATION", "GAME_PLAN",
         "SQUARE_VALUE", "BLOCKADE", "RESTRICTION"]

FLOOR, CEIL = 0.25, 4.0


def load_weights(path):
    w = [1.0] * N
    if path and os.path.exists(path):
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 3 and p[0] == "weight":
                    i = int(p[1])
                    if 0 <= i < N:
                        w[i] = float(p[2])
    return w


def write_weights(w, path):
    with open(path, "w") as f:
        for i, v in enumerate(w):
            f.write(f"weight {i} {v:.4f}\n")


def normalise(w):
    """Only ratios matter to the engine; keep the mean at 1.0 and stay in range."""
    w = [min(max(v, FLOOR), CEIL) for v in w]
    m = sum(w) / len(w)
    return [v / m for v in w] if m > 1e-9 else w


def side_dir(w):
    d = tempfile.mkdtemp(prefix="spsa_")
    write_weights(w, os.path.join(d, "strategy_weights.txt"))
    return d


def play(wdir, bdir, values, opening, depth, max_plies=200):
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    env = dict(os.environ)
    env["CHESS_WEIGHTS"] = os.path.abspath(values)
    env.pop("CHESS_BOOK", None)
    we = chess.engine.SimpleEngine.popen_uci(ab_match.ENGINE, env=env, cwd=wdir)
    be = chess.engine.SimpleEngine.popen_uci(ab_match.ENGINE, env=env, cwd=bdir)
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
            eng = we if board.turn == chess.WHITE else be
            board.push(eng.play(board, chess.engine.Limit(depth=depth)).move)
    finally:
        we.quit(); be.quit()
    if board.is_checkmate():
        return 0.0 if board.turn == chess.WHITE else 1.0
    return 0.5


def match(wa, wb, openings, values, depth):
    """Score for A over paired openings, each played both colours."""
    A, B = side_dir(wa), side_dir(wb)
    total = 0.0
    for op in openings:
        total += play(A, B, values, op, depth)              # A white
        total += 1.0 - play(B, A, values, op, depth)        # A black
    shutil.rmtree(A, ignore_errors=True)
    shutil.rmtree(B, ignore_errors=True)
    return total / (2 * len(openings))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", default=None, help="starting weights (default uniform)")
    ap.add_argument("--out", default="spsa_weights.txt")
    ap.add_argument("--values", default=ab_match.BASELINE_WEIGHTS)
    ap.add_argument("--iters", type=int, default=40)
    ap.add_argument("--openings-per-iter", type=int, default=8)
    ap.add_argument("--depth", type=int, default=5)
    ap.add_argument("-c", type=float, default=0.20, help="perturbation size")
    ap.add_argument("-a", type=float, default=0.30, help="step size")
    ap.add_argument("--validate-every", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    allops = ab_match.load_openings(0)
    theta = normalise(load_weights(args.start))
    start = list(theta)

    print(f"SPSA: {args.iters} iters x {args.openings_per_iter} openings "
          f"({2*args.openings_per_iter} games/iter), depth {args.depth}, "
          f"c={args.c} a={args.a}")
    print(f"start: " + " ".join(f"{n[:4]}={v:.2f}" for n, v in zip(NAMES, theta)))

    pos = 0
    for k in range(1, args.iters + 1):
        # Decaying schedule: big exploratory steps early, fine steps later.
        ck = args.c / (k ** 0.101)
        ak = args.a / (k ** 0.602)

        delta = [rng.choice([-1.0, 1.0]) for _ in range(N)]
        plus = normalise([t * (1 + ck * d) for t, d in zip(theta, delta)])
        minus = normalise([t * (1 - ck * d) for t, d in zip(theta, delta)])

        # Rotate which openings this iteration sees, so successive gradients
        # sample different positions instead of overfitting one set.
        ops = [allops[(pos + i) % len(allops)] for i in range(args.openings_per_iter)]
        pos += args.openings_per_iter

        score = match(plus, minus, ops, args.values, args.depth)
        g = score - 0.5                       # >0 => the + side played better
        theta = normalise([t * (1 + ak * g * 2.0 * d) for t, d in zip(theta, delta)])

        print(f"iter {k:3d}: +side {100*score:5.1f}%  step {ak*g*2:+.3f}  "
              f"MAT={theta[10]:.2f} SQV={theta[13]:.2f} RES={theta[15]:.2f}",
              flush=True)

        if args.validate_every and k % args.validate_every == 0:
            v = match(theta, start, allops[:16], args.values, args.depth)
            print(f"  == validation vs start: {100*v:.1f}% "
                  f"({'ahead' if v > 0.5 else 'behind'}) ==", flush=True)

    write_weights(theta, args.out)
    print("\nfinal weights -> " + args.out)
    for n, v in sorted(zip(NAMES, theta), key=lambda t: -t[1]):
        print(f"  {n:20s} {v:.4f}")

    print("\nfinal validation vs start (32 openings, 64 games)...")
    v = match(theta, start, allops[:32], args.values, args.depth)
    elo = -400.0 * math.log10(1.0 / v - 1.0) if 0 < v < 1 else float("inf")
    print(f"  tuned vs start: {100*v:.1f}%  ({elo:+.0f} Elo)")
    print("  -> keep the tuned file only if this is clearly above 50%")


if __name__ == "__main__":
    main()
