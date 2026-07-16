// eval.c — Part 8 (v3): tapered evaluation
// Every term is scored twice — middlegame and endgame — and blended by game
// phase (how much non-pawn material is left), so the king slides smoothly
// from cowering in the corner to leading the charge.
#include <stdlib.h>
#include "eval.h"
#include "attacks.h"
#include "magic.h"

int material_mg[6] = { 100, 320, 330, 500, 900, 0 };
int material_eg[6] = { 120, 300, 330, 520, 920, 0 };

// Tables read like a board diagram: rank 8 first.
int pst_mg[6][64] = {
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
int pawn_eg[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    80, 80, 80, 80, 80, 80, 80, 80,
    50, 50, 50, 50, 50, 50, 50, 50,
    30, 30, 30, 30, 30, 30, 30, 30,
    15, 15, 15, 15, 15, 15, 15, 15,
     5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

int king_eg[64] = {
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
int passed_mg[8] = { 0,  5, 10, 20, 35,  60, 100, 0 };
int passed_eg[8] = { 0, 10, 20, 35, 60, 100, 150, 0 };

// A passer the defender provably can't catch is nearly a queen
int UNSTOPPABLE_EG = 600;

int CHEB(int a, int b) {
    int rank_diff = abs(a/8 - b/8);
    int file_diff = abs(a%8 - b%8);
    return rank_diff > file_diff ? rank_diff : file_diff;
}

// Coordination feature weights (cp per instance): mutual defense, battery,
// outpost, focal pressure, pawn-piece sync. Seeds only set the unit scale —
// magnitudes are learned from game outcomes via the tuning registry.
int coord_w[5] = { 4, 4, 4, 4, 4 };

// Scalar terms are plain ints (uppercase kept from their #define past) so
// the Texel tuner can adjust them through the registry below.
int ISOLATED_MG  = -10;
int ISOLATED_EG  = -15;
int DOUBLED_MG   = -10;
int DOUBLED_EG   = -20;
int BISHOP_PAIR_MG = 30;
int BISHOP_PAIR_EG = 50;
int ROOK_OPEN      = 15;
int ROOK_SEMIOPEN  =  8;
int SHIELD_BONUS   =  8;

// ---- Tuning registry: every weight the Texel tuner may touch ----
// material_mg[0] (the pawn) is the scale anchor and is excluded.
const ParamBlock eval_params[] = {
    { "material_mg+1", material_mg + 1, 5 },   // N B R Q (K stays 0)
    { "material_eg",   material_eg,     5 },
    { "pst_mg[0]", pst_mg[0], 64 }, { "pst_mg[1]", pst_mg[1], 64 },
    { "pst_mg[2]", pst_mg[2], 64 }, { "pst_mg[3]", pst_mg[3], 64 },
    { "pst_mg[4]", pst_mg[4], 64 }, { "pst_mg[5]", pst_mg[5], 64 },
    { "pawn_eg",   pawn_eg,   64 },
    { "king_eg",   king_eg,   64 },
    { "passed_mg+1", passed_mg + 1, 6 },       // ranks 2-7
    { "passed_eg+1", passed_eg + 1, 6 },
    { "ISOLATED_MG", &ISOLATED_MG, 1 }, { "ISOLATED_EG", &ISOLATED_EG, 1 },
    { "DOUBLED_MG",  &DOUBLED_MG,  1 }, { "DOUBLED_EG",  &DOUBLED_EG,  1 },
    { "BISHOP_PAIR_MG", &BISHOP_PAIR_MG, 1 },
    { "BISHOP_PAIR_EG", &BISHOP_PAIR_EG, 1 },
    { "ROOK_OPEN", &ROOK_OPEN, 1 }, { "ROOK_SEMIOPEN", &ROOK_SEMIOPEN, 1 },
    { "SHIELD_BONUS", &SHIELD_BONUS, 1 },
    { "coord_w", coord_w, 5 },
};
const int eval_params_n = sizeof(eval_params) / sizeof(eval_params[0]);

// Load "name index value" weight lines (as written by tune mode). Unknown
// names are ignored so weight files stay usable across small refactors.
#include <stdio.h>
#include <string.h>
int eval_load_weights(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char name[64];
    int idx, val, applied = 0;
    while (fscanf(f, "%63s %d %d", name, &idx, &val) == 3) {
        for (int b = 0; b < eval_params_n; b++) {
            const ParamBlock *pb = &eval_params[b];
            if (strcmp(pb->name, name) == 0 && idx >= 0 && idx < pb->count) {
                pb->ptr[idx] = val;
                applied++;
                break;
            }
        }
    }
    fclose(f);
    return applied;
}

// ---- Precomputed masks ----
U64 file_mask[8];
U64 adj_files[8];        // files either side (not the file itself)
U64 passed_mask[2][64];  // squares that must be clear of enemy pawns
U64 shield_mask[2][64];  // pawn-shield zone in front of a king

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

// King attack: pieces bearing on the ring around the enemy king accumulate
// "attack units" (weighted by piece type); the bonus grows quadratically so
// a coordinated assault scores far more than a lone raider.
int king_atk_weight[6] = { 0, 2, 2, 3, 5, 0 };

// Wrapper: evaluate() now uses strategy-based evaluation
#include "eval_strategy.h"

int evaluate(const Board *bd) {
    static StrategyWeights weights = {0};
    static int initialized = 0;

    if (!initialized) {
        eval_default_strategy_weights(&weights);
        eval_load_strategy_weights("strategy_weights.txt", &weights);
        initialized = 1;
    }

    return eval_with_strategies(bd, &weights);
}
