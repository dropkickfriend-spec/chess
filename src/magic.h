// magic.h — Part 3b: magic bitboard slider attack lookup
#ifndef MAGIC_H
#define MAGIC_H

#include "board.h"

void init_slider_attacks(void);

U64 get_rook_attacks(int sq, U64 occ);
U64 get_bishop_attacks(int sq, U64 occ);
U64 get_queen_attacks(int sq, U64 occ);

#endif
