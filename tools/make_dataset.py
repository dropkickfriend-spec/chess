#!/usr/bin/env python3
"""Build a Texel-tuning dataset from logged games.

Sources: every PGN in Supabase (chessbb_games) plus any local .pgn files
passed as arguments. Output lines: "FEN;result" with the result from
White's perspective (1 / 0.5 / 0).

Position filter (standard Texel practice):
  - skip the first 16 plies (opening book noise)
  - skip positions in check (eval is meaningless mid-tactic)
  - skip positions where the move actually played was a capture or
    promotion (position wasn't quiet)
  - deduplicate by FEN

Usage:
  python3 tools/make_dataset.py out.txt [extra1.pgn extra2.pgn ...]
"""
import io
import json
import os
import sys
import urllib.request

import chess
import chess.pgn

from match import load_supabase_env

RESULT_MAP = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}


def fetch_supabase_pgns():
    conf = load_supabase_env()
    base_url = os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL")
    key = os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY")
    if not base_url or not key:
        return []
    pgns = []
    offset, page = 0, 1000
    while True:
        req = urllib.request.Request(
            f"{base_url.rstrip('/')}/rest/v1/chessbb_games"
            f"?select=pgn,result&offset={offset}&limit={page}",
            headers={"apikey": key, "Authorization": f"Bearer {key}"},
        )
        with urllib.request.urlopen(req) as resp:
            rows = json.loads(resp.read())
        pgns += [r["pgn"] for r in rows if r["result"] in RESULT_MAP]
        if len(rows) < page:
            break
        offset += page
    return pgns


def positions_from_pgn(pgn_text, seen):
    game = chess.pgn.read_game(io.StringIO(pgn_text))
    if game is None:
        return
    result = RESULT_MAP.get(game.headers.get("Result", "*"))
    if result is None:
        return
    board = game.board()
    for ply, move in enumerate(game.mainline_moves()):
        if (ply >= 16
                and not board.is_check()
                and not board.is_capture(move)
                and move.promotion is None):
            fen = board.fen()
            key = " ".join(fen.split()[:4])   # position only, ignore clocks
            if key not in seen:
                seen.add(key)
                yield fen, result
        board.push(move)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out_path = sys.argv[1]
    extra_files = sys.argv[2:]

    pgns = fetch_supabase_pgns()
    print(f"supabase: {len(pgns)} games")
    for path in extra_files:
        with open(path) as f:
            chunks = f.read().split("\n\n[")
        pgns += [c if c.startswith("[") else "[" + c for c in chunks if c.strip()]
    print(f"total games: {len(pgns)}")

    seen = set()
    count = 0
    with open(out_path, "w") as out:
        for pgn in pgns:
            for fen, result in positions_from_pgn(pgn, seen):
                out.write(f"{fen};{result}\n")
                count += 1
    print(f"wrote {count} unique quiet positions to {out_path}")


if __name__ == "__main__":
    main()
