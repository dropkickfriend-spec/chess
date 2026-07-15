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

// Phase weights: N/B=1, R=2, Q=4; 24 = full board
static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

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
#define UNSTOPPABLE_EG 600

#define CHEB(a, b) ((abs((a)/8 - (b)/8) > abs((a)%8 - (b)%8)) \
                    ? abs((a)/8 - (b)/8) : abs((a)%8 - (b)%8))

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

// King attack: pieces bearing on the ring around the enemy king accumulate
// "attack units" (weighted by piece type); the bonus grows quadratically so
// a coordinated assault scores far more than a lone raider.
static const int king_atk_weight[6] = { 0, 2, 2, 3, 5, 0 };

int evaluate(const Board *bd) {
    int mg = 0, eg = 0, phase = 0;
    int atk_units[2] = { 0, 0 };
    U64 occ = bd->occ[BOTH];
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    U64 king_zone[2];  // ring around each side's OWN king
    king_zone[WHITE] = bd->bb[WK] ? king_attacks[LSB(bd->bb[WK])] | bd->bb[WK] : 0;
    king_zone[BLACK] = bd->bb[BK] ? king_attacks[LSB(bd->bb[BK])] | bd->bb[BK] : 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = side == WHITE ? wp : bp;
        U64 their_pawns = side == WHITE ? bp : wp;
        U64 enemy_zone = king_zone[!side];

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

                        int enemy_base = side == WHITE ? BP : WP;
                        U64 enemy_pieces = bd->bb[enemy_base + 1] | bd->bb[enemy_base + 2]
                                         | bd->bb[enemy_base + 3] | bd->bb[enemy_base + 4];
                        int ek = LSB(bd->bb[enemy_base + 5]);
                        int ok = LSB(bd->bb[base + 5]);
                        int promo_sq = side == WHITE ? 56 + f : f;

                        // Rule of the square: with only a king to defend, the
                        // pawn promotes iff that king can't reach the
                        // promotion square in time (side to move = one tempo).
                        // Exact only when nothing blocks the pawn's path.
                        if (!enemy_pieces
                            && !(passed_mask[side][sq] & file_mask[f] & occ)) {
                            int steps = 7 - rel_rank - (rel_rank == 1);
                            int kdist = CHEB(ek, promo_sq) - (bd->side != side);
                            if (kdist > steps)
                                e += UNSTOPPABLE_EG;
                        }
                        // Otherwise judge the race by king proximity to the
                        // pawn's stop square, scaled by how advanced it is
                        int front = side == WHITE ? sq + 8 : sq - 8;
                        e += 2 * rel_rank * CHEB(ek, front);
                        e -= rel_rank * CHEB(ok, front);
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
                    U64 att = knight_attacks[sq];
                    int mob = COUNT(att & ~own) - 4;
                    m += mob * 4; e += mob * 4;
                    atk_units[side] += king_atk_weight[1] * COUNT(att & enemy_zone);
                    break;
                }
                case 2: {  // bishop mobility
                    U64 att = get_bishop_attacks(sq, occ);
                    int mob = COUNT(att & ~own) - 6;
                    m += mob * 3; e += mob * 3;
                    atk_units[side] += king_atk_weight[2] * COUNT(att & enemy_zone);
                    break;
                }
                case 3: {  // rook mobility + file quality
                    U64 att = get_rook_attacks(sq, occ);
                    int mob = COUNT(att & ~own) - 7;
                    m += mob * 2; e += mob * 4;
                    atk_units[side] += king_atk_weight[3] * COUNT(att & enemy_zone);
                    U64 fm = file_mask[sq % 8];
                    if (!(fm & (wp | bp)))      m += ROOK_OPEN;
                    else if (!(fm & own_pawns)) m += ROOK_SEMIOPEN;
                    break;
                }
                case 4: {  // queen: only the king attack matters here
                    atk_units[side] += king_atk_weight[4]
                        * COUNT(get_queen_attacks(sq, occ) & enemy_zone);
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

    // Quadratic king-attack bonus, middlegame only (kings fight in endings)
    for (int side = WHITE; side <= BLACK; side++) {
        int u = atk_units[side];
        if (u > 40) u = 40;
        mg += (side == WHITE ? 1 : -1) * (u * u / 4);

        // Penalize king with few/no escape squares, especially on back rank
        U64 king_bb = bd->bb[side == WHITE ? WK : BK];
        if (king_bb) {
            int ksq = LSB(king_bb);
            U64 escape_sq = king_attacks[ksq] & ~bd->occ[side];
            int escape_count = COUNT(escape_sq);
            int king_rank = ksq / 8;
            int back_rank = (side == WHITE) ? 0 : 7;

            // Heavy penalty if king trapped on back rank with rooks/queens nearby
            if (escape_count == 0 && king_rank == back_rank) {
                U64 enemy_rq = bd->bb[side == WHITE ? BR : WR]
                             | bd->bb[side == WHITE ? BQ : WQ];
                if (enemy_rq) {
                    int penalty = 150;  // very strong penalty for back-rank trap
                    mg -= (side == WHITE ? 1 : -1) * penalty;
                }
            }
            // Milder penalty for king with 1 escape square (still restricted)
            else if (escape_count == 1) {
                int penalty = 20;
                mg -= (side == WHITE ? 1 : -1) * penalty;
            }
        }
    }

    if (phase > 24) phase = 24;
    int score = (mg * phase + eg * (24 - phase)) / 24;
    return bd->side == WHITE ? score : -score;
}

// ---- Mate Certainty System: Dynamic evaluation based on mating patterns ----

// Analyze opponent defenders for a threatened square: can they defend?
// Returns logistics analysis showing how many defenders are truly reachable.
static DefenderAnalysis analyze_defenders(const Board *bd, int threatened_sq, int defender_side) {
    DefenderAnalysis da = {0, 0, 0, 0, 0};
    U64 own_occ = bd->occ[defender_side];
    U64 enemy_occ = bd->occ[!defender_side];
    U64 all_occ = own_occ | enemy_occ;

    // Pieces that could theoretically defend: rooks, queens, knights, bishops, pawns
    U64 potential_defenders = 0;
    int base = defender_side == WHITE ? WR : BR;
    potential_defenders |= bd->bb[base];     // rooks
    potential_defenders |= bd->bb[base + 1]; // queens
    potential_defenders |= bd->bb[base - 2]; // knights
    potential_defenders |= bd->bb[base - 3]; // bishops

    da.total_potential = COUNT(potential_defenders);

    while (potential_defenders) {
        int sq = LSB(potential_defenders);
        POP_BIT(potential_defenders, sq);

        int piece = 0;
        for (int i = 1; i < 5; i++) {
            int p = base + (defender_side == WHITE ? i : i - 5);
            if (p < 0 || p >= 12) continue;
            if (bd->bb[p] & (1ULL << sq)) {
                piece = p;
                break;
            }
        }
        if (!piece) continue;

        U64 piece_attacks = 0;
        int piece_type = piece % 6;

        if (piece_type == 3)      // rook
            piece_attacks = get_rook_attacks(sq, all_occ);
        else if (piece_type == 4) // queen
            piece_attacks = get_queen_attacks(sq, all_occ);
        else if (piece_type == 1) // knight
            piece_attacks = knight_attacks[sq];
        else if (piece_type == 2) // bishop
            piece_attacks = get_bishop_attacks(sq, all_occ);

        if (piece_attacks & (1ULL << threatened_sq)) {
            // Can potentially defend
            da.actually_reachable++;

            // Check if path is blocked by own pieces
            if (piece_type == 3 || piece_type == 4) {
                U64 path = (piece_type == 3) ?
                    get_rook_attacks(sq, all_occ & ~(1ULL << sq)) :
                    get_queen_attacks(sq, all_occ & ~(1ULL << sq));
                if (!(path & (1ULL << threatened_sq))) {
                    da.blocked_by_own++;
                }
            }

            // Check if our pieces control their path
            U64 our_controlled = 0;
            for (int our_sq = 0; our_sq < 64; our_sq++) {
                int our_piece = 0;
                for (int p = 0; p < 12; p++) {
                    if (!(bd->bb[p] & (1ULL << our_sq))) continue;
                    if (p / 6 == !defender_side) {
                        our_piece = p;
                        break;
                    }
                }
                if (!our_piece) continue;

                U64 our_attacks = 0;
                int our_type = our_piece % 6;
                if (our_type == 0) continue; // skip pawns for now
                else if (our_type == 1)
                    our_attacks = knight_attacks[our_sq];
                else if (our_type == 2)
                    our_attacks = get_bishop_attacks(our_sq, all_occ);
                else if (our_type == 3)
                    our_attacks = get_rook_attacks(our_sq, all_occ);
                else if (our_type == 4)
                    our_attacks = get_queen_attacks(our_sq, all_occ);
                else if (our_type == 5)
                    our_attacks = king_attacks[our_sq];

                our_controlled |= our_attacks;
            }

            if (piece_type == 3 || piece_type == 4) {
                U64 path_squares = get_rook_attacks(sq, 0) & get_rook_attacks(threatened_sq, 0);
                if (path_squares & our_controlled) {
                    da.blocked_by_our_control++;
                }
            }
        }
    }

    return da;
}

// Compute mate certainty from position features (0-100%)
// Returns context with base certainty and phase classification.
MateContext eval_compute_mate_context(const Board *bd) {
    MateContext mc = {0, 0, 0, 0};
    int us = bd->side;
    int them = !us;

    // Phase: determine where we are in the mating attack
    // 0: normal (material equal/ahead, no immediate threats)
    // 1: attacking (material roughly equal, we have initiative)
    // 2: mating (we have significant material + initiative)
    // 3: forcing (checkmate sequences or forced wins)

    // Evaluate material and position for base_certainty
    // Feature 1-3: Material count for each side
    int our_material = 0, their_material = 0;
    for (int i = 1; i < 5; i++) {
        our_material += COUNT(bd->bb[us == WHITE ? (WP + i) : (BP + i)]) * material_mg[i];
        their_material += COUNT(bd->bb[them == WHITE ? (WP + i) : (BP + i)]) * material_mg[i];
    }

    // Feature 4: Material imbalance (positive = we're up material)
    float material_advantage = (our_material - their_material) / 1000.0f;

    // Feature 5-6: King safety (escape squares for each side)
    int our_king_sq = LSB(bd->bb[us == WHITE ? WK : BK]);
    int their_king_sq = LSB(bd->bb[them == WHITE ? WK : BK]);
    int our_escapes = COUNT(king_attacks[our_king_sq] & ~bd->occ[us]);
    int their_escapes = COUNT(king_attacks[their_king_sq] & ~bd->occ[them]);

    // Feature 7: Enemy king exposure
    float enemy_exposure = (4 - their_escapes) / 4.0f;

    // Feature 8: Our king safety
    float our_safety = our_escapes / 4.0f;

    // Feature 9-10: Piece activity/mobility (knight + bishop mobility)
    U64 occ = bd->occ[BOTH];
    int our_mobility = 0, their_mobility = 0;

    for (int side = 0; side < 2; side++) {
        int base = side == WHITE ? WP : BP;
        for (int pt = 1; pt <= 2; pt++) {
            U64 pieces = bd->bb[base + pt];
            while (pieces) {
                int sq = LSB(pieces);
                POP_BIT(pieces, sq);
                U64 att = (pt == 1) ? knight_attacks[sq] :
                         get_bishop_attacks(sq, occ);
                int mob = COUNT(att & ~bd->occ[side]);
                if (side == us) our_mobility += mob;
                else their_mobility += mob;
            }
        }
    }

    // Feature 11-12: Rook/queen activity
    int our_rook_activity = 0, their_rook_activity = 0;
    for (int side = 0; side < 2; side++) {
        int base = side == WHITE ? WP : BP;
        U64 rooks = bd->bb[base + 3];
        U64 queens = bd->bb[base + 4];
        while (rooks) {
            int sq = LSB(rooks);
            POP_BIT(rooks, sq);
            int mob = COUNT(get_rook_attacks(sq, occ) & ~bd->occ[side]);
            if (side == us) our_rook_activity += mob;
            else their_rook_activity += mob;
        }
        while (queens) {
            int sq = LSB(queens);
            POP_BIT(queens, sq);
            int mob = COUNT(get_queen_attacks(sq, occ) & ~bd->occ[side]);
            if (side == us) our_rook_activity += mob * 2;
            else their_rook_activity += mob * 2;
        }
    }

    // Feature 13: Attacking pieces near enemy king
    int attacking_pieces = 0;
    U64 enemy_zone = king_attacks[their_king_sq] | (1ULL << their_king_sq);
    for (int pt = 1; pt < 5; pt++) {
        U64 pieces = bd->bb[us == WHITE ? (WP + pt) : (BP + pt)];
        while (pieces) {
            int sq = LSB(pieces);
            POP_BIT(pieces, sq);
            U64 att = 0;
            if (pt == 1) att = knight_attacks[sq];
            else if (pt == 2) att = get_bishop_attacks(sq, occ);
            else if (pt == 3) att = get_rook_attacks(sq, occ);
            else if (pt == 4) att = get_queen_attacks(sq, occ);
            if (att & enemy_zone) attacking_pieces++;
        }
    }

    // Feature 14: Defender analysis (piece routing logistics)
    DefenderAnalysis da = analyze_defenders(bd, their_king_sq, them);
    float defender_availability = da.total_potential > 0 ?
        (1.0f - (float)da.actually_reachable / da.total_potential) : 0.5f;

    // Feature 15: Passed pawns
    int our_passed = 0, their_passed = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (bd->bb[WP] & (1ULL << sq)) {
            if (!(passed_mask[WHITE][sq] & bd->bb[BP]))
                our_passed++;
        }
        if (bd->bb[BP] & (1ULL << sq)) {
            if (!(passed_mask[BLACK][sq] & bd->bb[WP]))
                their_passed++;
        }
    }

    // Feature 17-18: Tempo and inititative (measured by attack vs defense balance)
    float tempo_score = (our_rook_activity - their_rook_activity) / 10.0f;
    float initiative = (attacking_pieces > their_material / 500) ? 1.0f : 0.0f;

    // Compute base certainty from features
    mc.base_certainty = 50.0f;  // neutral baseline

    // Add material advantage
    mc.base_certainty += material_advantage * 20.0f;

    // Heavily weight king safety differential
    mc.base_certainty += (enemy_exposure - our_safety) * 30.0f;

    // Add mobility advantage
    mc.base_certainty += ((our_mobility + our_rook_activity) -
                         (their_mobility + their_rook_activity)) / 10.0f;

    // Add attacking piece coordination
    mc.base_certainty += attacking_pieces * 5.0f;

    // Penalize if defenders are actually reachable
    mc.base_certainty -= defender_availability * 20.0f;

    // Add passed pawn advantage
    mc.base_certainty += (our_passed - their_passed) * 10.0f;

    // Add tempo/initiative
    mc.base_certainty += tempo_score * 5.0f;
    mc.base_certainty += initiative * 15.0f;

    // Clamp to 0-100
    if (mc.base_certainty < 0) mc.base_certainty = 0;
    if (mc.base_certainty > 100) mc.base_certainty = 100;

    // Determine phase based on certainty level
    if (mc.base_certainty < 20)
        mc.phase = 0;  // normal
    else if (mc.base_certainty < 70)
        mc.phase = 1;  // attacking
    else if (mc.base_certainty < 95)
        mc.phase = 2;  // mating
    else
        mc.phase = 3;  // checkmate forcing

    mc.total_certainty = mc.base_certainty + mc.move_reinforcement;
    if (mc.total_certainty > 100) mc.total_certainty = 100;

    return mc;
}
