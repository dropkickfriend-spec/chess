// eval.c — material + piece-square tables
// Tables are written from white's point of view, rank 8 first so they read
// like a board diagram; black uses the vertically mirrored square.
#include "eval.h"

static const int material[6] = { 100, 320, 330, 500, 900, 0 };

static const int pst[6][64] = {
    { // pawn
         0,  0,  0,  0,  0,  0,  0,  0,
        50, 50, 50, 50, 50, 50, 50, 50,
        10, 10, 20, 30, 30, 20, 10, 10,
         5,  5, 10, 25, 25, 10,  5,  5,
         0,  0,  0, 20, 20,  0,  0,  0,
         5, -5,-10,  0,  0,-10, -5,  5,
         5, 10, 10,-20,-20, 10, 10,  5,
         0,  0,  0,  0,  0,  0,  0,  0
    },
    { // knight
       -50,-40,-30,-30,-30,-30,-40,-50,
       -40,-20,  0,  0,  0,  0,-20,-40,
       -30,  0, 10, 15, 15, 10,  0,-30,
       -30,  5, 15, 20, 20, 15,  5,-30,
       -30,  0, 15, 20, 20, 15,  0,-30,
       -30,  5, 10, 15, 15, 10,  5,-30,
       -40,-20,  0,  5,  5,  0,-20,-40,
       -50,-40,-30,-30,-30,-30,-40,-50
    },
    { // bishop
       -20,-10,-10,-10,-10,-10,-10,-20,
       -10,  0,  0,  0,  0,  0,  0,-10,
       -10,  0,  5, 10, 10,  5,  0,-10,
       -10,  5,  5, 10, 10,  5,  5,-10,
       -10,  0, 10, 10, 10, 10,  0,-10,
       -10, 10, 10, 10, 10, 10, 10,-10,
       -10,  5,  0,  0,  0,  0,  5,-10,
       -20,-10,-10,-10,-10,-10,-10,-20
    },
    { // rook
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 10, 10, 10, 10, 10,  5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
         0,  0,  0,  5,  5,  0,  0,  0
    },
    { // queen
       -20,-10,-10, -5, -5,-10,-10,-20,
       -10,  0,  0,  0,  0,  0,  0,-10,
       -10,  0,  5,  5,  5,  5,  0,-10,
        -5,  0,  5,  5,  5,  5,  0, -5,
         0,  0,  5,  5,  5,  5,  0, -5,
       -10,  5,  5,  5,  5,  5,  0,-10,
       -10,  0,  5,  0,  0,  0,  0,-10,
       -20,-10,-10, -5, -5,-10,-10,-20
    },
    { // king (middlegame: hide in the corner)
       -30,-40,-40,-50,-50,-40,-40,-30,
       -30,-40,-40,-50,-50,-40,-40,-30,
       -30,-40,-40,-50,-50,-40,-40,-30,
       -30,-40,-40,-50,-50,-40,-40,-30,
       -20,-30,-30,-40,-40,-30,-30,-20,
       -10,-20,-20,-20,-20,-20,-20,-10,
        20, 20,  0,  0,  0,  0, 20, 20,
        20, 30, 10,  0,  0, 10, 30, 20
    }
};

// Endgame king table: centralise once the queens/heavy pieces come off.
static const int king_endgame[64] = {
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50
};

// PST is laid out rank 8 first; convert a board square to a table index.
// White mirrors vertically, black reads directly.
#define W_IDX(sq) ((7 - (sq) / 8) * 8 + (sq) % 8)
#define B_IDX(sq) (sq)

int evaluate(const Board *bd) {
    // Endgame once both sides are down to <= ~1300cp of non-pawn material
    // (roughly: a rook + minor or less each).
    int wnp = COUNT(bd->bb[WN])*320 + COUNT(bd->bb[WB])*330
            + COUNT(bd->bb[WR])*500 + COUNT(bd->bb[WQ])*900;
    int bnp = COUNT(bd->bb[BN])*320 + COUNT(bd->bb[BB_])*330
            + COUNT(bd->bb[BR])*500 + COUNT(bd->bb[BQ])*900;
    int endgame = wnp <= 1300 && bnp <= 1300;

    int score = 0;
    for (int pt = 0; pt < 6; pt++) {
        const int *table = (pt == 5 && endgame) ? king_endgame : pst[pt];

        U64 b = bd->bb[WP + pt];
        while (b) {
            int sq = LSB(b); POP_BIT(b, sq);
            score += material[pt] + table[W_IDX(sq)];
        }
        b = bd->bb[BP + pt];
        while (b) {
            int sq = LSB(b); POP_BIT(b, sq);
            score -= material[pt] + table[B_IDX(sq)];
        }
    }
    return bd->side == WHITE ? score : -score;
}
