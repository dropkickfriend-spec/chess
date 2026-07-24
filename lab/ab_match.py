#!/usr/bin/env python3
"""Ablation harness: measure what each eval mechanism is worth, in Elo.

Method (standard engine A/B testing): play the full engine (baseline) head to
head against a copy with ONE mechanism switched off, over a fixed set of varied
openings, both colours, at a fixed search depth. The score of baseline-vs-ablated
is the mechanism's contribution. Fixed depth + a deterministic engine means the
whole run is reproducible: re-running gives the identical result, so the error
bars measure how much the verdict leans on the particular opening set, not
run-to-run noise.

A mechanism is ablated by APPENDING "name index 0" lines to a copy of the
baseline weights (data/learned_values.txt); the engine's loader is last-wins, so
those zero out the term while everything else stays identical. The route book is
disabled (engines run in an empty cwd with no route_book.txt) so every move is a
pure function of (position, weights).

Usage:
  python3 lab/ab_match.py                 # run every preset ablation
  python3 lab/ab_match.py trap_risk press # only these
  python3 lab/ab_match.py --depth 7 --openings 30
  python3 lab/ab_match.py --set "trap_w 0 0,trap_w 1 0"   # custom ablation
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile

import chess
import chess.engine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "chess")
BASELINE_WEIGHTS = os.path.join(ROOT, "data", "learned_values.txt")  # --baseline overrides

# Varied opening lines (UCI), a few plies each, so deterministic engines don't
# just replay one game. Both sides get each opening as White and as Black.
OPENINGS = [
    "e2e4 e7e5 g1f3 b8c6 f1b5",      # Ruy Lopez
    "e2e4 e7e5 g1f3 b8c6 f1c4",      # Italian
    "e2e4 c7c5 g1f3 d7d6 d2d4",      # Open Sicilian
    "e2e4 c7c5 b1c3",                # Closed Sicilian
    "e2e4 e7e6 d2d4 d7d5",           # French
    "e2e4 c7c6 d2d4 d7d5",           # Caro-Kann
    "d2d4 d7d5 c2c4 e7e6",           # QGD
    "d2d4 d7d5 c2c4 c7c6",           # Slav
    "d2d4 g8f6 c2c4 g7g6",           # KID/Gruenfeld
    "d2d4 g8f6 c2c4 e7e6",           # Nimzo/QID
    "c2c4 e7e5",                     # English
    "g1f3 d7d5 g2g3",               # Reti/KIA
    "e2e4 e7e5 g1f3 g8f6",           # Petrov
    "e2e4 d7d5",                     # Scandinavian
    "d2d4 f7f5",                     # Dutch
    "e2e4 g7g6",                     # Modern
    "e2e4 g8f6",                     # Alekhine
    "d2d4 d7d5 c1f4",               # London
    "e2e4 e7e5 b1c3",               # Vienna
    "c2c4 c7c5",                     # Symmetrical English
]

OPENING_LINES_FILE = os.path.join(ROOT, "data", "opening_lines.txt")


def load_openings(limit):
    """Built-in openings plus data/opening_lines.txt truncated at several
    depths (more distinct, still-balanced start positions => more games =>
    tighter error bars). Deterministic: sorted and deduped."""
    ops = set(OPENINGS)
    if os.path.exists(OPENING_LINES_FILE):
        with open(OPENING_LINES_FILE) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                moves = line.split()
                for ply in (4, 6, 8):          # a short, medium, long cut
                    if len(moves) >= ply:
                        ops.add(" ".join(moves[:ply]))
    ordered = sorted(ops)
    return ordered[:limit] if limit else ordered

# Preset ablations: name -> list of "block index" pairs to zero (all indices of
# a block if index is None). Zeroing a block disables that mechanism.
PRESETS = {
    # High-frequency core terms (what's actually doing the lifting):
    "pst":        [("pst_mg[0]", None), ("pst_mg[1]", None), ("pst_mg[2]", None),
                   ("pst_mg[3]", None), ("pst_mg[4]", None), ("pst_mg[5]", None),
                   ("pawn_eg", None), ("king_eg", None)],   # piece-square tables
    "plan":       [("PLAN_PART", 0), ("PLAN_IDLE", 0), ("PLAN_ENGAGE", 0),
                   ("LOGI_PLAN", 0), ("SQV_PLAN", 0),
                   ("LINE_KING", 0), ("LINE_HEAVY", 0)],    # game-plan machinery
    "pawn_struct":[("ISOLATED_MG", 0), ("ISOLATED_EG", 0), ("DOUBLED_MG", 0),
                   ("DOUBLED_EG", 0), ("BISHOP_PAIR_MG", 0), ("BISHOP_PAIR_EG", 0),
                   ("ROOK_OPEN", 0), ("ROOK_SEMIOPEN", 0), ("SHIELD_BONUS", 0)],
    "coord":      [("coord_w", None)],
    "restriction":[("action_w", 1)],
    "blockade":   [("action_w", 0)],
    # plan sub-parts, for drilling in once the whole-plan number is known:
    "plan_engage":[("PLAN_ENGAGE", 0)],
    "sqv_plan":   [("SQV_PLAN", 0)],
}
BLOCK_COUNTS = {"coord_w": 5,
                "pst_mg[0]": 64, "pst_mg[1]": 64, "pst_mg[2]": 64,
                "pst_mg[3]": 64, "pst_mg[4]": 64, "pst_mg[5]": 64,
                "pawn_eg": 64, "king_eg": 64}


def override_lines(spec):
    """spec: list of (block, index-or-None) -> weight lines that zero them."""
    lines = []
    for block, idx in spec:
        if idx is None:
            for i in range(BLOCK_COUNTS.get(block, 1)):
                lines.append(f"{block} {i} 0")
        else:
            lines.append(f"{block} {idx} 0")
    return lines


def make_variant(extra_lines):
    """A weights file = baseline + appended override lines (last-wins)."""
    base = ""
    if os.path.exists(BASELINE_WEIGHTS):
        with open(BASELINE_WEIGHTS) as f:
            base = f.read()
    fd, path = tempfile.mkstemp(suffix=".txt", prefix="ablate_")
    with os.fdopen(fd, "w") as f:
        f.write(base)
        if not base.endswith("\n"):
            f.write("\n")
        f.write("\n".join(extra_lines) + "\n")
    return path


def open_engine(weights_path, workdir):
    env = dict(os.environ)
    env["CHESS_WEIGHTS"] = os.path.abspath(weights_path)
    env.pop("CHESS_BOOK", None)          # disable the route book for clean A/B
    return chess.engine.SimpleEngine.popen_uci(ENGINE, env=env, cwd=workdir)


def play_game(white_w, black_w, opening, depth, workdir, max_plies=200):
    """Play one game; return result from WHITE's perspective (1/0.5/0)."""
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    we = open_engine(white_w, workdir)
    be = open_engine(black_w, workdir)
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
            eng = we if board.turn == chess.WHITE else be
            res = eng.play(board, chess.engine.Limit(depth=depth))
            board.push(res.move)
    finally:
        we.quit()
        be.quit()
    if board.is_checkmate():
        return 0.0 if board.turn == chess.WHITE else 1.0
    return 0.5   # draw, stalemate, or hit the ply cap


def phi(z):
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def run_ablation(name, extra_lines, openings, depth):
    variant = make_variant(extra_lines)
    workdir = tempfile.mkdtemp(prefix="ab_")
    # A = baseline (all mechanisms on), B = ablated. Each opening played twice:
    # A as White, then B as White (A as Black). Score is A's, i.e. the mechanism.
    scores = []
    w = d = l = 0
    for op in openings:
        r = play_game(BASELINE_WEIGHTS, variant, op, depth, workdir)  # A white
        scores.append(r);  w += r == 1.0; d += r == 0.5; l += r == 0.0
        r = play_game(variant, BASELINE_WEIGHTS, op, depth, workdir)  # A black
        a = 1.0 - r
        scores.append(a); w += a == 1.0; d += a == 0.5; l += a == 0.0
    os.remove(variant)
    n = len(scores)
    score = sum(scores) / n
    mean = score
    var = sum((s - mean) ** 2 for s in scores) / n
    se = math.sqrt(var / n) if n else 0.0
    los = phi((score - 0.5) / se) if se > 1e-9 else (1.0 if score > 0.5 else 0.0)
    if 0.0 < score < 1.0:
        elo = -400.0 * math.log10(1.0 / score - 1.0)
        elo_se = (400.0 / math.log(10)) * se / (score * (1 - score)) if 0 < score < 1 else 0
    else:
        elo = float("inf") if score >= 1.0 else float("-inf")
        elo_se = 0.0
    print(f"\n=== ablate {name} ===")
    print(f"  baseline vs ablated: +{w} ={d} -{l}  over {n} games (depth {depth})")
    print(f"  baseline score: {100*score:5.1f}%   Elo +{elo:6.1f} +/- {elo_se:.0f}"
          f"   LOS {100*los:5.1f}%")
    verdict = ("HELPS" if los > 0.95 else "HURTS" if los < 0.05
               else "no measurable effect")
    print(f"  -> mechanism {verdict}"
          + (f" (worth ~{elo:.0f} Elo)" if verdict == "HELPS" else ""))
    return {"name": name, "n": n, "wdl": (w, d, l), "score": score,
            "elo": elo, "los": los}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("presets", nargs="*", help="preset names (default: all)")
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--baseline", default=None,
                    help="baseline weights file (default data/learned_values.txt)")
    ap.add_argument("--openings", type=int, default=0,
                    help="cap on number of openings (0 = all available)")
    ap.add_argument("--set", default=None,
                    help='custom ablation: comma-separated "block index value=0" '
                         'lines, e.g. "trap_w 0 0,PRESS 0 0"')
    args = ap.parse_args()

    if not os.path.exists(ENGINE):
        sys.exit(f"engine not built: {ENGINE} (run make)")
    if args.baseline:
        global BASELINE_WEIGHTS
        BASELINE_WEIGHTS = os.path.abspath(args.baseline)
    openings = load_openings(args.openings)
    print(f"opening set: {len(openings)} positions "
          f"-> {2*len(openings)} games per ablation")

    if args.set:
        lines = [s.strip() for s in args.set.split(",")]
        run_ablation("custom", lines, openings, args.depth)
        return

    names = args.presets or list(PRESETS)
    results = []
    for name in names:
        if name not in PRESETS:
            print(f"unknown preset {name}; known: {', '.join(PRESETS)}")
            continue
        results.append(run_ablation(name, override_lines(PRESETS[name]),
                                    openings, args.depth))

    if len(results) > 1:
        print("\n==== summary (most valuable first) ====")
        for r in sorted(results, key=lambda x: -x["elo"]):
            print(f"  {r['name']:12s} Elo +{r['elo']:6.1f}  LOS {100*r['los']:5.1f}%")


if __name__ == "__main__":
    main()
