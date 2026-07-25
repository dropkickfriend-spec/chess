#!/usr/bin/env python3
"""A/B a boolean engine env flag (set vs unset): same weights, over openings,
both colours, fixed depth. Measures the flag's Elo.

E.g. does disabling DEFENDER_LOGISTICS (CHESS_NOLOGI=1) cost strength? If the
flag-set side scores >=50%, the disabled term wasn't helping -- cut it for the
free speed.

Usage:
  python3 lab/env_ab.py CHESS_NOLOGI --depth 6
"""
import argparse
import math
import os
import sys
import tempfile

import chess
import chess.engine

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_match


def open_engine(weights, flag, on, wd):
    env = dict(os.environ)
    env["CHESS_WEIGHTS"] = os.path.abspath(weights)
    env.pop("CHESS_BOOK", None)
    if on:
        env[flag] = "1"
    else:
        env.pop(flag, None)
    return chess.engine.SimpleEngine.popen_uci(ab_match.ENGINE, env=env, cwd=wd)


def play(weights, flag, wflag, bflag, opening, limit, wd, max_plies=200):
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    we = open_engine(weights, flag, wflag, wd)
    be = open_engine(weights, flag, bflag, wd)
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
            eng = we if board.turn == chess.WHITE else be
            board.push(eng.play(board, limit).move)
    finally:
        we.quit(); be.quit()
    if board.is_checkmate():
        return 0.0 if board.turn == chess.WHITE else 1.0
    return 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("flag", help="env var; A = flag SET, B = flag unset")
    ap.add_argument("--weights", default=ab_match.BASELINE_WEIGHTS)
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--nodes", type=int, default=0,
                    help="fixed node budget per move (correct for search-efficiency); "
                         "overrides --depth")
    ap.add_argument("--openings", type=int, default=0)
    args = ap.parse_args()

    limit = (chess.engine.Limit(nodes=args.nodes) if args.nodes
             else chess.engine.Limit(depth=args.depth))
    budget = f"{args.nodes} nodes" if args.nodes else f"depth {args.depth}"

    wd = tempfile.mkdtemp()
    openings = ab_match.load_openings(args.openings)
    print(f"env A/B: {args.flag} SET vs unset, {len(openings)} openings "
          f"-> {2*len(openings)} games, {budget}")

    scores, w, d, l = [], 0, 0, 0
    for op in openings:
        r = play(args.weights, args.flag, True, False, op, limit, wd)  # SET white
        scores.append(r); w += r == 1.0; d += r == 0.5; l += r == 0.0
        r = play(args.weights, args.flag, False, True, op, limit, wd)  # SET black
        a = 1.0 - r
        scores.append(a); w += a == 1.0; d += a == 0.5; l += a == 0.0

    n = len(scores)
    score = sum(scores) / n
    var = sum((s - score) ** 2 for s in scores) / n
    se = math.sqrt(var / n) if n else 0.0
    los = ab_match.phi((score - 0.5) / se) if se > 1e-9 else (1.0 if score > 0.5 else 0.0)
    elo = -400.0 * math.log10(1.0 / score - 1.0) if 0.0 < score < 1.0 else float("inf")
    print(f"\n{args.flag} SET vs unset: +{w} ={d} -{l}  over {n} games")
    print(f"  SET score {100*score:.1f}%   Elo +{elo:.1f}   LOS {100*los:.1f}%")
    print(f"  (SET >= ~50% at good LOS => the disabled work wasn't helping; cut it)")


if __name__ == "__main__":
    main()
