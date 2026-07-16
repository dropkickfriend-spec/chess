#!/usr/bin/env python3
"""Bleed metric: how fast chess-bb hemorrhages eval against Stockfish.

Reports TWO numbers per game and they mean different things:
- trend-bleed: eval drop per move with evals clamped to +/-800. ONLY valid
  for comparing runs against each other (the clamp stops dead-lost noise
  drowning the trend, but it hides how bad collapses really are).
- real ACPL: the same measurement UNCLAMPED - honest average centipawn
  loss, comparable to what chess sites report. This is the true strength
  number, and while the engine is young it is ugly. Never quote the
  clamped number as playing strength.

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
    pov = chess.WHITE if we_white else chess.BLACK
    info = sf.analyse(board, chess.engine.Limit(depth=depth))
    prev_raw = info["score"].pov(pov).score(mate_score=10000)
    prev = clamp(prev_raw)
    deltas, raw = [], []
    for mv in game.mainline_moves():
        ours = board.turn == pov
        board.push(mv)
        info = sf.analyse(board, chess.engine.Limit(depth=depth))
        cur_raw = info["score"].pov(pov).score(mate_score=10000)
        cur = clamp(cur_raw)
        if ours:
            deltas.append(cur - prev)
            raw.append(cur_raw - prev_raw)
        prev = cur
        prev_raw = cur_raw
    if not deltas:
        return None, None, 0
    return -sum(deltas) / len(deltas), -sum(raw) / len(raw), len(deltas)


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
                bleed, acpl, n = game_bleed(game, sf, depth)
                if bleed is None:
                    continue
                color = "white" if game.headers.get("White") == "chess-bb" else "black"
                rows.append((datetime.datetime.utcnow().isoformat(timespec="seconds"),
                             color, game.headers.get("Result", "*"), n,
                             round(bleed, 1), round(acpl, 1)))
                print(f"game as {color}: {game.headers.get('Result')} — "
                      f"trend-bleed {bleed:+.1f} (clamped), REAL ACPL {acpl:+.1f} cp/move over {n} moves")
    finally:
        sf.quit()

    if rows:
        new = not os.path.exists(csv_path)
        with open(csv_path, "a") as f:
            if new:
                f.write("timestamp,color,result,our_moves,bleed_cp_per_move,real_acpl\n")
            for r in rows:
                f.write(",".join(map(str, r)) + "\n")
        avg = sum(r[4] for r in rows) / len(rows)
        print(f"\n{len(rows)} games, mean bleed {avg:+.1f} cp/move "
              f"-> appended to {csv_path}")


if __name__ == "__main__":
    main()
