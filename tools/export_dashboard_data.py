#!/usr/bin/env python3
"""Export dashboard data: games with per-move piece/square values.

Reads data/games_log.pgn and writes docs/data/games.json (last N games,
newest first) plus docs/data/matches.json (recent results table). Each move
carries the engine's own value tables applied to that move: the moving
piece's material worth (mg/eg), its piece-square value on the from- and
to-squares, the delta, capture details, and the FEN after the move so the
dashboard board can render without a chess library.

Value tables mirror src/eval.c exactly (baseline, not the Texel overlay).

Usage:
  python3 tools/export_dashboard_data.py [max_games]
"""
import json
import os
import sys

import chess
import chess.pgn

MAX_GAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 24

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- Value tables copied from src/eval.c (keep in sync) ----
MATERIAL_MG = [100, 320, 330, 500, 900, 0]
MATERIAL_EG = [120, 300, 330, 520, 920, 0]

PST_MG = [
    [  # pawn
         0,  0,  0,  0,  0,  0,  0,  0,
        50, 50, 50, 50, 50, 50, 50, 50,
        10, 10, 20, 30, 30, 20, 10, 10,
         5,  5, 10, 25, 25, 10,  5,  5,
         0,  0,  0, 20, 20,  0,  0,  0,
         5, -5,-10,  0,  0,-10, -5,  5,
         5, 10, 10,-20,-20, 10, 10,  5,
         0,  0,  0,  0,  0,  0,  0,  0
    ],
    [  # knight
       -50,-40,-30,-30,-30,-30,-40,-50,
       -40,-20,  0,  0,  0,  0,-20,-40,
       -30,  0, 10, 15, 15, 10,  0,-30,
       -30,  5, 15, 20, 20, 15,  5,-30,
       -30,  0, 15, 20, 20, 15,  0,-30,
       -30,  5, 10, 15, 15, 10,  5,-30,
       -40,-20,  0,  5,  5,  0,-20,-40,
       -50,-40,-30,-30,-30,-30,-40,-50
    ],
    [  # bishop
       -20,-10,-10,-10,-10,-10,-10,-20,
       -10,  0,  0,  0,  0,  0,  0,-10,
       -10,  0,  5, 10, 10,  5,  0,-10,
       -10,  5,  5, 10, 10,  5,  5,-10,
       -10,  0, 10, 10, 10, 10,  0,-10,
       -10, 10, 10, 10, 10, 10, 10,-10,
       -10,  5,  0,  0,  0,  0,  5,-10,
       -20,-10,-10,-10,-10,-10,-10,-20
    ],
    [  # rook
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 10, 10, 10, 10, 10,  5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
         0,  0,  0,  5,  5,  0,  0,  0
    ],
    [  # queen
       -20,-10,-10, -5, -5,-10,-10,-20,
       -10,  0,  0,  0,  0,  0,  0,-10,
       -10,  0,  5,  5,  5,  5,  0,-10,
        -5,  0,  5,  5,  5,  5,  0, -5,
         0,  0,  5,  5,  5,  5,  0, -5,
       -10,  5,  5,  5,  5,  5,  0,-10,
       -10,  0,  5,  0,  0,  0,  0,-10,
       -20,-10,-10, -5, -5,-10,-10,-20
    ],
    [  # king (middlegame)
       -30,-40,-40,-50,-50,-40,-40,-30,
       -30,-40,-40,-50,-50,-40,-40,-30,
       -30,-40,-40,-50,-50,-40,-40,-30,
       -30,-40,-40,-50,-50,-40,-40,-30,
       -20,-30,-30,-40,-40,-30,-30,-20,
       -10,-20,-20,-20,-20,-20,-20,-10,
        20, 20,  0,  0,  0,  0, 20, 20,
        20, 30, 10,  0,  0, 10, 30, 20
    ],
]

PAWN_EG = [
     0,  0,  0,  0,  0,  0,  0,  0,
    80, 80, 80, 80, 80, 80, 80, 80,
    50, 50, 50, 50, 50, 50, 50, 50,
    30, 30, 30, 30, 30, 30, 30, 30,
    15, 15, 15, 15, 15, 15, 15, 15,
     5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,
     0,  0,  0,  0,  0,  0,  0,  0
]

KING_EG = [
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50
]

PIECE_IDX = {chess.PAWN: 0, chess.KNIGHT: 1, chess.BISHOP: 2,
             chess.ROOK: 3, chess.QUEEN: 4, chess.KING: 5}


def table_idx(square, color):
    """Mirror src/eval.c: White reads the diagram rank-flipped, Black as-is."""
    if color == chess.WHITE:
        return (7 - chess.square_rank(square)) * 8 + chess.square_file(square)
    return square


def pst(piece_type, square, color, endgame):
    pt = PIECE_IDX[piece_type]
    idx = table_idx(square, color)
    if endgame:
        if pt == 0:
            return PAWN_EG[idx]
        if pt == 5:
            return KING_EG[idx]
    return PST_MG[pt][idx]


def move_record(board, move):
    piece = board.piece_at(move.from_square)
    pt = PIECE_IDX[piece.piece_type]
    color = piece.color

    captured = None
    cap_value = 0
    if board.is_capture(move):
        cap_sq = move.to_square
        if board.is_en_passant(move):
            cap_sq = move.to_square + (-8 if color == chess.WHITE else 8)
        cap_piece = board.piece_at(cap_sq)
        if cap_piece:
            captured = cap_piece.symbol().upper()
            cap_value = MATERIAL_MG[PIECE_IDX[cap_piece.piece_type]]

    end_pt = PIECE_IDX[move.promotion] if move.promotion else pt
    rec = {
        "san": board.san(move),
        "uci": move.uci(),
        "piece": piece.symbol().upper(),
        "color": "w" if color == chess.WHITE else "b",
        "mat": [MATERIAL_MG[pt], MATERIAL_EG[pt]],
        "pst_from": [pst(piece.piece_type, move.from_square, color, False),
                     pst(piece.piece_type, move.from_square, color, True)],
        "pst_to": [pst(chess.PIECE_TYPES[end_pt], move.to_square, color, False),
                   pst(chess.PIECE_TYPES[end_pt], move.to_square, color, True)],
    }
    rec["pst_delta"] = [rec["pst_to"][0] - rec["pst_from"][0],
                        rec["pst_to"][1] - rec["pst_from"][1]]
    if captured:
        rec["captured"] = captured
        rec["cap_value"] = cap_value
    if move.promotion:
        rec["promo"] = chess.piece_symbol(move.promotion).upper()
        rec["mat"] = [MATERIAL_MG[end_pt], MATERIAL_EG[end_pt]]
    return rec


def main():
    log_path = os.path.join(ROOT, "data", "games_log.pgn")
    games = []
    with open(log_path) as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            games.append(game)

    recent = games[-MAX_GAMES:]
    out_games = []
    for game in reversed(recent):          # newest first
        board = game.board()
        moves = []
        for move in game.mainline_moves():
            rec = move_record(board, move)
            board.push(move)
            rec["fen"] = board.fen()
            moves.append(rec)
        out_games.append({
            "white": game.headers.get("White", "?"),
            "black": game.headers.get("Black", "?"),
            "result": game.headers.get("Result", "*"),
            "date": game.headers.get("Date", "?"),
            "event": game.headers.get("Event", ""),
            "moves": moves,
        })

    os.makedirs(os.path.join(ROOT, "docs", "data"), exist_ok=True)
    with open(os.path.join(ROOT, "docs", "data", "games.json"), "w") as f:
        json.dump(out_games, f)
    print(f"exported {len(out_games)} games "
          f"({sum(len(g['moves']) for g in out_games)} moves)")

    # Recent-results table for the matches section (chess-bb games only)
    matches = []
    for game in games:
        white = game.headers.get("White", "")
        black = game.headers.get("Black", "")
        if "chess-bb" not in (white, black):
            continue
        board = game.board()
        n = sum(1 for _ in game.mainline_moves())
        matches.append({
            "result": game.headers.get("Result", "*"),
            "our_color": "white" if white == "chess-bb" else "black",
            "moves": (n + 1) // 2,
            "date": game.headers.get("Date", "?").replace(".", "-"),
            "opponent": black if white == "chess-bb" else white,
        })
    matches = matches[-12:]
    with open(os.path.join(ROOT, "docs", "data", "matches.json"), "w") as f:
        json.dump(matches, f)
    print(f"exported {len(matches)} recent match rows")


if __name__ == "__main__":
    main()
