// eval.h — Part 8: static evaluation
#ifndef EVAL_H
#define EVAL_H

#include "board.h"

void eval_init(void);   // build pawn-structure / king-shield masks

// Score in centipawns from the side-to-move's perspective (negamax convention).
int evaluate(const Board *bd);

#endif
