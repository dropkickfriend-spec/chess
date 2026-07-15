// attacks.h — Part 2: stepper attack tables (pawn, knight, king)
#ifndef ATTACKS_H
#define ATTACKS_H

#include "board.h"

extern U64 pawn_attacks[2][64];   // [side][square]
extern U64 knight_attacks[64];
extern U64 king_attacks[64];

void init_stepper_attacks(void);

// Part 3a: slider building blocks
U64 rook_mask(int sq);                     // relevant occupancy mask (edges excluded)
U64 bishop_mask(int sq);
U64 rook_attacks_slow(int sq, U64 block);  // on-the-fly, used to build magic tables
U64 bishop_attacks_slow(int sq, U64 block);

#endif
