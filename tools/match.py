#!/usr/bin/env python3
"""Match runner: pit chess-bb against Stockfish over UCI.

Examples:
    python3 tools/match.py --games 10 --movetime 0.1 --skill 0
    python3 tools/match.py --games 2  --movetime 0.1              # full strength
    python3 tools/match.py --games 10 --movetime 0.1 --elo 1400

With --upload, results are stored in Supabase (chessbb_matches/chessbb_games).
Requires SUPABASE_URL and SUPABASE_KEY env vars (service role key, since the
tables are RLS-protected and only publicly readable).
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import urllib.request

import chess
import chess.engine
import chess.pgn


def supabase_insert(base_url, key, table, rows, return_repr=False):
    req = urllib.request.Request(
        f"{base_url}/rest/v1/{table}",
        data=json.dumps(rows).encode(),
        headers={
            "apikey": key,
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
            "Prefer": "return=representation" if return_repr else "return=minimal",
        },
        method="POST",
    )
    with urllib.request.urlopen(req) as resp:
        body = resp.read()
    return json.loads(body) if return_repr else None


def upload_match(sf_skill, sf_elo, movetime, wins, draws, losses, games):
    base_url = os.environ.get("SUPABASE_URL")
    key = os.environ.get("SUPABASE_KEY")
    if not base_url or not key:
        print("upload skipped: set SUPABASE_URL and SUPABASE_KEY", file=sys.stderr)
        return
    try:
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or None
    except OSError:
        sha = None

    match = supabase_insert(base_url.rstrip("/"), key, "chessbb_matches", [{
        "opponent": "Stockfish 16",
        "sf_skill": sf_skill,
        "sf_elo": sf_elo,
        "movetime_ms": int(movetime * 1000),
        "engine_sha": sha,
        "wins": wins, "draws": draws, "losses": losses,
    }], return_repr=True)
    match_id = match[0]["id"]

    supabase_insert(base_url.rstrip("/"), key, "chessbb_games", [{
        "match_id": match_id,
        "round": g["round"],
        "our_color": g["our_color"],
        "result": g["result"],
        "outcome": g["outcome"],
        "moves": g["moves"],
        "pgn": g["pgn"],
    } for g in games])
    print(f"uploaded match {match_id} ({len(games)} games) to Supabase")


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
    ap.add_argument("--upload", action="store_true",
                    help="store results in Supabase (needs SUPABASE_URL/SUPABASE_KEY)")
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
    played = []
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
                outcome = "win"
            else:
                losses += 1
                outcome = "loss"

            print(f"game {g+1}: chess-bb as {'white' if we_are_white else 'black'} "
                  f"vs {sf_desc}: {result} "
                  f"({outcome.upper() if outcome == 'win' else outcome}, "
                  f"{board.fullmove_number} moves)")
            sys.stdout.flush()

            game = chess.pgn.Game.from_board(board)
            game.headers["Event"] = f"chess-bb vs {sf_desc}"
            game.headers["Date"] = datetime.date.today().strftime("%Y.%m.%d")
            game.headers["White"] = "chess-bb" if we_are_white else sf_desc
            game.headers["Black"] = sf_desc if we_are_white else "chess-bb"
            game.headers["Round"] = str(g + 1)
            game.headers["Result"] = result

            played.append({
                "round": g + 1,
                "our_color": "white" if we_are_white else "black",
                "result": result if result != "*" else "1/2-1/2",
                "outcome": outcome,
                "moves": board.fullmove_number,
                "pgn": str(game),
            })
            if pgn_out:
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

    if args.upload:
        upload_match(args.skill, args.elo, args.movetime,
                     wins, draws, losses, played)


if __name__ == "__main__":
    main()
