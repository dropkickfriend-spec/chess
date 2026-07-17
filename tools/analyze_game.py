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
import sys
import chess
import chess.pgn

STRATEGIES = [
    "DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
    "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE", "DEFENDER_LOGISTICS",
    "KING_ACTIVITY", "PAWN_PROMOTION", "OPPOSITION",
    "MATERIAL", "COORDINATION", "GAME_PLAN", "SQUARE_VALUE"
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
        active = ["MATERIAL", "COORDINATION", "GAME_PLAN", "SQUARE_VALUE"]  # Always active
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

def load_weights(path):
    """Load current strategy weights."""
    weights = {}
    try:
        with open(path) as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 3 and parts[0] == "weight":
                    idx = int(parts[1])
                    w = float(parts[2])
                    if idx < len(STRATEGIES):
                        weights[STRATEGIES[idx]] = w
    except FileNotFoundError:
        pass

    # Fill missing with defaults
    for strat in STRATEGIES:
        if strat not in weights:
            weights[strat] = 1.0

    return weights

def save_weights(path, weights):
    """Save adjusted strategy weights."""
    with open(path, "w") as f:
        for idx, strat in enumerate(STRATEGIES):
            w = weights.get(strat, 1.0)
            f.write(f"weight {idx} {w}\n")

def adjust_weights(weights, strategy_log, outcome, expected=0.5):
    """Surprise-driven, zero-sum weight adjustment.

    The old rule multiplied by 0.98 per MOVE of a lost game, so against an
    opponent that always wins every weight compounded toward zero and the
    eval degenerated to noise (observed: 20 straight losses drove MATERIAL
    to 0.0000 and games got shorter as training progressed).

    New rule:
    - surprise = outcome - expected: a loss to max-skill Stockfish is
      expected and teaches almost nothing; a draw or win is a shock.
    - one bounded nudge per game, scaled by each strategy's share of moves.
    - weights renormalise to mean 1.0 afterwards: learning redistributes
      emphasis between strategies (only ratios matter to the engine's
      relative reweighting), so collapse is impossible.
    """
    surprise = outcome - expected
    total_moves = max(len(strategy_log), 1)

    counts = {}
    for entry in strategy_log:
        for strat in entry["active_strategies"]:
            counts[strat] = counts.get(strat, 0) + 1

    LR = 0.5
    for strat, c in counts.items():
        if strat in weights:
            share = c / total_moves
            weights[strat] *= 1.0 + LR * surprise * share

    mean = sum(weights.values()) / max(len(weights), 1)
    if mean > 1e-6:
        for strat in weights:
            weights[strat] /= mean

    for strat in weights:
        weights[strat] = min(max(weights[strat], 0.05), 20.0)

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
    weights = adjust_weights(weights, strategy_log, outcome, expected)

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
