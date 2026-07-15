#!/usr/bin/env python3
"""Minimal Lichess bot bridge for chess-bb.

Setup (one time):
  1. Create a NEW lichess account (it must have zero games played).
  2. Create an API token with the bot:play scope:
       https://lichess.org/account/oauth/token/create?scopes[]=bot:play
  3. Upgrade the account to a BOT account (irreversible for that account):
       curl -d '' https://lichess.org/api/bot/account/upgrade \
            -H "Authorization: Bearer <token>"

Run:
  LICHESS_TOKEN=<token> python3 tools/lichess_bot.py [--engine ./chess]

It accepts standard-chess challenges, plays with the engine over UCI, and
logs every finished game to Supabase (chessbb_matches/chessbb_games) using
supabase.env, building the same training-data set as tools/match.py.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import threading
import urllib.request

import chess
import chess.engine
import chess.pgn

from match import load_supabase_env, supabase_insert

API = "https://lichess.org"
TOKEN = os.environ.get("LICHESS_TOKEN")


def req(path, data=None, stream=False):
    r = urllib.request.Request(
        API + path,
        data=data if data is not None else None,
        headers={"Authorization": f"Bearer {TOKEN}"},
        method="POST" if data is not None else "GET",
    )
    resp = urllib.request.urlopen(r)
    return resp if stream else resp.read()


def ndjson_lines(resp):
    for raw in resp:
        line = raw.strip()
        if line:
            yield json.loads(line)


def engine_sha():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip() or None
    except OSError:
        return None


def upload_game(board, our_color, opponent, result):
    conf = load_supabase_env()
    base_url = os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL")
    key = os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY")
    if not base_url or not key:
        return
    if result == "1/2-1/2":
        w, d, l = 0, 1, 0
        outcome = "draw"
    elif (result == "1-0") == (our_color == "white"):
        w, d, l = 1, 0, 0
        outcome = "win"
    else:
        w, d, l = 0, 0, 1
        outcome = "loss"

    game = chess.pgn.Game.from_board(board)
    game.headers["Event"] = "lichess"
    game.headers["Date"] = datetime.date.today().strftime("%Y.%m.%d")
    game.headers["White"] = "chess-bb" if our_color == "white" else opponent
    game.headers["Black"] = opponent if our_color == "white" else "chess-bb"
    game.headers["Result"] = result

    try:
        match = supabase_insert(base_url.rstrip("/"), key, "chessbb_matches", [{
            "opponent": f"lichess:{opponent}",
            "movetime_ms": 0,
            "engine_sha": engine_sha(),
            "wins": w, "draws": d, "losses": l,
        }], return_repr=True)
        supabase_insert(base_url.rstrip("/"), key, "chessbb_games", [{
            "match_id": match[0]["id"],
            "round": 1,
            "our_color": our_color,
            "result": result if result != "*" else "1/2-1/2",
            "outcome": outcome,
            "moves": board.fullmove_number,
            "pgn": str(game),
        }])
        print(f"logged game vs {opponent} to Supabase ({outcome})")
    except Exception as e:  # logging must never kill a live game thread
        print(f"supabase upload failed: {e}", file=sys.stderr)


def play_game(game_id, engine_path, my_id):
    eng = chess.engine.SimpleEngine.popen_uci(engine_path)
    board = chess.Board()
    our_color, opponent = None, "?"
    try:
        stream = req(f"/api/bot/game/stream/{game_id}", stream=True)
        for ev in ndjson_lines(stream):
            if ev["type"] == "gameFull":
                white_id = ev["white"].get("id", "")
                our_color = "white" if white_id == my_id else "black"
                opp = ev["black"] if our_color == "white" else ev["white"]
                opponent = opp.get("name", opp.get("id", "?"))
                if ev.get("initialFen", "startpos") != "startpos":
                    board = chess.Board(ev["initialFen"])
                state = ev["state"]
            elif ev["type"] == "gameState":
                state = ev
            else:
                continue

            board = chess.Board()
            for mv in state["moves"].split():
                board.push_uci(mv)

            if state["status"] != "started":
                result = board.result(claim_draw=True)
                if result == "*":  # resign/timeout: board alone can't tell
                    result = ("1-0" if state.get("winner") == "white"
                              else "0-1" if state.get("winner") == "black"
                              else "1/2-1/2")
                upload_game(board, our_color, opponent, result)
                break

            our_turn = (board.turn == chess.WHITE) == (our_color == "white")
            if our_turn and not board.is_game_over(claim_draw=True):
                limit = chess.engine.Limit(
                    white_clock=state["wtime"] / 1000,
                    black_clock=state["btime"] / 1000,
                    white_inc=state["winc"] / 1000,
                    black_inc=state["binc"] / 1000,
                )
                result = eng.play(board, limit)
                req(f"/api/bot/game/{game_id}/move/{result.move.uci()}", data=b"")
    except Exception as e:
        print(f"game {game_id}: {e}", file=sys.stderr)
    finally:
        eng.quit()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="./chess")
    args = ap.parse_args()

    if not TOKEN:
        sys.exit("set LICHESS_TOKEN (see the docstring for setup steps)")

    me = json.loads(req("/api/account"))
    my_id = me["id"]
    print(f"online as {me['username']} (bot={me.get('title') == 'BOT'})")

    while True:  # reconnect loop: lichess drops idle streams periodically
        try:
            events = req("/api/stream/event", stream=True)
            for ev in ndjson_lines(events):
                if ev["type"] == "challenge":
                    ch = ev["challenge"]
                    ok = (ch["variant"]["key"] == "standard"
                          and ch["speed"] != "correspondence")
                    verb = "accept" if ok else "decline"
                    try:
                        req(f"/api/challenge/{ch['id']}/{verb}", data=b"")
                        print(f"{verb}ed challenge from {ch['challenger']['name']}")
                    except Exception as e:
                        print(f"challenge {ch['id']}: {e}", file=sys.stderr)
                elif ev["type"] == "gameStart":
                    gid = ev["game"]["gameId"]
                    threading.Thread(target=play_game,
                                     args=(gid, args.engine, my_id),
                                     daemon=True).start()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"event stream dropped ({e}), reconnecting...", file=sys.stderr)


if __name__ == "__main__":
    main()
