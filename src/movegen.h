// movegen.h — Part 4: move encoding + pseudo-legal generation
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"

// Move packed into one int:
// bits 0-5   from square
// bits 6-11  to square
// bits 12-15 moving piece
// bits 16-19 promoted piece (0 = none)
// bit 20     capture
// bit 21     double pawn push
// bit 22     en passant
// bit 23     castling
#define ENCODE(from,to,pc,promo,cap,dbl,ep,castle) \
    ((from) | ((to)<<6) | ((pc)<<12) | ((promo)<<16) | \
     ((cap)<<20) | ((dbl)<<21) | ((ep)<<22) | ((castle)<<23))

#define M_FROM(m)    ((m) & 0x3F)
#define M_TO(m)      (((m)>>6) & 0x3F)
#define M_PIECE(m)   (((m)>>12) & 0xF)
#define M_PROMO(m)   (((m)>>16) & 0xF)
#define M_CAP(m)     ((m) & 0x100000)
#define M_DBL(m)     ((m) & 0x200000)
#define M_EP(m)      ((m) & 0x400000)
#define M_CASTLE(m)  ((m) & 0x800000)

typedef struct {
    int moves[256];
    int count;
} MoveList;

int  is_square_attacked(const Board *bd, int sq, int by_side);
void generate_moves(const Board *bd, MoveList *ml);
void move_to_str(int move, char *buf);   // e.g. "e2e4", "e7e8q"

#endif
