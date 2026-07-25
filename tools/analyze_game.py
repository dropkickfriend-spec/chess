#!/usr/bin/env python3
"""Post-game analyzer: trace which strategies were active, adjust weights based on outcome.

Usage:
  python3 tools/analyze_game.py game.pgn engine_path weights.txt outcome [expected]

  outcome:  1 (win), 0.5 (draw), 0 (loss)
  expected: prior expectation of scoring against this opponent (default 0.5;
            use ~0.05 vs max-skill Stockfish so losses teach little and
            draws/wins teach a lot)
  Adjusts weights.txt and uploads to Supabase if configured.
"""
import math
import os
import sys
import chess
import chess.pgn

STRATEGIES = [
    "DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
    "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE", "DEFENDER_LOGISTICS",
    "KING_ACTIVITY", "PAWN_PROMOTION", "OPPOSITION",
    "MATERIAL", "COORDINATION", "SQUARE_VALUE",
    "BLOCKADE", "RESTRICTION"
]

PHASE_THRESHOLDS = {
    "opening": 20,
    "middlegame": 10,
    "endgame": 0
}

def detect_phase(board):
    """Detect game phase based on material."""
    phase_weights = {
        chess.KNIGHT: 1, chess.BISHOP: 1, chess.ROOK: 2, chess.QUEEN: 4
    }
    material = 0
    for piece_type, weight in phase_weights.items():
        material += len(board.pieces(piece_type, chess.WHITE)) * weight
        material += len(board.pieces(piece_type, chess.BLACK)) * weight

    if material >= PHASE_THRESHOLDS["opening"]:
        return "opening"
    elif material >= PHASE_THRESHOLDS["middlegame"]:
        return "middlegame"
    return "endgame"

def trace_strategies(pgn_file, engine_path):
    """Trace active strategies through each move."""
    import io
    game = chess.pgn.read_game(io.StringIO(pgn_file))
    if not game:
        return []

    board = game.board()
    strategy_log = []

    for move_num, move in enumerate(game.mainline_moves()):
        is_pawn_move = board.piece_type_at(move.from_square) == chess.PAWN
        board.push(move)
        phase = detect_phase(board)

        # Determine which strategies are "active" in this position.
        # Every strategy now has an implementation, so each phase tunes
        # its full set instead of leaving weights frozen at 1.0.
        active = ["MATERIAL", "COORDINATION", "SQUARE_VALUE", "BLOCKADE", "RESTRICTION"]  # Always active
        if phase == "opening":
            active.extend(["DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING"])
        elif phase == "middlegame":
            active.extend(["PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE"])
        else:
            active.extend(["KING_ACTIVITY", "PAWN_PROMOTION", "OPPOSITION"])

        # Pawn moves engage the logistics tradeoff (blockage vs promotion)
        if is_pawn_move:
            active.append("DEFENDER_LOGISTICS")

        strategy_log.append({
            "move": move_num,
            "fen": board.fen(),
            "phase": phase,
            "active_strategies": active
        })

    return strategy_log

# Slot order from when weight files were keyed by integer index, kept so older
# files still load correctly. A name here that is no longer in STRATEGIES is a
# retired strategy and is dropped — which is the whole point of keying by name:
# without this table, removing GAME_PLAN (slot 12) would shift every later
# weight onto the wrong strategy.
LEGACY_SLOTS = [
    "DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
    "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE", "DEFENDER_LOGISTICS",
    "KING_ACTIVITY", "PAWN_PROMOTION", "OPPOSITION",
    "MATERIAL", "COORDINATION", "GAME_PLAN", "SQUARE_VALUE",
    "BLOCKADE", "RESTRICTION", "TRANSIT",
]


def load_weights(path):
    """Load current strategy weights, by name or by legacy integer index."""
    weights = {}
    try:
        with open(path) as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) < 3 or parts[0] != "weight":
                    continue
                key, w = parts[1], float(parts[2])
                if key not in STRATEGIES:
                    if not key.isdigit() or int(key) >= len(LEGACY_SLOTS):
                        continue
                    key = LEGACY_SLOTS[int(key)]
                if key in STRATEGIES:
                    weights[key] = w
    except FileNotFoundError:
        pass

    # Fill missing with defaults
    for strat in STRATEGIES:
        if strat not in weights:
            weights[strat] = 1.0

    return weights

def save_weights(path, weights):
    """Save adjusted strategy weights, keyed by NAME.

    Keying by position meant deleting a strategy from the middle of the enum
    silently shifted every later weight onto the wrong strategy. Names survive
    enum edits; the engine still reads the old integer form through a legacy
    slot table (eval_strategy.c) so existing files keep working."""
    with open(path, "w") as f:
        for strat in STRATEGIES:
            f.write(f"weight {strat} {weights.get(strat, 1.0)}\n")

def live_strategies(engine_path):
    """Names the engine still scores. A strategy whose machinery is switched
    off reports an effective weight of 0 and is excluded from the engine's
    budget entirely — nudging its weight afterwards just makes a dead number
    drift upward forever, since it counts as 'active' on every move. (GAME_PLAN
    was the case that motivated this; it has since been removed outright.)
    Returns None if the probe fails, meaning "treat everything as live"."""
    import subprocess
    try:
        p = subprocess.run([engine_path, "evalfens"],
                           input=chess.STARTING_FEN + "\n",
                           capture_output=True, text=True, timeout=15,
                           cwd=os.path.dirname(os.path.abspath(engine_path)) or ".")
    except Exception:
        return None
    live = set()
    for line in p.stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[0] in STRATEGIES and float(f[2]) != 0.0:
            live.add(f[0])
    return live or None


def adjust_weights(weights, strategy_log, outcome, expected=0.5,
                   LR=0.3, MEANREV=0.15, live=None):
    """Surprise-driven weight adjustment in log space.

    The previous rule multiplied each weight by (1 + LR*surprise*share) and
    renormalised by the ARITHMETIC mean. "share" = fraction of moves a
    strategy was active, so the six all-game strategies (share~1) grew fastest
    on every win, inflated the mean, and the renormalise step pushed every
    phase-specific strategy (share<0.5) DOWN — to the floor, permanently. A
    simulation confirmed it: from a uniform start, 20 wins drove PIECE_ACTIVITY,
    KING_SAFETY, DEVELOPMENT etc. to the 0.05/0.25 floor and left them stuck.
    It rewarded *duration of activity*, not contribution to the result.

    New rule (validated by that same simulation, tools test_weights3.py):
    - work in log space so updates are symmetric and can't drive a weight <0;
    - MEANREV pulls every weight back toward the uniform prior (1.0) each game,
      so a crushed strategy climbs back on its own and nothing runs away;
    - centred credit surprise*(share - mean_share): above-average participation
      earns weight on a win (below-average loses), reversed on a loss — zero
      sum, so it can't pump every active strategy at once;
    - GEOMETRIC-mean normalisation (subtract the mean log), which is floor
      neutral, instead of arithmetic-mean normalisation which floored the
      small weights. Real differentiation comes from the --retune fit; this
      nudge just moves the brain sensibly between retunes without collapsing.
    """
    surprise = outcome - expected
    total_moves = max(len(strategy_log), 1)

    counts = {}
    for entry in strategy_log:
        for strat in entry["active_strategies"]:
            counts[strat] = counts.get(strat, 0) + 1
    keys = [s for s in weights if live is None or s in live]
    shares = {s: counts.get(s, 0) / total_moves for s in keys}
    mean_share = sum(shares.values()) / max(len(shares), 1)

    for strat in keys:
        lw = math.log(max(weights[strat], 1e-6))
        lw = (1.0 - MEANREV) * lw + LR * surprise * (shares[strat] - mean_share)
        weights[strat] = math.exp(lw)

    # Geometric-mean normalisation: subtract the mean log so the ratios stay
    # centred on 1.0 without floor bias (only ratios matter to the engine).
    mean_log = sum(math.log(max(weights[s], 1e-6)) for s in keys) / max(len(keys), 1)
    for strat in keys:
        weights[strat] = math.exp(math.log(max(weights[strat], 1e-6)) - mean_log)

    # Floor at a participatory level (matches fit_strategy_weights.py).
    for strat in keys:
        weights[strat] = min(max(weights[strat], 0.25), 20.0)

    return weights

def upload_weights_to_supabase(strategy_log, weights, game_id=None):
    """Upload weights to Supabase for tracking."""
    try:
        from match import load_supabase_env, supabase_insert
    except ImportError:
        return

    try:
        conf = load_supabase_env()
        base_url = conf.get("SUPABASE_URL", "")
        key = conf.get("SUPABASE_KEY", "")

        if not base_url or not key:
            return

        rows = []
        for idx, strat in enumerate(STRATEGIES):
            w = weights.get(strat, 1.0)
            rows.append({
                "game_id": game_id,
                "strategy_idx": idx,
                "strategy_name": strat,
                "weight": w
            })

        supabase_insert(base_url.rstrip("/"), key, "strategy_weights", rows)

        # Update current weights (single upsert batch on the PK)
        supabase_insert(base_url.rstrip("/"), key, "strategy_weights_current",
                        [{"strategy_idx": idx, "strategy_name": strat,
                          "weight": weights.get(strat, 1.0)}
                         for idx, strat in enumerate(STRATEGIES)],
                        upsert=True)
    except Exception as e:
        print(f"Supabase upload failed: {e}", file=sys.stderr)

def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return

    pgn_path, engine_path, weights_path, outcome_str = sys.argv[1:5]
    expected = float(sys.argv[5]) if len(sys.argv) > 5 else 0.5
    outcome = float(outcome_str)

    # Read game
    with open(pgn_path) as f:
        pgn_text = f.read()

    # Trace strategies through game
    strategy_log = trace_strategies(pgn_text, engine_path)
    if not strategy_log:
        print("No strategies traced")
        return

    # Load current weights
    weights = load_weights(weights_path)

    # Adjust based on outcome relative to expectation
    live = live_strategies(engine_path)
    dead = [s for s in STRATEGIES if live is not None and s not in live]
    if dead:
        print(f"  (skipping dead strategies: {', '.join(dead)})")
    weights = adjust_weights(weights, strategy_log, outcome, expected, live=live)

    # Save updated weights locally
    save_weights(weights_path, weights)

    # Upload to Supabase for tracking
    upload_weights_to_supabase(strategy_log, weights)

    print(f"Updated weights: {len(strategy_log)} moves, outcome {outcome:.1f}")
    print("New weights:")
    for strat, w in sorted(weights.items()):
        print(f"  {strat}: {w:.4f}")

if __name__ == "__main__":
    main()
