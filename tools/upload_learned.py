#!/usr/bin/env python3
"""Push the learned value tables (piece + square values) straight to Supabase
so the dashboard's piece/square-value tabs update live from any machine — no
git push. The strategy weights already upload via analyze_game.py; this closes
the last file-only gap.

Usage: python3 tools/upload_learned.py
"""
import datetime
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from export_dashboard_data import build_learned_tables
from match import load_supabase_env, supabase_insert


def main():
    conf = load_supabase_env()
    base = os.environ.get("SUPABASE_URL") or conf.get("SUPABASE_URL")
    key = os.environ.get("SUPABASE_KEY") or conf.get("SUPABASE_KEY")
    if not base or not key:
        print("no supabase creds; skipped")
        return
    tables = build_learned_tables()
    # Verbatim learned_values.txt so any machine can restore the EXACT brain
    # (the display 'tables' dict is only a subset - no scalars/coord/plan).
    raw = ""
    try:
        with open(os.path.join(ROOT, "data", "learned_values.txt")) as f:
            raw = f.read()
    except OSError:
        pass
    supabase_insert(base.rstrip("/"), key, "learned_tables", [{
        "id": 1,
        "tables": json.dumps(tables),
        "raw": raw,
        "updated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }], upsert=True)
    print("uploaded learned tables + raw values to Supabase")


if __name__ == "__main__":
    main()
