#!/usr/bin/env python3
"""A/B the borrowed-horizon book anchor: same weights, same FROZEN book,
CHESS_ANCHOR on vs off, over openings that sit inside the book. Isolates the
in-search anchor's Elo contribution from the book's root move-play.

Openings are drawn from data/opening_lines.txt (the same lines the book was
built from) so the anchor actually has coverage; games out of book are noise.

Usage:
  python3 lab/book_ab.py /path/to/valued_book.txt --depth 6
"""
import argparse
import math
import os
import shutil
import stat
import sys
import tempfile

import chess
import chess.engine

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_match

LINES_FILE = os.path.join(ab_match.ROOT, "data", "opening_lines.txt")


def in_book_openings(limit):
    ops = set()
    with open(LINES_FILE) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            mv = line.split()
            for ply in (4, 6, 8):
                if len(mv) >= ply:
                    ops.add(" ".join(mv[:ply]))
    ops = sorted(ops)
    return ops[:limit] if limit else ops


def open_engine(weights, book, anchor, wd):
    env = dict(os.environ)
    env["CHESS_WEIGHTS"] = os.path.abspath(weights)
    env["CHESS_BOOK"] = os.path.abspath(book)
    env["CHESS_ANCHOR"] = "1" if anchor else "0"
    return chess.engine.SimpleEngine.popen_uci(ab_match.ENGINE, env=env, cwd=wd)


def play(weights, book, white_anchor, black_anchor, opening, depth, wd, max_plies=200):
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    we = open_engine(weights, book, white_anchor, wd)
    be = open_engine(weights, book, black_anchor, wd)
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
    ap.add_argument("book")
    ap.add_argument("--weights", default=ab_match.BASELINE_WEIGHTS)
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--openings", type=int, default=0)
    args = ap.parse_args()

    # Freeze the book read-only so book_remember's mid-game appends silently
    # fail (fopen "a" returns NULL) and neither engine mutates the shared book.
    fd, frozen = tempfile.mkstemp(suffix=".txt", prefix="book_ro_")
    os.close(fd)
    shutil.copy(args.book, frozen)
    os.chmod(frozen, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
    wd = tempfile.mkdtemp()

    openings = in_book_openings(args.openings)
    print(f"book A/B (anchor ON vs OFF): {len(openings)} in-book openings "
          f"-> {2*len(openings)} games, depth {args.depth}")

    scores, w, d, l = [], 0, 0, 0
    for op in openings:
        r = play(args.weights, frozen, True, False, op, args.depth, wd)   # A(on) white
        scores.append(r); w += r == 1.0; d += r == 0.5; l += r == 0.0
        r = play(args.weights, frozen, False, True, op, args.depth, wd)   # A(on) black
        a = 1.0 - r
        scores.append(a); w += a == 1.0; d += a == 0.5; l += a == 0.0
    os.remove(frozen)

    n = len(scores)
    score = sum(scores) / n
    var = sum((s - score) ** 2 for s in scores) / n
    se = math.sqrt(var / n) if n else 0.0
    los = ab_match.phi((score - 0.5) / se) if se > 1e-9 else (1.0 if score > 0.5 else 0.0)
    elo = -400.0 * math.log10(1.0 / score - 1.0) if 0.0 < score < 1.0 else float("inf")
    print(f"\nanchor ON vs OFF: +{w} ={d} -{l}  over {n} games")
    print(f"  ON score {100*score:.1f}%   Elo +{elo:.1f}   LOS {100*los:.1f}%")
    verdict = ("borrowed horizon ADDS strength" if los > 0.95 else
               "borrowed horizon HURTS" if los < 0.05 else
               "no measurable effect (raise depth/openings, or book too small)")
    print(f"  -> {verdict}")


if __name__ == "__main__":
    main()
