#!/usr/bin/env python3
"""Match runner: pit chess-bb against Stockfish over UCI.

Examples:
    python3 tools/match.py --games 10 --movetime 0.1 --skill 0
    python3 tools/match.py --games 2  --movetime 0.1              # full strength
    python3 tools/match.py --games 10 --movetime 0.1 --elo 1400

With --upload, results are stored in Supabase (chessbb_matches/chessbb_games).
Credentials come from SUPABASE_URL/SUPABASE_KEY env vars, falling back to the
supabase.env file at the repo root (committed anon key: append-only insert on
the chessbb tables plus public reads — nothing else).
"""
import argparse
import datetime
import json
import os
import tempfile

TMP_DIR = tempfile.gettempdir()
import shutil
import subprocess
import sys
import urllib.request

import chess
import chess.engine
import chess.pgn

# Works on desktop Linux (/usr/games) and Termux ($PREFIX/bin) alike
DEFAULT_STOCKFISH = (shutil.which("stockfish")
                     or "/usr/games/stockfish")

# Seconds between live-dashboard updates. The POST costs ~900 ms round trip, so
# on a slow link a tighter interval buys nothing but latency. LIVE_EVERY=0 with
# --no-live turns per-move streaming off entirely for bulk training runs, where
# nobody is watching and only the finished games matter.
LIVE_EVERY = float(os.environ.get("LIVE_EVERY", "2.0"))


def supabase_insert(base_url, key, table, rows, return_repr=False, upsert=False):
    prefer = "return=representation" if return_repr else "return=minimal"
    if upsert:
        prefer += ",resolution=merge-duplicates"
    req = urllib.request.Request(
        f"{base_url}/rest/v1/{table}",
        data=json.dumps(rows).encode(),
        headers={
            "apikey": key,
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
            "Prefer": prefer,
        },
        method="POST",
    )
    with urllib.request.urlopen(req) as resp:
        body = resp.read()
    return json.loads(body) if return_repr else None


def load_supabase_env():
    """supabase.env at the repo root; real env vars take precedence."""
    path = os.path.join(os.path.dirname(__file__), "..", "supabase.env")
    conf = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def _supabase_creds():
    conf = load_supabase_env()
    return (os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL"),
            os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY"))


def open_match(sf_skill, sf_elo, movetime):
    """Create the match row UP FRONT and return (base_url, key, match_id).

    Games used to be uploaded only after a whole match finished, so an
    interrupted run lost every completed game in it — a container restart cost
    7 finished games at skill 3. Opening the match first lets each game be
    written as it ends, so an interrupt costs at most the game in flight.
    Returns None if Supabase is not configured (caller then skips uploading).
    """
    base_url, key = _supabase_creds()
    if not base_url or not key:
        print("upload skipped: set SUPABASE_URL and SUPABASE_KEY "
              "(env or supabase.env)", file=sys.stderr)
        return None
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
        "wins": 0, "draws": 0, "losses": 0,
    }], return_repr=True)
    return base_url.rstrip("/"), key, match[0]["id"]


def upload_game(handle, sf_skill, g):
    """Write one finished game immediately. Never lets a network error end the
    run — a failed upload costs that game, not the batch."""
    if not handle:
        return
    base_url, key, match_id = handle
    try:
        supabase_insert(base_url, key, "chessbb_games", [{
            "match_id": match_id,
            "round": g["round"], "our_color": g["our_color"],
            "result": g["result"], "outcome": g["outcome"],
            "moves": g["moves"], "pgn": g["pgn"],
            "uci": g.get("uci"), "evals": g.get("evals"),
            "strat_series": g.get("strat_series"),
            "sf_skill": sf_skill,
        }])
    except Exception as e:
        print(f"  (game upload failed: {e})", file=sys.stderr)


def close_match(handle, wins, draws, losses):
    """Patch the final W/D/L onto the match row."""
    if not handle:
        return
    base_url, key, match_id = handle
    try:
        req = urllib.request.Request(
            f"{base_url}/rest/v1/chessbb_matches?id=eq.{match_id}",
            data=json.dumps({"wins": wins, "draws": draws,
                             "losses": losses}).encode(),
            headers={"apikey": key, "Authorization": f"Bearer {key}",
                     "Content-Type": "application/json",
                     "Prefer": "return=minimal"},
            method="PATCH")
        urllib.request.urlopen(req).read()
    except Exception as e:
        print(f"  (match summary update failed: {e})", file=sys.stderr)
    print(f"match {match_id}: +{wins} ={draws} -{losses} recorded in Supabase")


def upload_match(sf_skill, sf_elo, movetime, wins, draws, losses, games):
    """Batch upload for callers that already hold a finished game list."""
    handle = open_match(sf_skill, sf_elo, movetime)
    if not handle:
        return
    for g in games:
        upload_game(handle, sf_skill, g)
    close_match(handle, wins, draws, losses)


def pv_plan(mover, pv):
    """Split a principal variation's from/to squares by the side that plays
    each move (the engine's own game plan), matching plan_squares[w]/[b] in
    search.c. Returns {"w": [...], "b": [...]}; capped to the near future."""
    w, b = [], []
    side = mover                       # chess.WHITE / chess.BLACK
    for mv in (pv or [])[:8]:
        (w if side == chess.WHITE else b).extend((mv.from_square, mv.to_square))
        side = not side
    return {"w": w, "b": b}


def play_game(white, black, movetime, max_plies=1000, live_cb=None):
    board = chess.Board()
    evals = []
    plans = []                          # per-ply game-plan squares (engine PV)
    while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
        eng = white if board.turn == chess.WHITE else black
        mover = board.turn
        result = eng.play(board, chess.engine.Limit(time=movetime),
                          info=chess.engine.INFO_SCORE | chess.engine.INFO_PV)
        board.push(result.move)
        sc = result.info.get("score") if result.info else None
        evals.append(sc.white().score(mate_score=10000) if sc else None)
        pv = result.info.get("pv") if result.info else None
        plans.append(pv_plan(mover, pv) if pv else None)
        if live_cb:
            live_cb(board, evals, plans)
    return board, evals


STRAT_ORDER = ["DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
               "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE",
               "DEFENDER_LOGISTICS", "KING_ACTIVITY", "PAWN_PROMOTION",
               "OPPOSITION", "MATERIAL", "COORDINATION", "GAME_PLAN",
               "SQUARE_VALUE", "BLOCKADE", "RESTRICTION"]


def strat_contribs(engine_path, fen, env):
    """Per-strategy weighted contribution (raw x effective weight) for one
    position, via the engine's evalfens mode. Returns one int per strategy in
    STRAT_ORDER (all 17), or None on any failure."""
    try:
        p = subprocess.run([engine_path, "evalfens"], input=fen + "\n",
                           capture_output=True, text=True, env=env, timeout=5)
    except Exception:
        return None
    vals = {}
    for line in p.stdout.splitlines():
        f = line.split()
        if len(f) == 3:
            try:
                vals[f[0]] = round(int(f[1]) * float(f[2]))
            except ValueError:
                pass
    if len(vals) < len(STRAT_ORDER):
        return None
    return [vals.get(s, 0) for s in STRAT_ORDER]


def make_live_cb(base_url, key, our_color, engine_path=None, env=None, sf_skill=None):
    """Stream the in-progress game to the live_game row after each move
    (throttled to ~1/s) so dashboards update per move. On our moves also
    records the per-move strategy contribution (the weight chain re-priced
    each position). Never lets a network hiccup touch the game."""
    import datetime as _dt
    import threading as _th
    import time as _time
    last = [0.0]
    series = []
    inflight = _th.Lock()

    # Measured: the live-stream POST takes ~900 ms round trip while the engine
    # subprocess costs ~8 ms. Done inline once a second, that network wait was
    # eating roughly half the wall-clock of every --upload game (worse on mobile
    # data). So post in a background thread and, if one is still in flight, DROP
    # this update rather than queueing: a live view wants the latest position,
    # not a backlog of stale ones, and the game never waits on the network.
    def _post(payload):
        try:
            supabase_insert(base_url, key, "live_game", [payload], upsert=True)
        except Exception:
            pass
        finally:
            inflight.release()

    def cb(board, evals, plans=None):
        # Throttle to ~1/s. The per-move strategy breakdown spawns an engine
        # subprocess, so we only pay for it when we actually upload — keeps
        # games fast (especially on phones) while the live graph still moves.
        # (evals and plans are full per-ply lists, so the game plan overlay
        # and eval trace stay full-resolution regardless of the throttle.)
        over = board.is_game_over(claim_draw=True)
        now = _time.time()
        if now - last[0] < LIVE_EVERY and not over:
            return
        last[0] = now
        if engine_path:
            c = strat_contribs(engine_path, board.fen(), env)
            if c is not None:
                series.append(c)
        payload = {
            "id": 1,
            "our_color": our_color,
            "uci": " ".join(m.uci() for m in board.move_stack),
            "evals": json.dumps(evals),
            "strat_series": json.dumps(series),
            "plans": json.dumps(plans or []),
            "sf_skill": sf_skill,
            "ply": board.ply(),
            "updated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        }
        if over:
            # Final state: post inline so the finished game is never lost to a
            # dropped frame or a thread dying with the process.
            if inflight.acquire(timeout=5.0):
                _post(payload)
            return
        if not inflight.acquire(blocking=False):
            return                      # previous post still going — skip this one
        _th.Thread(target=_post, args=(payload,), daemon=True).start()

    cb.series = series
    return cb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="./chess")
    ap.add_argument("--stockfish", default=DEFAULT_STOCKFISH)
    ap.add_argument("--games", type=int, default=10)
    ap.add_argument("--movetime", type=float, default=0.1, help="seconds per move")
    ap.add_argument("--skill", type=int, default=None, help="Stockfish Skill Level 0-20")
    ap.add_argument("--elo", type=int, default=None, help="Stockfish UCI_Elo (min 1320)")
    ap.add_argument("--pgn", default=None, help="write games to this PGN file")
    ap.add_argument("--no-live", action="store_true",
                    help="skip per-move live streaming (bulk runs); finished "
                         "games are still uploaded with --upload")
    ap.add_argument("--upload", action="store_true",
                    help="store results in Supabase (needs SUPABASE_URL/SUPABASE_KEY)")
    ap.add_argument("--learn", action="store_true",
                    help="nudge strategy_weights.txt per game and append to "
                         "data/games_log.pgn (makes fixed-skill runs compound)")
    ap.add_argument("--retune", action="store_true",
                    help="after the batch, rebuild the dataset, staged-tune the "
                         "value tables, and refit the strategy calibration")
    ap.add_argument("--sync", action="store_true",
                    help="pull the latest weights from Supabase into the local "
                         "files before playing (Supabase = source of truth)")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Supabase-canonical brain: pull the latest weights into the files the
    # engine reads, so it never plays stale files or hardcoded defaults.
    if args.sync:
        subprocess.run(["python3", "tools/sync_weights.py"], cwd=root, check=False)

    pgn_out = open(args.pgn, "w") if args.pgn else None
    wins = draws = losses = 0
    played = []
    # Open the match row before playing so each finished game can be written
    # immediately (see open_match): an interrupted run keeps its finished games.
    match_handle = open_match(args.skill, args.elo, args.movetime) \
        if args.upload else None
    try:
        for g in range(args.games):
            # Create fresh engine instances for each game to avoid TT pollution
            ours = chess.engine.SimpleEngine.popen_uci(args.engine)
            sf = chess.engine.SimpleEngine.popen_uci(args.stockfish)
            sf_desc = "Stockfish"
            if args.skill is not None:
                sf.configure({"Skill Level": args.skill})
                sf_desc += f" (skill {args.skill})"
            if args.elo is not None:
                sf.configure({"UCI_LimitStrength": True, "UCI_Elo": args.elo})
                sf_desc += f" (elo {args.elo})"

            we_are_white = g % 2 == 0
            white, black = (ours, sf) if we_are_white else (sf, ours)

            live_cb = None
            if args.upload and not args.no_live:
                conf_l = load_supabase_env()
                b_url = os.environ.get("SUPABASE_URL") or conf_l.get("SUPABASE_URL")
                b_key = os.environ.get("SUPABASE_KEY") or conf_l.get("SUPABASE_KEY")
                if b_url and b_key:
                    live_cb = make_live_cb(b_url.rstrip("/"), b_key,
                                           "white" if we_are_white else "black",
                                           args.engine, os.environ.copy(),
                                           sf_skill=args.skill)
            board, evals = play_game(white, black, args.movetime, live_cb=live_cb)
            strat_series = getattr(live_cb, "series", []) if live_cb else []

            # Quit engines to reset state for next game
            ours.quit()
            sf.quit()

            # Incomplete games (engine crash, timeout) shouldn't be recorded.
            # claim_draw matches play_game's loop condition, else games drawn
            # by repetition/fifty-move get misfiled as incomplete.
            if not board.is_game_over(claim_draw=True):
                print(f"game {g+1}: INCOMPLETE (max plies reached at move {board.fullmove_number}), skipped")
                sys.stdout.flush()
                continue

            result = board.result(claim_draw=True)

            if result == "1/2-1/2":
                draws += 1
                outcome = "draw"
            elif result == "*":
                # Game hit move limit without a decisive result
                # This shouldn't happen with a reasonable limit, but if it does treat as a draw
                # (the proper fix is to increase max_plies or remove the limit)
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

            row = {
                "round": g + 1,
                "our_color": "white" if we_are_white else "black",
                "result": result if result != "*" else "1/2-1/2",
                "outcome": outcome,
                "moves": board.fullmove_number,
                "pgn": str(game),
                "uci": " ".join(m.uci() for m in board.move_stack),
                "evals": json.dumps(evals),
                "strat_series": json.dumps(strat_series),
            }
            played.append(row)
            # Durable as it goes: write this game now rather than waiting for
            # the match to end, so an interrupted run keeps everything it
            # actually finished.
            upload_game(match_handle, args.skill, row)
            if pgn_out:
                print(game, file=pgn_out, flush=True)
                print(file=pgn_out)

            # --learn: same per-game learning ladder_loop.py does — nudge the
            # strategy weights from this game's outcome and add it to the
            # permanent training log so the next game plays a changed engine.
            if args.learn:
                oc = {"win": 1.0, "draw": 0.5, "loss": 0.0}[outcome]
                with open(os.path.join(root, "data", "games_log.pgn"), "a") as log:
                    print(game, file=log); print(file=log)
                one = os.path.join(root, "tmp_learn_game.pgn")
                with open(one, "w") as f:
                    print(game, file=f)
                subprocess.run(["python3", os.path.join(root, "tools", "analyze_game.py"),
                                one, args.engine,
                                os.path.join(root, "strategy_weights.txt"),
                                str(oc), "0.5"],
                               cwd=root, check=False)
                print(f"  learned from game {g+1} (outcome {oc})")
                sys.stdout.flush()
    finally:
        if pgn_out:
            pgn_out.close()

    n = wins + draws + losses
    score = wins + draws / 2
    print(f"\nchess-bb vs {sf_desc}: +{wins} ={draws} -{losses}  "
          f"({score}/{n} = {100*score/max(n,1):.0f}%)")

    # Games were uploaded one by one as they finished; just record the summary.
    close_match(match_handle, wins, draws, losses)

    # --retune: the slow deep learning — relearn the square/piece value tables
    # and refit the strategy calibration from the whole (now larger) log.
    if args.retune:
        print("\nretuning value tables + calibration from the full log...")
        sys.stdout.flush()
        # Build the dataset from the FULL shared Supabase game history (plus
        # any un-uploaded local games) so every machine retunes on the same
        # corpus, not just its own local log.
        env = dict(os.environ,
                   CHESS_WEIGHTS="data/learned_values.txt",
                   CHESS_BOOK="data/route_book.txt")
        subprocess.run(["python3", "tools/make_dataset.py",
                        "data/texel_dataset.txt", "data/games_log.pgn"],
                       cwd=root, env=env, check=False)
        with open(os.path.join(root, "data/learned_values.txt.new"), "w") as out, \
             open(f"{TMP_DIR}/retune.log", "w") as err:
            subprocess.run(["./chess", "tune", "data/texel_dataset.txt"],
                           cwd=root, env=env, stdout=out, stderr=err, check=False)
        os.replace(os.path.join(root, "data/learned_values.txt.new"),
                   os.path.join(root, "data/learned_values.txt"))
        subprocess.run(["python3", "tools/fit_strategy_weights.py", "--apply"],
                       cwd=root, env=env, check=False)
        subprocess.run(["python3", "tools/upload_learned.py"], cwd=root, check=False)
        print("retune complete.")


if __name__ == "__main__":
    main()
