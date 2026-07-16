#!/usr/bin/env python3
"""Bleed metric: how fast chess-bb hemorrhages eval against Stockfish.

Wins/losses can't measure progress while every game vs max Stockfish is a
loss — but the RATE of loss can. For every chess-bb move we take Stockfish's
eval (our POV, clamped to +/-800 so dead-lost noise doesn't dominate) before
and after the move; the mean drop per move is the bleed, in cp/move.
A learning engine bleeds slower over time.

Appends one row per game to data/bleed_history.csv:
  timestamp,color,result,our_moves,bleed_cp_per_move

Usage:
  python3 tools/bleed_metric.py game.pgn [depth=12] [csv=data/bleed_history.csv]
"""
import datetime
import os
import sys

import chess
import chess.engine
import chess.pgn

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SF = "/usr/games/stockfish"
CLAMP = 800


def clamp(v):
    return max(-CLAMP, min(CLAMP, v))


def game_bleed(game, sf, depth):
    """Mean cp lost per chess-bb move, Stockfish's judgement."""
    we_white = game.headers.get("White") == "chess-bb"
    board = game.board()
    info = sf.analyse(board, chess.engine.Limit(depth=depth))
    prev = clamp(info["score"].pov(chess.WHITE if we_white else chess.BLACK)
                 .score(mate_score=10000))
    deltas = []
    for mv in game.mainline_moves():
        ours = board.turn == (chess.WHITE if we_white else chess.BLACK)
        board.push(mv)
        info = sf.analyse(board, chess.engine.Limit(depth=depth))
        cur = clamp(info["score"].pov(chess.WHITE if we_white else chess.BLACK)
                    .score(mate_score=10000))
        if ours:
            deltas.append(cur - prev)
        prev = cur
    if not deltas:
        return None, 0
    return -sum(deltas) / len(deltas), len(deltas)


def main():
    pgn_path = sys.argv[1]
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    csv_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        ROOT, "data", "bleed_history.csv")

    sf = chess.engine.SimpleEngine.popen_uci(SF)
    rows = []
    try:
        with open(pgn_path) as f:
            while True:
                game = chess.pgn.read_game(f)
                if game is None:
                    break
                if "chess-bb" not in (game.headers.get("White", ""),
                                      game.headers.get("Black", "")):
                    continue
                bleed, n = game_bleed(game, sf, depth)
                if bleed is None:
                    continue
                color = "white" if game.headers.get("White") == "chess-bb" else "black"
                rows.append((datetime.datetime.utcnow().isoformat(timespec="seconds"),
                             color, game.headers.get("Result", "*"), n,
                             round(bleed, 1)))
                print(f"game as {color}: {game.headers.get('Result')} — "
                      f"bleed {bleed:+.1f} cp/move over {n} moves")
    finally:
        sf.quit()

    if rows:
        new = not os.path.exists(csv_path)
        with open(csv_path, "a") as f:
            if new:
                f.write("timestamp,color,result,our_moves,bleed_cp_per_move\n")
            for r in rows:
                f.write(",".join(map(str, r)) + "\n")
        avg = sum(r[4] for r in rows) / len(rows)
        print(f"\n{len(rows)} games, mean bleed {avg:+.1f} cp/move "
              f"-> appended to {csv_path}")


if __name__ == "__main__":
    main()
