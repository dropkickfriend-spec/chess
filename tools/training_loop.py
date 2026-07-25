#!/usr/bin/env python3
"""Self-play training: grow the game log with games that actually teach.

WHY THIS WAS REWRITTEN. The previous version played two games per iteration
against Stockfish skill 20 and lost 0/2 every single time, by construction --
no hand-crafted eval beats max-strength Stockfish. That was worse than useless:

  * Every position from those blowouts entered the permanent training corpus
    labelled "loss". The Texel tuner learns piece/square values by fitting
    position -> game result, so a corpus where every label is 0.0 carries almost
    no signal, and actively teaches that positions the engine was still fine in
    were already lost.
  * The per-game weight nudge took a full-strength penalty for games that were
    unwinnable, so the strategy mix got blamed for the opponent's strength.

Now the engine plays ITSELF from varied book openings. Self-play is balanced by
definition, so results split roughly evenly and the corpus gets positions
labelled with outcomes that genuinely discriminate good play from bad -- which
is exactly what the tuner needs.

Opening variety is REQUIRED, not decoration: the engine is a deterministic
function of (position, weights), so self-play from the start position would
replay one identical game forever. Openings come from data/opening_lines.txt,
truncated at several depths, cycled so every iteration starts somewhere new.

The per-game strategy-weight nudge is deliberately skipped: both sides use the
same weights, so "which strategy mix won" is meaningless in self-play. Strategy
calibration comes from the retune's logistic fit over the whole corpus instead.
Self-play games are logged locally (they feed the dataset) but NOT uploaded as
match results -- they are not games against Stockfish and would corrupt the
dashboard's per-level win/loss stats.

Usage:
  python3 tools/training_loop.py [games]      # default 10
"""
import os
import subprocess
import sys
import tempfile

import chess
import chess.engine
import chess.pgn

TMP_DIR = tempfile.gettempdir()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

GAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 10
RETUNE_EVERY = 10          # games between square/piece value retunes
MOVETIME = float(os.environ.get("SELFPLAY_MOVETIME", "0.1"))
MAX_PLIES = 300

# The engine plays with its own learned values, and the retune below starts
# descent from those same values — the tables it plays with are the tables it
# worked out from its own games.
BOOT_ENV = dict(os.environ, CHESS_WEIGHTS="data/learned_values.txt",
                CHESS_BOOK="data/route_book.txt")

LINES_FILE = os.path.join(ROOT, "data", "opening_lines.txt")


def load_openings():
    """Book lines truncated at several depths — distinct, balanced starts."""
    ops = set()
    try:
        with open(LINES_FILE) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                mv = line.split()
                for ply in (4, 6, 8):
                    if len(mv) >= ply:
                        ops.add(" ".join(mv[:ply]))
    except FileNotFoundError:
        pass
    return sorted(ops) or ["e2e4 e7e5", "d2d4 d7d5", "c2c4 e7e5", "g1f3 d7d5"]


def play_self_game(opening):
    """One engine instance plays both sides from `opening`. Returns the board."""
    board = chess.Board()
    for mv in opening.split():
        board.push_uci(mv)
    eng = chess.engine.SimpleEngine.popen_uci("./chess", env=BOOT_ENV)
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < MAX_PLIES:
            res = eng.play(board, chess.engine.Limit(time=MOVETIME))
            if res.move is None:
                break
            board.push(res.move)
    finally:
        eng.quit()
    return board


def to_pgn(board, opening, n):
    game = chess.pgn.Game.from_board(board)
    game.headers["Event"] = "chess-bb self-play training"
    game.headers["White"] = "chess-bb"
    game.headers["Black"] = "chess-bb"
    game.headers["Round"] = str(n)
    game.headers["Opening"] = opening
    game.headers["Result"] = board.result(claim_draw=True)
    return game


def retune():
    print(f"--- retuning square/piece values on the full corpus ---", flush=True)
    subprocess.run(["python3", "tools/make_dataset.py",
                    "data/texel_dataset.txt", "data/games_log.pgn"],
                   env=dict(os.environ), check=False)
    with open("data/learned_values.txt.new", "w") as out:
        # stderr inherits the terminal so the tuner's per-pass progress is
        # visible; burying it made the step look frozen and got it Ctrl-C'd.
        subprocess.run(["./chess", "tune", "data/texel_dataset.txt"],
                       stdout=out, env=BOOT_ENV, check=False)
    if os.path.getsize("data/learned_values.txt.new") > 0:
        os.replace("data/learned_values.txt.new", "data/learned_values.txt")
        subprocess.run(["python3", "tools/fit_strategy_weights.py", "--apply"],
                       env=BOOT_ENV, check=False)
    else:
        os.remove("data/learned_values.txt.new")
        print("tune produced nothing (interrupted?) — keeping previous values",
              flush=True)


openings = load_openings()
print(f"self-play training: {GAMES} games, {len(openings)} distinct openings, "
      f"{MOVETIME}s/move", flush=True)

w = d = b = 0
for n in range(1, GAMES + 1):
    opening = openings[(n - 1) % len(openings)]
    board = play_self_game(opening)
    result = board.result(claim_draw=True)
    w += result == "1-0"
    b += result == "0-1"
    d += result == "1/2-1/2"

    with open("data/games_log.pgn", "a") as log:
        print(to_pgn(board, opening, n), file=log)
        print(file=log)

    print(f"game {n}/{GAMES}: {result} in {board.ply()} plies  [{opening}]",
          flush=True)

    if n % RETUNE_EVERY == 0:
        retune()

print(f"\nself-play done: {w} white wins, {b} black wins, {d} draws "
      f"({GAMES} games added to the training corpus)", flush=True)
