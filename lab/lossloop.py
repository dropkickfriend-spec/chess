#!/usr/bin/env python3
"""Play a fixed cycle of games, perturb weights on the ones we LOST, repeat.

The idea: a loss carries more information than a win, but only if you use the
position where it actually went wrong. So each round

  1. plays a FIXED set of openings against Stockfish (same set every round, so
     rounds are comparable),
  2. finds, for every game we lost, the position where the eval collapsed
     across one of OUR moves -- the move that lost it,
  3. runs SPSA seeded from exactly those positions: the same position played
     twice, once with theta*(1+c*delta) and once with theta*(1-c*delta), which
     is what turns "we lost" into a direction in weight space,
  4. adopts the update and replays the same cycle.

Repeats until the target score is reached or --max-rounds is spent.

READ THIS BEFORE TRUSTING THE NUMBER IT PRINTS
----------------------------------------------
This tunes on the games it is scored on. A rising cycle score is NOT evidence
of strength -- it is the definition of overfitting, and with 10 games it can be
achieved by memorising ten openings. So every round also plays a HELD-OUT set
of different openings that is never tuned on. Compare the two columns:

    cycle 6/10 -> 9/10 and held-out 45% -> 44%   = overfitting, learned nothing
    cycle 6/10 -> 9/10 and held-out 45% -> 58%   = it actually got stronger

Only the second is worth keeping, and only lab/strat_ab.py can confirm it.

Also note Stockfish's skill levels are randomised, so "the same ten games" are
the same OPENINGS, not the same games. Two rounds with identical weights will
not score identically; treat small movements as noise.

Usage:
  python3 lab/lossloop.py --skill 5 --games 10 --target 10 --max-rounds 6
  python3 lab/lossloop.py --skill 3 --target 8 --max-rounds 4 --spsa-iters 6
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import uuid

import chess
import chess.engine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "lab"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import ab_match
import spsa as S
from blunder_mine import find_blunder
# Reuse the match runner's upload path rather than writing a second one: it
# already opens the match row up front and writes each game as it finishes, so
# an interrupted run keeps its completed games.
from match import (open_match, upload_game, close_match, durable_insert,
                   drain_pending, queued_count, _supabase_creds)


def log_round(run_id, rnd, args, n, cycle, heldout, wins, losses, tps):
    """One row per round. Retries and spools on failure rather than dropping —
    a dropped round row is exactly what lost round 1 of run 0f5cea082b30."""
    base_url, key = _supabase_creds()
    if not base_url or not key:
        return
    try:
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or None
    except OSError:
        sha = None
    durable_insert(base_url.rstrip("/"), key, "lossloop_rounds", [{
        "run_id": run_id, "round": rnd, "sf_skill": args.skill,
        "movetime_ms": int(args.movetime * 1000), "games": n,
        "cycle_score": cycle, "heldout_score": heldout,
        "wins": wins, "losses": losses, "turning_points": tps,
        "spsa_iters": args.spsa_iters, "engine_sha": sha,
    }])


def stockfish_path():
    return (os.environ.get("STOCKFISH") or shutil.which("stockfish")
            or "/usr/games/stockfish")


def play_vs_sf(weights_dir, values, opening, we_white, skill, movetime,
               max_plies=300):
    """One game from `opening`. Returns (score_for_us, uci_moves, white_evals)."""
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    env = dict(os.environ)
    env["CHESS_WEIGHTS"] = os.path.abspath(values)
    env.pop("CHESS_BOOK", None)
    us = chess.engine.SimpleEngine.popen_uci(ab_match.ENGINE, env=env,
                                             cwd=weights_dir)
    sf = chess.engine.SimpleEngine.popen_uci(stockfish_path())
    sf.configure({"Skill Level": skill})
    moves, evals = list(opening.split()), [None] * len(opening.split())
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
            our_turn = (board.turn == chess.WHITE) == we_white
            eng = us if our_turn else sf
            r = eng.play(board, chess.engine.Limit(time=movetime),
                         info=chess.engine.INFO_SCORE)
            board.push(r.move)
            moves.append(r.move.uci())
            sc = r.info.get("score") if r.info else None
            # Stored from WHITE's point of view, matching chessbb_games.evals,
            # which is what find_blunder expects.
            evals.append(sc.white().score(mate_score=10000) if sc else None)
    finally:
        us.quit(); sf.quit()
    if board.is_checkmate():
        winner_white = board.turn == chess.BLACK
        score = 1.0 if winner_white == we_white else 0.0
    else:
        score = 0.5
    return score, moves, evals


def run_cycle(theta, values, openings, skill, movetime, live, handle=None,
              tag=""):
    """Play each opening as both colours. Returns (score, wins, losses, rows).

    `handle` is an open match row; each finished game is written immediately, so
    an interrupted run keeps everything it already played."""
    d = S.side_dir(theta)
    total = wins = losses = 0.0
    rows = []
    try:
        n = 0
        for op in openings:
            for we_white in (True, False):
                sc, moves, evals = play_vs_sf(d, values, op, we_white, skill,
                                              movetime)
                total += sc
                wins += sc == 1.0
                losses += sc == 0.0
                n += 1
                if handle:
                    res = ("1/2-1/2" if sc == 0.5 else
                           ("1-0" if (sc == 1.0) == we_white else "0-1"))
                    # `outcome` is TEXT in chessbb_games ("win"/"draw"/"loss"),
                    # not a number — passing the float 400s.
                    upload_game(handle, skill, {
                        "round": n, "our_color": "white" if we_white else "black",
                        "result": res,
                        "outcome": "draw" if sc == 0.5 else
                                   ("win" if sc == 1.0 else "loss"),
                        # match.py stores fullmove_number here, not plies —
                        # keep the column meaning one thing.
                        "moves": (len(moves) + 1) // 2, "pgn": tag,
                        "uci": " ".join(moves), "evals": json.dumps(evals),
                    })
                if sc == 0.0:
                    # Shape find_blunder expects (see blunder_mine.py).
                    rows.append({
                        "our_color": "white" if we_white else "black",
                        # This branch only runs on a loss, so the winner is
                        # always the other side.
                        "result": "0-1" if we_white else "1-0",
                        "uci": " ".join(moves),
                        "evals": json.dumps(evals),
                        "sf_skill": skill,
                    })
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return total, int(wins), int(losses), rows


def write_summary(args, summary):
    """Rewritten after every round, so an interrupted or offline run still
    leaves a readable record on disk."""
    path = os.path.join(ROOT, "data", "lossloop_summary.json")
    try:
        with open(path, "w") as f:
            json.dump(summary, f, indent=1)
    except OSError as e:
        print(f"  (summary write failed: {e})", file=sys.stderr)


def mine_turning_points(rows, min_drop):
    fens = []
    for r in rows:
        b = find_blunder(r)
        if b and b["drop"] >= min_drop:
            fens.append(b["fen"])
    return fens


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skill", type=int, default=5)
    ap.add_argument("--games", type=int, default=10,
                    help="games per cycle (openings = games/2, each both colours)")
    ap.add_argument("--target", type=int, default=10,
                    help="wins needed to stop. NOTE 10/10 against a skill-5 "
                         "Stockfish may simply not be reachable; --max-rounds "
                         "is what guarantees termination")
    ap.add_argument("--max-rounds", type=int, default=6)
    ap.add_argument("--movetime", type=float, default=0.2)
    ap.add_argument("--spsa-iters", type=int, default=6,
                    help="SPSA iterations per round, seeded from the losses")
    ap.add_argument("--depth", type=int, default=5, help="depth for SPSA self-play")
    ap.add_argument("--min-drop", type=int, default=150)
    ap.add_argument("--start", default="data/strategy_weights_nudged.txt")
    ap.add_argument("--values", default=ab_match.BASELINE_WEIGHTS)
    ap.add_argument("--out", default="data/strategy_weights_lossloop.txt")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-upload", action="store_true",
                    help="run offline. Uploading is the DEFAULT here (unlike "
                         "match.py) because the whole failure mode this fixes "
                         "is results existing only in terminal scrollback")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    allops = ab_match.load_openings(0)
    n_ops = max(1, args.games // 2)
    cycle_ops = allops[:n_ops]                 # tuned on
    heldout_ops = allops[n_ops:2 * n_ops]      # never tuned on
    if not heldout_ops:
        heldout_ops = allops[:n_ops]
        print("!! not enough openings for a disjoint held-out set; "
              "the held-out column is NOT independent this run")

    start_path = args.start if os.path.isabs(args.start) else os.path.join(ROOT, args.start)
    live = S.live_strategies(args.values)
    theta = S.normalise(S.load_weights(start_path), live)

    print(f"loss loop: skill {args.skill}, {args.games} games/cycle "
          f"({n_ops} openings x 2 colours), target {args.target}/{args.games}, "
          f"max {args.max_rounds} rounds")
    print(f"  tuning on openings 0-{n_ops-1}, held out {n_ops}-{2*n_ops-1}")
    print("  held-out is the column that matters; cycle score alone is a "
          "training score\n")

    run_id = uuid.uuid4().hex[:12]
    summary = {"run_id": run_id, "skill": args.skill, "games": 2 * n_ops,
               "movetime": args.movetime, "rounds": []}
    if not args.no_upload:
        print(f"run {run_id}: logging games and rounds to Supabase")

    best = None
    for rnd in range(1, args.max_rounds + 1):
        handle = (None if args.no_upload
                  else open_match(args.skill, None, args.movetime))
        total, wins, losses, rows = run_cycle(
            theta, args.values, cycle_ops, args.skill, args.movetime, live,
            handle, f"lossloop {run_id} round {rnd} cycle")
        # Held-out games are NOT uploaded as match games: they are a measuring
        # instrument, not training data, and mixing them into the corpus would
        # quietly contaminate every later blunder-mine and fit.
        ho, hw, hl, _ = run_cycle(theta, args.values, heldout_ops, args.skill,
                                  args.movetime, live)
        n = 2 * n_ops
        if handle:
            close_match(handle, wins, int(total - wins - losses + 0.5), losses)
        print(f"round {rnd}: cycle {total:.1f}/{n} ({100*total/n:.0f}%) "
              f"{wins}W {losses}L   |   held-out {ho:.1f}/{n} "
              f"({100*ho/n:.0f}%)", flush=True)

        if best is None or ho > best[0]:
            best = (ho, list(theta), rnd)

        # Mine before logging so the round row carries the turning-point count,
        # and log before any break/continue so no round goes unrecorded —
        # including the round that hits the target, which is the interesting one.
        fens = mine_turning_points(rows, args.min_drop) if rows else []
        summary["rounds"].append({
            "round": rnd, "cycle": total, "heldout": ho, "games": n,
            "wins": wins, "losses": losses, "turning_points": len(fens),
        })
        if not args.no_upload:
            log_round(run_id, rnd, args, n, total, ho, wins, losses, len(fens))
        write_summary(args, summary)

        if wins >= args.target:
            print(f"  target reached ({wins} wins).")
            break
        if not rows:
            print("  no losses to learn from this round.")
            continue
        if not fens:
            print(f"  {len(rows)} losses but none with a swing >= "
                  f"{args.min_drop}cp — nothing to seed SPSA with.")
            continue
        print(f"  {len(rows)} losses -> {len(fens)} turning-point positions; "
              f"{args.spsa_iters} SPSA iters seeded from them", flush=True)

        # Paired perturbation FROM the positions that lost the games: the same
        # position played with theta+ and theta-, which is what converts "we
        # lost" into a direction rather than just a penalty.
        for k in range(1, args.spsa_iters + 1):
            ck, ak = 0.08 / (k ** 0.101), 0.15 / (k ** 0.602)
            delta = [rng.choice([-1.0, 1.0]) if live[i] else 0.0
                     for i in range(len(theta))]
            plus = S.normalise([t * (1 + ck * d) for t, d in zip(theta, delta)], live)
            minus = S.normalise([t * (1 - ck * d) for t, d in zip(theta, delta)], live)
            ops = fens[:8] if len(fens) > 8 else fens
            sc = S.match(plus, minus, ops, args.values, args.depth)
            g = sc - 0.5
            theta = S.normalise(
                [t * (1 + ak * g * 2.0 * d) for t, d in zip(theta, delta)], live)
            print(f"    spsa {k}: +side {100*sc:.0f}%  step {ak*g*2:+.3f}",
                  flush=True)

    out = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    # Keep the round that was best on HELD-OUT, not the one best on the cycle —
    # the cycle score is what we tuned against and is not evidence.
    S.write_weights(best[1], out)
    if queued_count():
        print(f"\n!! {queued_count()} row(s) could not be uploaded and are "
              f"queued in data/pending_uploads.jsonl — they go up on the next "
              f"run with a working connection")
    print(f"\nbest held-out was round {best[2]} ({100*best[0]/(2*n_ops):.0f}%)"
          f" -> {out}")
    print("This is NOT adopted. Gate it first:")
    print(f"  python3 lab/strat_ab.py {os.path.relpath(out, ROOT)} "
          f"{args.start} --depth 6")


if __name__ == "__main__":
    main()
