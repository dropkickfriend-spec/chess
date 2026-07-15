// perft.h — Part 6: move generator correctness testing
#ifndef PERFT_H
#define PERFT_H

#include "board.h"

long long perft(Board *bd, int depth);
void perft_divide(Board *bd, int depth);

#endif
