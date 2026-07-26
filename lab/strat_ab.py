#!/usr/bin/env python3
"""A/B two STRATEGY-WEIGHT files head to head.

The engine loads `strategy_weights.txt` from its working directory, so each side
gets its own cwd containing the file under test. Answers the question the whole
learning apparatus rests on: are the learned strategy weights actually better
than plain uniform 1.0?

Usage:
  python3 lab/strat_ab.py A.txt B.txt --depth 6      # A vs B
  python3 lab/strat_ab.py A.txt uniform --depth 6    # A vs all-1.0
"""
import argparse
import math
import os
import shutil
import sys
import tempfile

import chess
import chess.engine

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_match

N_STRATS = 15


def make_side(path):
    """A cwd holding this side's strategy_weights.txt ('uniform' = all 1.0)."""
    d = tempfile.mkdtemp(prefix="strat_")
    dst = os.path.join(d, "strategy_weights.txt")
    if path == "uniform":
        with open(dst, "w") as f:
            for name in ab_match.STRAT_NAMES:
                f.write(f"weight {name} 1.0\n")
    else:
        shutil.copy(path, dst)
    return d


def open_engine(cwd, values):
    env = dict(os.environ)
    env["CHESS_WEIGHTS"] = os.path.abspath(values)
    env.pop("CHESS_BOOK", None)
    return chess.engine.SimpleEngine.popen_uci(ab_match.ENGINE, env=env, cwd=cwd)


def play(wdir, bdir, values, opening, depth, max_plies=200):
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    we = open_engine(wdir, values)
    be = open_engine(bdir, values)
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
            eng = we if board.turn == chess.WHITE else be
            board.push(eng.play(board, chess.engine.Limit(depth=depth)).move)
    finally:
        we.quit(); be.quit()
    if board.is_checkmate():
        return 0.0 if board.turn == chess.WHITE else 1.0
    return 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a", help="side A strategy weights file (or 'uniform')")
    ap.add_argument("b", help="side B strategy weights file (or 'uniform')")
    ap.add_argument("--values", default=ab_match.BASELINE_WEIGHTS,
                    help="shared learned_values.txt (both sides identical)")
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--openings", type=int, default=0)
    args = ap.parse_args()

    A, B = make_side(args.a), make_side(args.b)
    openings = ab_match.load_openings(args.openings)
    print(f"strategy-weight A/B: {args.a} vs {args.b}\n"
          f"{len(openings)} openings -> {2*len(openings)} games, depth {args.depth}")

    scores, w, d, l = [], 0, 0, 0
    for op in openings:
        r = play(A, B, args.values, op, args.depth)        # A white
        scores.append(r); w += r == 1.0; d += r == 0.5; l += r == 0.0
        r = play(B, A, args.values, op, args.depth)        # A black
        a = 1.0 - r
        scores.append(a); w += a == 1.0; d += a == 0.5; l += a == 0.0

    n = len(scores)
    score = sum(scores) / n
    var = sum((s - score) ** 2 for s in scores) / n
    se = math.sqrt(var / n) if n else 0.0
    los = ab_match.phi((score - 0.5) / se) if se > 1e-9 else (1.0 if score > 0.5 else 0.0)
    elo = -400.0 * math.log10(1.0 / score - 1.0) if 0.0 < score < 1.0 else float("inf")
    print(f"\nA ({args.a}) vs B ({args.b}): +{w} ={d} -{l}  over {n} games")
    print(f"  A score {100*score:.1f}%   Elo {elo:+.1f}   LOS {100*los:.1f}%")
    if los > 0.95:
        print("  -> A is stronger")
    elif los < 0.05:
        print("  -> B is stronger")
    else:
        print("  -> no measurable difference")


if __name__ == "__main__":
    main()
