// attacks.c — generate stepper attack tables at startup
#include "attacks.h"

U64 pawn_attacks[2][64];
U64 knight_attacks[64];
U64 king_attacks[64];

// File masks to stop shifts wrapping around board edges
static const U64 NOT_A  = 0xFEFEFEFEFEFEFEFEULL; // everything except a-file
static const U64 NOT_H  = 0x7F7F7F7F7F7F7F7FULL;
static const U64 NOT_AB = 0xFCFCFCFCFCFCFCFCULL;
static const U64 NOT_GH = 0x3F3F3F3F3F3F3F3FULL;

static U64 gen_pawn(int side, int sq) {
    U64 b = 1ULL << sq, a = 0;
    if (side == WHITE) {
        a |= (b << 7) & NOT_H;  // capture left  (from white's view)
        a |= (b << 9) & NOT_A;  // capture right
    } else {
        a |= (b >> 7) & NOT_A;
        a |= (b >> 9) & NOT_H;
    }
    return a;
}

static U64 gen_knight(int sq) {
    U64 b = 1ULL << sq, a = 0;
    a |= (b << 17) & NOT_A;   // up 2, right 1
    a |= (b << 15) & NOT_H;   // up 2, left 1
    a |= (b << 10) & NOT_AB;  // up 1, right 2
    a |= (b <<  6) & NOT_GH;  // up 1, left 2
    a |= (b >> 17) & NOT_H;
    a |= (b >> 15) & NOT_A;
    a |= (b >> 10) & NOT_GH;
    a |= (b >>  6) & NOT_AB;
    return a;
}

static U64 gen_king(int sq) {
    U64 b = 1ULL << sq, a = 0;
    a |= (b << 8) | (b >> 8);            // up, down
    a |= ((b << 1) | (b << 9) | (b >> 7)) & NOT_A;  // right-ish
    a |= ((b >> 1) | (b >> 9) | (b << 7)) & NOT_H;  // left-ish
    return a;
}

void init_stepper_attacks(void) {
    for (int sq = 0; sq < 64; sq++) {
        pawn_attacks[WHITE][sq] = gen_pawn(WHITE, sq);
        pawn_attacks[BLACK][sq] = gen_pawn(BLACK, sq);
        knight_attacks[sq] = gen_knight(sq);
        king_attacks[sq]   = gen_king(sq);
    }
}

// ---- Part 3a: slider masks + on-the-fly attacks ----

// Relevant occupancy mask: ray squares excluding board edges
// (edge squares never affect what's reachable, so they're dropped
//  to shrink the magic table index space)
U64 rook_mask(int sq) {
    U64 m = 0;
    int r = sq / 8, f = sq % 8;
    for (int i = r + 1; i <= 6; i++) m |= 1ULL << (i * 8 + f);
    for (int i = r - 1; i >= 1; i--) m |= 1ULL << (i * 8 + f);
    for (int i = f + 1; i <= 6; i++) m |= 1ULL << (r * 8 + i);
    for (int i = f - 1; i >= 1; i--) m |= 1ULL << (r * 8 + i);
    return m;
}

U64 bishop_mask(int sq) {
    U64 m = 0;
    int r = sq / 8, f = sq % 8;
    for (int i = r+1, j = f+1; i <= 6 && j <= 6; i++, j++) m |= 1ULL << (i*8+j);
    for (int i = r+1, j = f-1; i <= 6 && j >= 1; i++, j--) m |= 1ULL << (i*8+j);
    for (int i = r-1, j = f+1; i >= 1 && j <= 6; i--, j++) m |= 1ULL << (i*8+j);
    for (int i = r-1, j = f-1; i >= 1 && j >= 1; i--, j--) m |= 1ULL << (i*8+j);
    return m;
}

// On-the-fly attacks: walk each ray until hitting a blocker (blocker square included)
U64 rook_attacks_slow(int sq, U64 block) {
    U64 a = 0;
    int r = sq / 8, f = sq % 8;
    for (int i = r+1; i <= 7; i++) { a |= 1ULL << (i*8+f); if (block & (1ULL << (i*8+f))) break; }
    for (int i = r-1; i >= 0; i--) { a |= 1ULL << (i*8+f); if (block & (1ULL << (i*8+f))) break; }
    for (int i = f+1; i <= 7; i++) { a |= 1ULL << (r*8+i); if (block & (1ULL << (r*8+i))) break; }
    for (int i = f-1; i >= 0; i--) { a |= 1ULL << (r*8+i); if (block & (1ULL << (r*8+i))) break; }
    return a;
}

U64 bishop_attacks_slow(int sq, U64 block) {
    U64 a = 0;
    int r = sq / 8, f = sq % 8;
    for (int i = r+1, j = f+1; i <= 7 && j <= 7; i++, j++) { a |= 1ULL << (i*8+j); if (block & (1ULL << (i*8+j))) break; }
    for (int i = r+1, j = f-1; i <= 7 && j >= 0; i++, j--) { a |= 1ULL << (i*8+j); if (block & (1ULL << (i*8+j))) break; }
    for (int i = r-1, j = f+1; i >= 0 && j <= 7; i--, j++) { a |= 1ULL << (i*8+j); if (block & (1ULL << (i*8+j))) break; }
    for (int i = r-1, j = f-1; i >= 0 && j >= 0; i--, j--) { a |= 1ULL << (i*8+j); if (block & (1ULL << (i*8+j))) break; }
    return a;
}
