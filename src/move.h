// move.h — Part 5: make/unmake + legal move filtering
#ifndef MOVE_H
#define MOVE_H

#include "board.h"
#include "movegen.h"

typedef struct {
    int captured;   // captured piece index, -1 if none
    int ep;
    int castle;
    int halfmove;
    int fullmove;
} Undo;

void make_move(Board *bd, int move, Undo *undo);
void unmake_move(Board *bd, int move, const Undo *undo);

// Pseudo-legal moves filtered down to ones that don't leave the mover's king in check.
void generate_legal_moves(const Board *bd, MoveList *ml);

#endif
