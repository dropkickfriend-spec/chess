// search.h — Part 9: iterative deepening alpha-beta search
#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"

#define MATE_SCORE 32000
#define INF_SCORE  32500
#define MAX_PLY    64

// Position hash for repetition detection (not a transposition-table key).
U64 board_hash(const Board *bd);

// Hashes of positions already played this game (for threefold-ish detection
// inside the search). Call before search_best_move; n may be 0.
void search_set_history(const U64 *hashes, int n);

// Iterative deepening until movetime_ms elapses or max_depth completes.
// Prints UCI "info" lines per depth; returns the best move (0 if none legal).
int search_best_move(Board *bd, int movetime_ms, int max_depth);

// The current game plan: squares fought over by the last completed search
// depth's principal variation, per side. Published by search_best_move
// after every finished iteration; read by the GAME_PLAN eval strategy so
// the NEXT iteration prices pieces by their role in the plan. Zero when no
// lookahead has happened (depth 1, tune mode, tooling).
extern U64 plan_squares[2];

#endif
