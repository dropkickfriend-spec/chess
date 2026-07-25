#!/usr/bin/env python3
"""Pull the canonical engine brain FROM Supabase into the local files the
engine reads at startup — so every machine plays the latest shared weights,
never stale files or the hardcoded eval.c defaults.

Writes:
  strategy_weights.txt   <- strategy_weights_current (13 rows)
  data/learned_values.txt <- learned_tables.raw (verbatim registry dump)

Usage: python3 tools/sync_weights.py
"""
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from match import load_supabase_env, STRAT_ORDER as STRATS


def get(base, key, q):
    req = urllib.request.Request(
        f"{base}/rest/v1/{q}",
        headers={"apikey": key, "Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


def main():
    conf = load_supabase_env()
    base = (os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL", "")).rstrip("/")
    key = os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY")
    if not base or not key:
        print("no supabase creds; sync skipped")
        return

    # Strategy weights -> strategy_weights.txt (already the file's exact format)
    # Keyed by name: strategy_idx means different things either side of an enum
    # edit, so a row's name is the only stable identity. Rows for strategies the
    # engine no longer has (GAME_PLAN, TRANSIT) are skipped rather than written
    # out where they would land on a live strategy's slot.
    rows = get(base, key,
               "strategy_weights_current?select=strategy_idx,strategy_name,weight"
               "&order=strategy_idx.asc")
    live = set(STRATS)
    rows = [r for r in rows or [] if r.get("strategy_name") in live]
    if rows:
        with open(os.path.join(ROOT, "strategy_weights.txt"), "w") as f:
            for r in rows:
                f.write(f"weight {r['strategy_name']} {r['weight']}\n")
        print(f"synced {len(rows)} strategy weights")

    # Full registry -> learned_values.txt (verbatim raw dump)
    lt = get(base, key, "learned_tables?id=eq.1&select=raw")
    raw = lt[0]["raw"] if lt and lt[0].get("raw") else None
    if raw:
        with open(os.path.join(ROOT, "data", "learned_values.txt"), "w") as f:
            f.write(raw)
        print(f"synced learned_values.txt ({raw.count(chr(10))} params)")
    else:
        print("no raw learned_values on Supabase yet (run a --retune to seed it)")


if __name__ == "__main__":
    main()
