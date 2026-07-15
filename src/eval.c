// eval.c — Part 8 (v3): tapered evaluation
// Every term is scored twice — middlegame and endgame — and blended by game
// phase (how much non-pawn material is left), so the king slides smoothly
// from cowering in the corner to leading the charge.
#include "eval.h"
#include "attacks.h"
#include "magic.h"

static const int material_mg[6] = { 100, 320, 330, 500, 900, 0 };
static const int material_eg[6] = { 120, 300, 330, 520, 920, 0 };

// Phase weights: N/B=1, R=2, Q=4; 24 = full board
static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

// Tables read like a board diagram: rank 8 first.
static const int pst_mg[6][64] = {
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
    { // king: hide behind the pawns
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

// Endgame overrides: pawns race to promote, king centralises.
// Knights/bishops/rooks/queens reuse their middlegame shapes.
static const int pawn_eg[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    80, 80, 80, 80, 80, 80, 80, 80,
    50, 50, 50, 50, 50, 50, 50, 50,
    30, 30, 30, 30, 30, 30, 30, 30,
    15, 15, 15, 15, 15, 15, 15, 15,
     5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

static const int king_eg[64] = {
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50
};

// Passed pawn bonus by relative rank (rank 1 = home, 7 = about to queen)
static const int passed_mg[8] = { 0,  5, 10, 20, 35,  60, 100, 0 };
static const int passed_eg[8] = { 0, 10, 20, 35, 60, 100, 150, 0 };

#define ISOLATED_MG  -10
#define ISOLATED_EG  -15
#define DOUBLED_MG   -10
#define DOUBLED_EG   -20
#define BISHOP_PAIR_MG 30
#define BISHOP_PAIR_EG 50
#define ROOK_OPEN      15
#define ROOK_SEMIOPEN   8
#define SHIELD_BONUS    8

// ---- Precomputed masks ----
static U64 file_mask[8];
static U64 adj_files[8];        // files either side (not the file itself)
static U64 passed_mask[2][64];  // squares that must be clear of enemy pawns
static U64 shield_mask[2][64];  // pawn-shield zone in front of a king

void eval_init(void) {
    for (int f = 0; f < 8; f++) {
        file_mask[f] = 0x0101010101010101ULL << f;
        adj_files[f] = 0;
        if (f > 0) adj_files[f] |= file_mask[f - 1];
        if (f < 7) adj_files[f] |= file_mask[f + 1];
    }
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8, f = sq % 8;
        U64 span = file_mask[f] | adj_files[f];
        U64 ahead_w = 0, ahead_b = 0;
        for (int i = r + 1; i < 8; i++) ahead_w |= 0xFFULL << (i * 8);
        for (int i = 0; i < r; i++)     ahead_b |= 0xFFULL << (i * 8);
        passed_mask[WHITE][sq] = span & ahead_w;
        passed_mask[BLACK][sq] = span & ahead_b;

        // Shield: the three squares one and two ranks in front of the king
        U64 sh_w = 0, sh_b = 0;
        for (int df = -1; df <= 1; df++) {
            int nf = f + df;
            if (nf < 0 || nf > 7) continue;
            if (r + 1 < 8) sh_w |= 1ULL << ((r + 1) * 8 + nf);
            if (r + 2 < 8) sh_w |= 1ULL << ((r + 2) * 8 + nf);
            if (r - 1 >= 0) sh_b |= 1ULL << ((r - 1) * 8 + nf);
            if (r - 2 >= 0) sh_b |= 1ULL << ((r - 2) * 8 + nf);
        }
        shield_mask[WHITE][sq] = sh_w;
        shield_mask[BLACK][sq] = sh_b;
    }
}

#define W_IDX(sq) ((7 - (sq) / 8) * 8 + (sq) % 8)
#define B_IDX(sq) (sq)

int evaluate(const Board *bd) {
    int mg = 0, eg = 0, phase = 0;
    U64 occ = bd->occ[BOTH];
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = side == WHITE ? wp : bp;
        U64 their_pawns = side == WHITE ? bp : wp;

        for (int pt = 0; pt < 6; pt++) {
            U64 b = bd->bb[base + pt];
            while (b) {
                int sq = LSB(b); POP_BIT(b, sq);
                int idx = side == WHITE ? W_IDX(sq) : B_IDX(sq);
                phase += phase_w[pt];

                int m = material_mg[pt] + pst_mg[pt][idx];
                int e = material_eg[pt];
                if (pt == 0)      e += pawn_eg[idx];
                else if (pt == 5) e += king_eg[idx];
                else              e += pst_mg[pt][idx];

                switch (pt) {
                case 0: {  // pawn structure
                    int f = sq % 8;
                    int rel_rank = side == WHITE ? sq / 8 : 7 - sq / 8;
                    if (!(passed_mask[side][sq] & their_pawns)) {
                        m += passed_mg[rel_rank];
                        e += passed_eg[rel_rank];
                    }
                    if (!(adj_files[f] & own_pawns)) {
                        m += ISOLATED_MG; e += ISOLATED_EG;
                    }
                    // Count each extra pawn on the file once (from the rear one)
                    if (passed_mask[side][sq] & file_mask[f] & own_pawns) {
                        m += DOUBLED_MG; e += DOUBLED_EG;
                    }
                    break;
                }
                case 1: {  // knight mobility
                    int mob = COUNT(knight_attacks[sq] & ~own) - 4;
                    m += mob * 4; e += mob * 4;
                    break;
                }
                case 2: {  // bishop mobility
                    int mob = COUNT(get_bishop_attacks(sq, occ) & ~own) - 6;
                    m += mob * 3; e += mob * 3;
                    break;
                }
                case 3: {  // rook mobility + file quality
                    int mob = COUNT(get_rook_attacks(sq, occ) & ~own) - 7;
                    m += mob * 2; e += mob * 4;
                    U64 fm = file_mask[sq % 8];
                    if (!(fm & (wp | bp)))      m += ROOK_OPEN;
                    else if (!(fm & own_pawns)) m += ROOK_SEMIOPEN;
                    break;
                }
                case 5: {  // king shield (middlegame concern only)
                    m += SHIELD_BONUS * COUNT(shield_mask[side][sq] & own_pawns);
                    break;
                }
                }

                mg += sign * m;
                eg += sign * e;
            }
        }
        if (COUNT(bd->bb[base + 2]) >= 2) {  // bishop pair
            mg += sign * BISHOP_PAIR_MG;
            eg += sign * BISHOP_PAIR_EG;
        }
    }

    if (phase > 24) phase = 24;
    int score = (mg * phase + eg * (24 - phase)) / 24;
    return bd->side == WHITE ? score : -score;
}
