#!/usr/bin/env python3
"""Continuous training loop: grow the game log against max-skill Stockfish.

Each iteration plays a two-game mini-match (one as White, one as Black),
appends both games to the permanent log, and updates the strategy weights
from each outcome. Every RETUNE_EVERY games the quiet-position dataset is
rebuilt from the full log and the Texel tuner relearns every square and
piece value — logged once, reused on every future startup.

Usage:
  python3 tools/training_loop.py [iterations]     # default 10 (= 20 games)
"""
import os
import shutil
import subprocess
import sys

import chess.pgn

ITERATIONS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
RETUNE_EVERY = 10          # games between square/piece value retunes
MOVETIME = "0.1"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SF = shutil.which("stockfish") or "/usr/games/stockfish"

OUTCOME = {
    ("1-0", True): 1.0, ("1-0", False): 0.0,
    ("0-1", True): 0.0, ("0-1", False): 1.0,
}

games_done = 0
score = 0.0

# Bootstrap: the engine plays every game with its current learned square/
# piece values (zero human priors at the start), and each retune below
# starts descent from those same values — so the tables it plays with are
# the tables it worked out from its own games.
BOOT_ENV = dict(os.environ, CHESS_WEIGHTS="data/learned_values.txt")

for it in range(1, ITERATIONS + 1):
    print(f"=== training iteration {it}/{ITERATIONS} ===", flush=True)
    subprocess.run(
        ["python3", "tools/match.py", "--engine", "./chess",
         "--stockfish", SF, "--games", "2", "--movetime", MOVETIME,
         "--skill", "20", "--pgn", "/tmp/train_pair.pgn"],
        env=BOOT_ENV, check=False)

    with open("/tmp/train_pair.pgn") as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            result = game.headers.get("Result", "*")
            we_white = game.headers.get("White") == "chess-bb"
            outcome = OUTCOME.get((result, we_white), 0.5)
            games_done += 1
            score += outcome

            with open("data/games_log.pgn", "a") as log:
                print(game, file=log)
                print(file=log)

            with open("/tmp/train_one.pgn", "w") as one:
                print(game, file=one)
            # expected 0.05: losses to max-skill Stockfish are priced in and
            # teach little; draws and wins are the surprises worth learning
            subprocess.run(
                ["python3", "tools/analyze_game.py", "/tmp/train_one.pgn",
                 "./chess", "strategy_weights.txt", str(outcome), "0.05"],
                check=False)
            print(f"game {games_done}: {result} as "
                  f"{'white' if we_white else 'black'} -> outcome {outcome}",
                  flush=True)

    # Bleed metric: cp lost per move by Stockfish's judgement — the metric
    # that shows learning long before the first earned draw.
    subprocess.run(["python3", "tools/bleed_metric.py", "/tmp/train_pair.pgn",
                    "10"], check=False)

    if games_done and games_done % RETUNE_EVERY == 0:
        print(f"--- retuning square/piece values on {games_done} new games ---",
              flush=True)
        env = dict(os.environ, DATASET_LOCAL_ONLY="1")
        subprocess.run(
            ["python3", "tools/make_dataset.py", "data/texel_dataset.txt",
             "data/games_log.pgn"],
            env=env, check=False)
        with open("data/learned_values.txt.new", "w") as out, \
             open("/tmp/retune.log", "w") as err:
            subprocess.run(["./chess", "tune", "data/texel_dataset.txt"],
                           stdout=out, stderr=err, env=BOOT_ENV, check=False)
        os.replace("data/learned_values.txt.new", "data/learned_values.txt")

        # Refit the strategy calibration from all logged outcomes — the
        # optimal pre-game weights, not the running average of nudges.
        subprocess.run(["python3", "tools/fit_strategy_weights.py", "--apply"],
                       env=BOOT_ENV, check=False)

print(f"\ntraining done: {score}/{games_done} "
      f"({100 * score / max(games_done, 1):.0f}%) vs Stockfish skill 20",
      flush=True)
