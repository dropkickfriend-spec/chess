#!/usr/bin/env python3
"""Match runner: pit chess-bb against Stockfish over UCI.

Examples:
    python3 tools/match.py --games 10 --movetime 0.1 --skill 0
    python3 tools/match.py --games 2  --movetime 0.1              # full strength
    python3 tools/match.py --games 10 --movetime 0.1 --elo 1400
"""
import argparse
import datetime
import sys

import chess
import chess.engine
import chess.pgn


def play_game(white, black, movetime, max_plies=600):
    board = chess.Board()
    while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
        eng = white if board.turn == chess.WHITE else black
        result = eng.play(board, chess.engine.Limit(time=movetime))
        board.push(result.move)
    return board


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="./chess")
    ap.add_argument("--stockfish", default="/usr/games/stockfish")
    ap.add_argument("--games", type=int, default=10)
    ap.add_argument("--movetime", type=float, default=0.1, help="seconds per move")
    ap.add_argument("--skill", type=int, default=None, help="Stockfish Skill Level 0-20")
    ap.add_argument("--elo", type=int, default=None, help="Stockfish UCI_Elo (min 1320)")
    ap.add_argument("--pgn", default=None, help="write games to this PGN file")
    args = ap.parse_args()

    ours = chess.engine.SimpleEngine.popen_uci(args.engine)
    sf = chess.engine.SimpleEngine.popen_uci(args.stockfish)
    sf_desc = "Stockfish"
    if args.skill is not None:
        sf.configure({"Skill Level": args.skill})
        sf_desc += f" (skill {args.skill})"
    if args.elo is not None:
        sf.configure({"UCI_LimitStrength": True, "UCI_Elo": args.elo})
        sf_desc += f" (elo {args.elo})"

    pgn_out = open(args.pgn, "w") if args.pgn else None
    wins = draws = losses = 0
    try:
        for g in range(args.games):
            we_are_white = g % 2 == 0
            white, black = (ours, sf) if we_are_white else (sf, ours)
            board = play_game(white, black, args.movetime)
            result = board.result(claim_draw=True)

            if result == "1/2-1/2" or result == "*":
                draws += 1
                outcome = "draw"
            elif (result == "1-0") == we_are_white:
                wins += 1
                outcome = "WIN"
            else:
                losses += 1
                outcome = "loss"

            print(f"game {g+1}: chess-bb as {'white' if we_are_white else 'black'} "
                  f"vs {sf_desc}: {result} ({outcome}, {board.fullmove_number} moves)")
            sys.stdout.flush()

            if pgn_out:
                game = chess.pgn.Game.from_board(board)
                game.headers["Event"] = f"chess-bb vs {sf_desc}"
                game.headers["Date"] = datetime.date.today().strftime("%Y.%m.%d")
                game.headers["White"] = "chess-bb" if we_are_white else sf_desc
                game.headers["Black"] = sf_desc if we_are_white else "chess-bb"
                game.headers["Round"] = str(g + 1)
                game.headers["Result"] = result
                print(game, file=pgn_out, flush=True)
                print(file=pgn_out)
    finally:
        ours.quit()
        sf.quit()
        if pgn_out:
            pgn_out.close()

    n = wins + draws + losses
    score = wins + draws / 2
    print(f"\nchess-bb vs {sf_desc}: +{wins} ={draws} -{losses}  "
          f"({score}/{n} = {100*score/max(n,1):.0f}%)")


if __name__ == "__main__":
    main()
