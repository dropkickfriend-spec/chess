#!/usr/bin/env python3
"""Ladder training: climb Stockfish skill levels by winning.

Instead of getting flattened by max Stockfish forever (zero reward
variance, learning crawls), the engine plays its CURRENT rung — starting
at Skill Level 0 — where wins and losses both happen, so every game
carries signal. Expected score at the rung is 0.5: wins and losses teach
symmetrically. Score >= 60 percent over the last 10 rung games promotes
to the next skill level. Progress is wins on the board, and each
promotion is a measured Elo step.

Everything else stays the standard loop: games append to the permanent
log and upload live (per-move streaming included), strategy weights
nudge per game, bleed/ACPL logged per match, and every third iteration
the dataset rebuilds, the staged tuner refits values, and the strategy
calibration refits.

State persists in data/ladder.json (rung, rolling window, promotions).

Usage:
  python3 tools/ladder_loop.py [iterations]   # default 10 (x4 games)
"""
import json
import os
import tempfile

TMP_DIR = tempfile.gettempdir()
import shutil
import subprocess
import sys

import chess.pgn

ITERATIONS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
GAMES_PER_MATCH = 4
PROMOTE_WINDOW = 10
PROMOTE_SCORE = 6.0          # 60% of the window
MOVETIME = "0.1"
LADDER = "data/ladder.json"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SF = shutil.which("stockfish") or "/usr/games/stockfish"

BOOT_ENV = dict(os.environ, CHESS_WEIGHTS="data/learned_values.txt",
                CHESS_BOOK="data/route_book.txt")

OUTCOME = {
    ("1-0", True): 1.0, ("1-0", False): 0.0,
    ("0-1", True): 0.0, ("0-1", False): 1.0,
}


def load_state():
    try:
        with open(LADDER) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"skill": 0, "window": [], "games_total": 0, "promotions": []}


def save_state(st):
    with open(LADDER, "w") as f:
        json.dump(st, f, indent=1)


st = load_state()

# Start from the latest Supabase brain so every machine plays the same weights.
subprocess.run(["python3", "tools/sync_weights.py"], check=False)

for it in range(1, ITERATIONS + 1):
    skill = st["skill"]
    print(f"=== ladder iteration {it}/{ITERATIONS} — rung: SF skill {skill} ===",
          flush=True)
    subprocess.run(
        ["python3", "tools/match.py", "--engine", "./chess",
         "--stockfish", SF, "--games", str(GAMES_PER_MATCH),
         "--movetime", MOVETIME, "--skill", str(skill),
         "--pgn", f"{TMP_DIR}/ladder_match.pgn", "--upload"],
        env=BOOT_ENV, check=False)

    with open(f"{TMP_DIR}/ladder_match.pgn") as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            res = game.headers.get("Result", "*")
            we_white = game.headers.get("White") == "chess-bb"
            oc = OUTCOME.get((res, we_white), 0.5)
            st["window"] = (st["window"] + [oc])[-PROMOTE_WINDOW:]
            st["games_total"] += 1

            with open("data/games_log.pgn", "a") as log:
                print(game, file=log)
                print(file=log)
            with open(f"{TMP_DIR}/ladder_one.pgn", "w") as one:
                print(game, file=one)
            # expected 0.5: the rung IS our level until we out-earn it,
            # so wins and losses teach with equal force
            subprocess.run(
                ["python3", "tools/analyze_game.py", f"{TMP_DIR}/ladder_one.pgn",
                 "./chess", "strategy_weights.txt", str(oc), "0.5"],
                check=False)
            print(f"game {st['games_total']}: {res} as "
                  f"{'white' if we_white else 'black'} -> {oc} "
                  f"(rung window {sum(st['window']):.1f}/{len(st['window'])})",
                  flush=True)

    if len(st["window"]) >= PROMOTE_WINDOW and sum(st["window"]) >= PROMOTE_SCORE:
        st["promotions"].append({"from_skill": st["skill"],
                                 "at_game": st["games_total"]})
        st["skill"] = min(st["skill"] + 1, 20)
        st["window"] = []
        print(f"*** PROMOTED to SF skill {st['skill']} "
              f"(promotion #{len(st['promotions'])}) ***", flush=True)
    save_state(st)

    subprocess.run(["python3", "tools/bleed_metric.py",
                    f"{TMP_DIR}/ladder_match.pgn", "10"], check=False)

    if it % 3 == 0:
        print("--- checkpoint: dataset + staged tune + calibration fit ---",
              flush=True)
        env = dict(os.environ, DATASET_LOCAL_ONLY="1")
        subprocess.run(["python3", "tools/make_dataset.py",
                        "data/texel_dataset.txt", "data/games_log.pgn"],
                       env=env, check=False)
        with open("data/learned_values.txt.new", "w") as out, \
             open(f"{TMP_DIR}/retune.log", "w") as err:
            subprocess.run(["./chess", "tune", "data/texel_dataset.txt"],
                           stdout=out, stderr=err, env=BOOT_ENV, check=False)
        os.replace("data/learned_values.txt.new", "data/learned_values.txt")
        subprocess.run(["python3", "tools/fit_strategy_weights.py", "--apply"],
                       env=BOOT_ENV, check=False)
        subprocess.run(["python3", "tools/upload_learned.py"], check=False)

print(f"\nladder run done: rung SF skill {st['skill']}, "
      f"{st['games_total']} ladder games, "
      f"{len(st['promotions'])} promotions", flush=True)
