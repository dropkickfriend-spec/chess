#!/usr/bin/env python3
"""Mine the positions where games were actually lost.

Result-only learning gives ONE bit per game (won/lost) and never says which move
was the mistake. That is the fundamental credit-assignment blindness in this
project: the Texel tuner labels every position in a lost game "loss", including
the ones where the engine was still completely fine.

But we already store a per-ply eval trace for every game. A loss looks like:

    +4, +44, +77, +24, +116, +59, ... , -9998, -9999

The collapse is right there in the numbers. So we do not need to replay a game
from every move to find the blunder -- we scan the trace for the largest drop
across one of OUR moves, and that is the position where the game turned.

Two things to get right:
  * evals are stored from WHITE's perspective (match.py records
    score.white()), so when we played Black a blunder by us is an INCREASE.
  * Only drops across OUR OWN moves count. A drop across the opponent's move
    means they played something strong, not that we erred.

Output:
  * a ranked report of where games are being lost;
  * a dataset of those positions (FEN;result) that can train the tuner on
    decisive moments instead of a uniform sample of mostly-quiet positions;
  * per-opening loss counts, so opening choice can be steered by where we
    actually fall apart.

Usage:
  python3 lab/blunder_mine.py --limit 400 --out data/blunders.txt
  python3 lab/blunder_mine.py --min-drop 200 --top 30
"""
import argparse
import json
import os
import sys
import urllib.request
from collections import Counter

import chess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

MATE = 9000          # |eval| above this is a mate score, not a centipawn count
CLAMP = 2000         # cap swings so one mate does not dominate the ranking


def _creds():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from match import load_supabase_env
    conf = load_supabase_env()
    base = (os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL", "")).rstrip("/")
    key = os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY")
    if not base or not key:
        sys.exit("no Supabase credentials")
    return base, key


def _get(url, key):
    req = urllib.request.Request(url, headers={"apikey": key,
                                               "Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def fetch_sql(limit, min_drop):
    """Server-side scan via the blunder_positions view.

    The scan itself (unnest the eval trace, lag() for before/after, flip the
    sign for Black, clamp mates, take the max drop per game) runs in Postgres,
    so we transfer one small row per game instead of every game's full eval
    trace. Measured: 1.0 MB -> 200 KB for the same result, and the view carries
    only the move prefix needed to rebuild the position, not the whole game.
    """
    base, key = _creds()
    url = (f"{base}/rest/v1/blunder_positions?select=ply,drop,before_cp,after_cp,"
           f"our_color,result,sf_skill,uci_prefix,score"
           f"&drop=gte.{min_drop}&order=drop.desc&limit={limit}")
    return _get(url, key)


def fetch(limit):
    """Legacy client-side path: pull whole games and scan them here."""
    base, key = _creds()
    url = (f"{base}/rest/v1/chessbb_games?select=our_color,result,uci,evals,sf_skill"
           f"&evals=not.is.null&uci=not.is.null&order=created_at.desc&limit={limit}")
    return _get(url, key)


def clamp(v):
    if v is None:
        return None
    return max(-CLAMP, min(CLAMP, v))


def find_blunder(row):
    """Largest eval drop across one of OUR moves. Returns dict or None."""
    try:
        evals = json.loads(row["evals"])
        moves = row["uci"].split()
    except Exception:
        return None
    if not evals or len(moves) < 4:
        return None

    we_white = row["our_color"] == "white"
    # our plies: 0,2,4,... as White; 1,3,5,... as Black
    parity = 0 if we_white else 1
    sign = 1 if we_white else -1        # convert White-POV eval to our POV

    best = None
    for i in range(len(evals)):
        if i % 2 != parity or i == 0:
            continue                     # not our move, or no "before" to compare
        before, after = evals[i - 1], evals[i]
        if before is None or after is None:
            continue
        b, a = sign * before, sign * after
        # Once the game is already decided, later swings are noise. Only count a
        # drop that starts from a position we were not yet lost in.
        if b < -MATE or b > MATE:
            continue
        drop = clamp(b) - clamp(a)
        if best is None or drop > best["drop"]:
            best = {"drop": drop, "ply": i, "before": b, "after": a}
    if not best or best["drop"] <= 0:
        return None

    # Rebuild the position we FACED before playing the losing move -- that is
    # the position worth training on, not the one after the mistake.
    board = chess.Board()
    try:
        for mv in moves[:best["ply"]]:
            board.push_uci(mv)
    except Exception:
        return None
    best["fen"] = board.fen()
    best["played"] = moves[best["ply"]] if best["ply"] < len(moves) else None
    best["opening"] = " ".join(moves[:6])
    best["sf_skill"] = row.get("sf_skill")
    best["result"] = row["result"]
    best["our_color"] = row["our_color"]
    # Outcome from our side, for the dataset label.
    if row["result"] == "1/2-1/2":
        best["score"] = 0.5
    else:
        best["score"] = 1.0 if (row["result"] == "1-0") == we_white else 0.0
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=400, help="games to scan")
    ap.add_argument("--min-drop", type=int, default=150,
                    help="ignore swings smaller than this (cp)")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--out", default=None,
                    help="write FEN;result lines for the tuner")
    ap.add_argument("--client-scan", action="store_true",
                    help="scan locally instead of using the blunder_positions "
                         "view (fallback if the view is missing)")
    args = ap.parse_args()

    found = []
    if args.client_scan:
        rows = fetch(args.limit)
        print(f"client-side scan of {len(rows)} games")
        for r in rows:
            b = find_blunder(r)
            if b and b["drop"] >= args.min_drop:
                found.append(b)
    else:
        # Postgres does the scan; we only rebuild the board for the flagged
        # positions, from the move prefix the view already trimmed for us.
        rows = fetch_sql(args.limit, args.min_drop)
        print(f"server-side scan (blunder_positions view): {len(rows)} swings "
              f">= {args.min_drop} cp")
        for r in rows:
            board = chess.Board()
            ok = True
            try:
                for mv in (r["uci_prefix"] or "").split():
                    board.push_uci(mv)
            except Exception:
                ok = False
            if not ok:
                continue
            found.append({"drop": r["drop"], "ply": r["ply"],
                          "before": r["before_cp"], "after": r["after_cp"],
                          "fen": board.fen(), "played": None,
                          "opening": " ".join((r["uci_prefix"] or "").split()[:6]),
                          "sf_skill": r["sf_skill"], "result": r["result"],
                          "our_color": r["our_color"], "score": r["score"]})
    found.sort(key=lambda x: -x["drop"])
    print(f"{len(found)} decisive positions\n")

    print(f"=== top {min(args.top, len(found))} positions where games turned ===")
    for b in found[:args.top]:
        sk = "max" if b["sf_skill"] is None else b["sf_skill"]
        print(f"  ply {b['ply']:3d}  drop {b['drop']:5d} cp "
              f"({b['before']:+5d} -> {b['after']:+6d})  "
              f"skill {sk:>3}  as {b['our_color']:<5}"
              + (f" played {b['played']}" if b['played'] else ""))
        print(f"        {b['fen']}")

    # Where do we fall apart? Opening and phase are the actionable summaries:
    # they say which openings to stop playing and whether we lose games in the
    # opening, middlegame or endgame.
    print("\n=== losses by opening (first 3 moves) ===")
    for op, n in Counter(b["opening"] for b in found).most_common(8):
        print(f"  {n:3d}  {op}")

    print("\n=== when games turn ===")
    buckets = Counter()
    for b in found:
        p = b["ply"]
        buckets["opening (ply<20)" if p < 20 else
                "middlegame (20-59)" if p < 60 else "endgame (60+)"] += 1
    for k, n in buckets.most_common():
        print(f"  {n:3d}  {k}")

    if args.out:
        path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
        with open(path, "w") as f:
            for b in found:
                f.write(f"{b['fen']};{b['score']}\n")
        print(f"\nwrote {len(found)} decisive positions -> {path}")
        print("  (train on these instead of a uniform quiet-position sample:")
        print("   every line is a position where a game was actually decided)")


if __name__ == "__main__":
    main()
