#!/usr/bin/env python3
"""Term-quality probe: does an eval term's influence improve DECISIONS, judged
where it actually acts?

Game ablation dilutes a rare-firing term across whole games and drowns it in
noise. This probe is sharper and self-contained (no external engine, no hand
labels):

  1. Sample real positions (from the texel dataset).
  2. For each, get the engine's SHALLOW best move with the term ON (baseline)
     and OFF (ablated). Keep only positions where they DISAGREE -- i.e. the term
     changed the decision. Everywhere else the term is irrelevant.
  3. For those, compute a DEEP self-search truth with the term both on and off;
     keep only positions where deep-on and deep-off AGREE (robust, unbiased
     truth -- at depth the tiny static term doesn't sway the tactics).
  4. Among the term's decisions, how often does its move match deep truth vs the
     term-off move? More = the term's nudges are improvements.

Usage:
  python3 lab/term_quality.py trap_risk --sample 1500 --shallow 4 --deep 10
  python3 lab/term_quality.py press --baseline /path/to/tuned.txt
"""
import argparse
import math
import os
import sys
import tempfile

import chess
import chess.engine

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_match  # reuse engine/variant plumbing

DATASET = os.path.join(ab_match.ROOT, "data", "texel_dataset.txt")


def load_fens(limit):
    fens = []
    with open(DATASET) as f:
        for line in f:
            if ";" in line:
                fens.append(line.split(";", 1)[0].strip())
            if limit and len(fens) >= limit:
                break
    return fens


def best_move(eng, board, depth):
    try:
        return eng.play(board, chess.engine.Limit(depth=depth)).move
    except Exception:
        return None


def phi(z):
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("preset", help="ablation preset (e.g. trap_risk, press)")
    ap.add_argument("--sample", type=int, default=1500)
    ap.add_argument("--shallow", type=int, default=4)
    ap.add_argument("--deep", type=int, default=10)
    ap.add_argument("--baseline", default=None)
    args = ap.parse_args()

    if args.baseline:
        ab_match.BASELINE_WEIGHTS = os.path.abspath(args.baseline)
    if args.preset not in ab_match.PRESETS:
        sys.exit(f"unknown preset {args.preset}; known: {', '.join(ab_match.PRESETS)}")

    ablated = ab_match.make_variant(ab_match.override_lines(ab_match.PRESETS[args.preset]))
    wd = tempfile.mkdtemp(prefix="tq_")
    on = ab_match.open_engine(ab_match.BASELINE_WEIGHTS, wd)   # term ON
    off = ab_match.open_engine(ablated, wd)                    # term OFF

    fens = load_fens(args.sample)
    changed = []           # positions where the term flipped the shallow move
    for fen in fens:
        try:
            board = chess.Board(fen)
        except Exception:
            continue
        if board.is_game_over():
            continue
        m_on = best_move(on, board, args.shallow)
        m_off = best_move(off, board, args.shallow)
        if m_on and m_off and m_on != m_off:
            changed.append((fen, m_on, m_off))

    on_correct = off_correct = neither = ambiguous = 0
    for fen, m_on, m_off in changed:
        board = chess.Board(fen)
        t_on = best_move(on, board, args.deep)
        t_off = best_move(off, board, args.deep)
        if not t_on or t_on != t_off:      # truth not robust -> skip
            ambiguous += 1
            continue
        truth = t_on
        if truth == m_on:
            on_correct += 1
        elif truth == m_off:
            off_correct += 1
        else:
            neither += 1
    on.quit(); off.quit()
    os.remove(ablated)

    decisive = on_correct + off_correct
    print(f"\n=== term-quality: {args.preset} "
          f"(shallow d{args.shallow} vs truth d{args.deep}) ===")
    print(f"  scanned {len(fens)} positions; term changed the move in "
          f"{len(changed)} ({100*len(changed)/max(len(fens),1):.1f}%)")
    print(f"  of those, deep-truth was robust in {decisive+neither} "
          f"(ambiguous {ambiguous})")
    print(f"  term's move matched truth : {on_correct}")
    print(f"  term-off move matched truth: {off_correct}")
    print(f"  neither (truth a 3rd move) : {neither}")
    if decisive:
        rate = on_correct / decisive
        se = math.sqrt(rate * (1 - rate) / decisive)
        los = phi((rate - 0.5) / se) if se > 1e-9 else (1.0 if rate > 0.5 else 0.0)
        print(f"  -> when the term is decisive, it is RIGHT {100*rate:.0f}% "
              f"of the time (LOS {100*los:.0f}%)")
        verdict = ("term IMPROVES decisions" if los > 0.95 else
                   "term WORSENS decisions" if los < 0.05 else
                   "no clear signal (need larger sample)")
        print(f"  -> {verdict}")
    else:
        print("  -> the term never changed a decision with robust truth; "
              "it fires too rarely at this depth/sample.")


if __name__ == "__main__":
    main()
