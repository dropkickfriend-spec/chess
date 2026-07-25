#!/usr/bin/env python3
"""Replay logged games through a candidate online-learning rule, offline.

Playing 40 games to find out whether a weight-update rule works costs hours and
confounds the answer, because the brain moves while you measure it. But the
update rule is a pure function of (positions, outcome) — both of which are
already stored for every game. So we can replay the exact same games through a
candidate rule in seconds and inspect what it does to the weights.

This is what caught the previous rule. Replaying its own 40-game run shows it
producing only FIVE distinct weights across fifteen strategies, because its
credit term came from a hardcoded phase table and so was near-constant: the
update was rank-1 and structurally could not express anything else.

Two diagnostics, both cheap and both decisive:

  * RANK — how many distinct values the rule can produce. A rule that cannot
    separate two strategies will never learn that one is better than the other.
  * DIRECTION — Spearman correlation against the ablation ground truth in
    lab/README.md (measured Elo per strategy). A learning rule that moves
    ANTI-correlated with measured strength is worse than not learning.

Usage:
  python3 lab/replay_nudge.py                      # last 40 skill 5-8 games
  python3 lab/replay_nudge.py --limit 100 --lr 0.08 --meanrev 0.02
  python3 lab/replay_nudge.py --rule old           # reproduce the collapse
"""
import argparse
import json
import os
import sys
import urllib.request

import chess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from analyze_game import (STRATEGIES, adjust_weights, detect_phase,
                          eval_contributions, load_weights, strategy_verdicts)

ENGINE = os.path.join(ROOT, "chess")

# Head-to-head Elo per strategy from lab/README.md. Only the terms that were
# actually ablated appear; these are the ground truth a learning rule should be
# moving toward. None of them came from this nudge — they are independent.
ABLATION_ELO = {
    "SQUARE_VALUE": 103,
    "RESTRICTION": 66,
    "DEFENDER_LOGISTICS": 44,
    "PAWN_PROMOTION": 44,
    "PAWN_STRUCTURE": 22,
    "BLOCKADE": 8,
    "KING_ACTIVITY": 0,
    "OPPOSITION": 0,
    "COORDINATION": -19,
}


def fetch_games(limit, skills):
    from match import load_supabase_env
    conf = load_supabase_env()
    base = (os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL", "")).rstrip("/")
    key = os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY")
    if not base or not key:
        sys.exit("no Supabase credentials")
    q = (f"{base}/rest/v1/chessbb_games?select=our_color,result,uci,sf_skill"
         f"&uci=not.is.null&order=created_at.desc&limit={limit}")
    if skills:
        q += f"&sf_skill=in.({','.join(str(s) for s in skills)})"
    req = urllib.request.Request(q, headers={"apikey": key,
                                             "Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req) as r:
        rows = json.load(r)
    rows.reverse()                     # oldest first: replay in play order
    return rows


def game_log(uci):
    """Rebuild exactly what trace_strategies would have logged for this game:
    the FEN after every ply plus the phase-membership list the old rule used as
    its credit signal. Reproducing that table faithfully is what makes the
    'old' mode a real control rather than a strawman."""
    board, log = chess.Board(), []
    for mv in uci.split():
        try:
            is_pawn_move = board.piece_type_at(chess.Move.from_uci(mv).from_square) == chess.PAWN
            board.push_uci(mv)
        except Exception:
            break
        phase = detect_phase(board)
        active = ["MATERIAL", "COORDINATION", "SQUARE_VALUE", "BLOCKADE", "RESTRICTION"]
        if phase == "opening":
            active += ["DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING"]
        elif phase == "middlegame":
            active += ["PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE"]
        else:
            active += ["KING_ACTIVITY", "PAWN_PROMOTION", "OPPOSITION"]
        if is_pawn_move:
            active.append("DEFENDER_LOGISTICS")
        log.append({"fen": board.fen(), "active_strategies": active})
    return log


def clusters(values, tol=0.01):
    """Group weights that are within `tol` relative of each other.

    Counting exact distinct values is useless here: the degenerate rule produced
    groups agreeing to three decimals but differing in the fourth, which reads
    as "all distinct" while being five values in every way that matters."""
    out = []
    for v in sorted(values):
        if out and abs(v - out[-1][-1]) <= tol * max(abs(v), 1e-9):
            out[-1].append(v)
        else:
            out.append([v])
    return out


def spearman(a, b):
    """Rank correlation, ties averaged. Small n, so keep it simple."""
    def ranks(xs):
        order = sorted(range(len(xs)), key=lambda i: xs[i])
        r = [0.0] * len(xs)
        i = 0
        while i < len(order):
            j = i
            while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
                j += 1
            avg = (i + j) / 2.0 + 1
            for k in range(i, j + 1):
                r[order[k]] = avg
            i = j + 1
        return r
    ra, rb = ranks(a), ranks(b)
    n = len(a)
    ma, mb = sum(ra) / n, sum(rb) / n
    num = sum((ra[i] - ma) * (rb[i] - mb) for i in range(n))
    da = sum((ra[i] - ma) ** 2 for i in range(n)) ** 0.5
    db = sum((rb[i] - mb) ** 2 for i in range(n)) ** 0.5
    return num / (da * db) if da > 1e-9 and db > 1e-9 else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--skills", default="5,6,7,8",
                    help="comma list, or empty for all")
    ap.add_argument("--start", default="data/strategy_weights_spsa.txt")
    ap.add_argument("--lr", type=float, default=0.08)
    ap.add_argument("--meanrev", type=float, default=0.02)
    ap.add_argument("--rule", choices=["new", "old"], default="new",
                    help="'old' ignores the eval probe, reproducing the "
                         "participation-based rule that collapsed")
    ap.add_argument("--out", default=None, help="write the final vector here")
    args = ap.parse_args()

    skills = [int(s) for s in args.skills.split(",") if s.strip()]
    rows = fetch_games(args.limit, skills)
    start = args.start if os.path.isabs(args.start) else os.path.join(ROOT, args.start)
    weights = load_weights(start)
    prior = dict(weights)
    print(f"replaying {len(rows)} games through the '{args.rule}' rule "
          f"(LR={args.lr}, MEANREV={args.meanrev})")
    print(f"  start: {os.path.relpath(start, ROOT)}")

    keys = list(STRATEGIES)
    for i, row in enumerate(rows):
        log = game_log(row["uci"] or "")
        if len(log) < 4:
            continue
        fens = [e["fen"] for e in log]
        we_white = row["our_color"] == "white"
        if row["result"] == "1/2-1/2":
            outcome = 0.5
        else:
            outcome = 1.0 if (row["result"] == "1-0") == we_white else 0.0

        verdicts = None
        if args.rule == "new":
            contribs = eval_contributions(ENGINE, fens)
            if contribs:
                verdicts = strategy_verdicts(contribs, we_white, keys)
        adjust_weights(weights, log, outcome, 0.5,
                       LR=args.lr, MEANREV=args.meanrev, verdicts=verdicts)
        if (i + 1) % 10 == 0:
            print(f"  after {i+1:3d} games: "
                  f"{len(clusters(weights.values()))} distinct weight levels")

    print("\nfinal vector:")
    for s in sorted(weights, key=lambda k: -weights[k]):
        arrow = "up  " if weights[s] > prior[s] else "down"
        print(f"  {s:<22}{weights[s]:7.4f}   ({arrow} from {prior[s]:.4f})")

    cl = clusters(weights.values())
    print(f"\nRANK: {len(cl)} distinct weight levels across {len(weights)} "
          f"strategies (1% tolerance)")
    if len(cl) < len(weights) / 2:
        print("  -> DEGENERATE: the rule cannot separate most strategies")
        for g in cl:
            if len(g) > 1:
                names = [s for s in weights if any(abs(weights[s]-v) < 1e-12 for v in g)]
                print(f"       tied at ~{sum(g)/len(g):.4f}: {', '.join(sorted(names))}")

    common = [s for s in ABLATION_ELO if s in weights]
    moved = [weights[s] / prior[s] for s in common]
    rho = spearman(moved, [ABLATION_ELO[s] for s in common])
    # Critical |rho| for p<0.05, two-tailed, small n. With only nine ablated
    # strategies this bar is high on purpose: it is very easy to read a story
    # into a rank correlation over nine points that is pure noise.
    crit = {5: 0.90, 6: 0.83, 7: 0.75, 8: 0.71, 9: 0.68, 10: 0.65,
            11: 0.62, 12: 0.59, 13: 0.57, 14: 0.54, 15: 0.52}.get(len(common), 0.5)
    print(f"DIRECTION: Spearman vs ablation Elo = {rho:+.2f} "
          f"(n={len(common)}, needs |rho|>{crit:.2f} for p<0.05)")
    if abs(rho) < crit:
        print("  -> NOT SIGNIFICANT: no measurable relationship either way")
    elif rho < 0:
        print("  -> ANTI-CORRELATED: it is unlearning what ablation measured")
    else:
        print("  -> agrees with measured ground truth")

    if args.out:
        path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
        with open(path, "w") as f:
            for s in STRATEGIES:
                f.write(f"weight {s} {weights[s]:.4f}\n")
        print(f"\nwrote {path}")


if __name__ == "__main__":
    main()
