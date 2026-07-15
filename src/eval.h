// eval.h — Part 8: static evaluation
#ifndef EVAL_H
#define EVAL_H

#include "board.h"

// Score in centipawns from the side-to-move's perspective (negamax convention).
int evaluate(const Board *bd);

#endif
