// eval_strategy.c — Granular term-based evaluation
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "eval_strategy.h"
#include "eval.h"
#include "attacks.h"
#include "magic.h"

extern int material_mg[6], material_eg[6];
extern int pst_mg[6][64], pst_eg[6][64];
extern int pawn_eg[64], king_eg[64];
extern int passed_mg[8], passed_eg[8];
extern int ISOLATED_MG, ISOLATED_EG, DOUBLED_MG, DOUBLED_EG;
extern int BISHOP_PAIR_MG, BISHOP_PAIR_EG;
extern int ROOK_OPEN, ROOK_SEMIOPEN;
extern int SHIELD_BONUS;
extern int UNSTOPPABLE_EG;
extern U64 passed_mask[2][64], file_mask[8], adj_files[8];
extern int king_atk_weight[6];
extern int CHEB(int a, int b);

static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

// ---- Individual term evaluation functions ----

// Material: base piece values + PST (tapered)
static int eval_material(const Board *bd, int *phase_out) {
    int mg = 0, eg = 0, phase = 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        for (int pt = 0; pt < 6; pt++) {
            U64 bb = bd->bb[side * 6 + pt];
            while (bb) {
                int sq = LSB(bb); POP_BIT(bb, sq);
                int idx = side == WHITE ? W_IDX(sq) : B_IDX(sq);
                phase += phase_w[pt];

                int m = material_mg[pt] + pst_mg[pt][idx];
                int e = material_eg[pt];
                if (pt == 0) e += pawn_eg[idx];
                else if (pt == 5) e += king_eg[idx];
                else e += pst_mg[pt][idx];

                mg += sign * m;
                eg += sign * e;
            }
        }
    }

    if (phase > 24) phase = 24;
    *phase_out = phase;
    return (mg * phase + eg * (24 - phase)) / 24;
}

// Pawn structure: passed pawns
static int eval_passed_pawns(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 their_pawns = side == WHITE ? bp : wp;

        U64 bb = bd->bb[base];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int f = sq % 8;
            int rel_rank = side == WHITE ? sq / 8 : 7 - sq / 8;

            if (!(passed_mask[side][sq] & their_pawns)) {
                mg += passed_mg[rel_rank];
                eg += passed_eg[rel_rank];

                int enemy_base = side == WHITE ? BP : WP;
                U64 enemy_pieces = bd->bb[enemy_base + 1] | bd->bb[enemy_base + 2]
                                 | bd->bb[enemy_base + 3] | bd->bb[enemy_base + 4];
                int ek = LSB(bd->bb[enemy_base + 5]);
                int promo_sq = side == WHITE ? 56 + f : f;

                if (!enemy_pieces && !(passed_mask[side][sq] & file_mask[f] & occ)) {
                    int steps = 7 - rel_rank - (rel_rank == 1);
                    int kdist = CHEB(ek, promo_sq) - (bd->side != side);
                    if (kdist > steps) eg += UNSTOPPABLE_EG;
                }
                int front = side == WHITE ? sq + 8 : sq - 8;
                eg += 2 * rel_rank * CHEB(ek, front);
                int ok = LSB(bd->bb[base + 5]);
                eg -= rel_rank * CHEB(ok, front);
            }
        }
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Pawn structure: isolated pawns
static int eval_isolated_pawns(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 own_pawns = side == WHITE ? wp : bp;

        U64 bb = bd->bb[base];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int f = sq % 8;

            if (!(adj_files[f] & own_pawns)) {
                mg += ISOLATED_MG;
                eg += ISOLATED_EG;
            }
        }
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Pawn structure: doubled pawns
static int eval_doubled_pawns(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 own_pawns = side == WHITE ? wp : bp;

        U64 bb = bd->bb[base];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int f = sq % 8;

            if (passed_mask[side][sq] & file_mask[f] & own_pawns) {
                mg += DOUBLED_MG;
                eg += DOUBLED_EG;
            }
        }
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Piece mobility: knights
static int eval_knight_mobility(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 own = bd->occ[WHITE];

    U64 bb = bd->bb[WN];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = knight_attacks[sq];
        int mob = COUNT(att & ~own) - 4;
        mg += mob * 4;
        eg += mob * 4;
    }

    own = bd->occ[BLACK];
    bb = bd->bb[BN];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = knight_attacks[sq];
        int mob = COUNT(att & ~own) - 4;
        mg -= mob * 4;
        eg -= mob * 4;
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Piece mobility: bishops
static int eval_bishop_mobility(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];
    U64 own = bd->occ[WHITE];

    U64 bb = bd->bb[WB];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = get_bishop_attacks(sq, occ);
        int mob = COUNT(att & ~own) - 6;
        mg += mob * 3;
        eg += mob * 3;
    }

    own = bd->occ[BLACK];
    bb = bd->bb[BB_];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = get_bishop_attacks(sq, occ);
        int mob = COUNT(att & ~own) - 6;
        mg -= mob * 3;
        eg -= mob * 3;
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Piece mobility: rooks
static int eval_rook_mobility(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];
    U64 own = bd->occ[WHITE];

    U64 bb = bd->bb[WR];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = get_rook_attacks(sq, occ);
        int mob = COUNT(att & ~own) - 7;
        mg += mob * 2;
        eg += mob * 4;
    }

    own = bd->occ[BLACK];
    bb = bd->bb[BR];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = get_rook_attacks(sq, occ);
        int mob = COUNT(att & ~own) - 7;
        mg -= mob * 2;
        eg -= mob * 4;
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Piece mobility: queens
static int eval_queen_mobility(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];
    U64 own = bd->occ[WHITE];

    U64 bb = bd->bb[WQ];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = get_queen_attacks(sq, occ);
        int mob = COUNT(att & ~own) - 13;
        mg += mob * 1;
        eg += mob * 2;
    }

    own = bd->occ[BLACK];
    bb = bd->bb[BQ];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        U64 att = get_queen_attacks(sq, occ);
        int mob = COUNT(att & ~own) - 13;
        mg -= mob * 1;
        eg -= mob * 2;
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Piece positioning: bishop pair
static int eval_bishop_pair(const Board *bd, int phase) {
    int mg = 0, eg = 0;

    if (COUNT(bd->bb[WB]) >= 2) {
        mg += BISHOP_PAIR_MG;
        eg += BISHOP_PAIR_EG;
    }
    if (COUNT(bd->bb[BB_]) >= 2) {
        mg -= BISHOP_PAIR_MG;
        eg -= BISHOP_PAIR_EG;
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Piece positioning: rook open/semiopen files
static int eval_rook_files(const Board *bd, int phase) {
    int mg = 0;
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    U64 bb = bd->bb[WR];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        int f = sq % 8;
        if (!(file_mask[f] & (wp | bp))) mg += ROOK_OPEN;
        else if (!(file_mask[f] & wp)) mg += ROOK_SEMIOPEN;
    }

    bb = bd->bb[BR];
    while (bb) {
        int sq = LSB(bb); POP_BIT(bb, sq);
        int f = sq % 8;
        if (!(file_mask[f] & (wp | bp))) mg -= ROOK_OPEN;
        else if (!(file_mask[f] & bp)) mg -= ROOK_SEMIOPEN;
    }

    return mg * phase / 24;
}

// King safety: shield
static int eval_king_shield(const Board *bd, int phase) {
    int mg = 0;
    U64 wp = bd->bb[WP], bp = bd->bb[BP];
    extern U64 shield_mask[2][64];

    U64 king_bb = bd->bb[WK];
    if (king_bb) {
        int ksq = LSB(king_bb);
        mg += SHIELD_BONUS * COUNT(shield_mask[WHITE][ksq] & wp);
    }

    king_bb = bd->bb[BK];
    if (king_bb) {
        int ksq = LSB(king_bb);
        mg -= SHIELD_BONUS * COUNT(shield_mask[BLACK][ksq] & bp);
    }

    return mg * phase / 24;
}

// King safety: escape squares
static int eval_king_escape_squares(const Board *bd, int phase) {
    int mg = 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        U64 king_bb = bd->bb[side == WHITE ? WK : BK];
        if (king_bb) {
            int ksq = LSB(king_bb);
            U64 escape = king_attacks[ksq] & ~bd->occ[side];
            int escape_count = COUNT(escape);

            if (escape_count == 0) {
                mg += sign * 100;  // No escape squares is bad
            } else if (escape_count == 1) {
                mg += sign * 20;   // One escape square is suboptimal
            }
        }
    }

    return mg * phase / 24;
}

// King safety: back rank trap
static int eval_king_back_rank_trap(const Board *bd, int phase) {
    int mg = 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        U64 king_bb = bd->bb[side == WHITE ? WK : BK];
        if (king_bb) {
            int ksq = LSB(king_bb);
            int king_rank = ksq / 8;
            int back_rank = (side == WHITE) ? 0 : 7;

            if (king_rank == back_rank) {
                U64 escape = king_attacks[ksq] & ~bd->occ[side];
                if (COUNT(escape) == 0) {
                    U64 enemy_rq = bd->bb[side == WHITE ? BR : WR]
                                 | bd->bb[side == WHITE ? BQ : WQ];
                    if (enemy_rq) {
                        mg -= sign * 150;
                    }
                }
            }
        }
    }

    return mg * phase / 24;
}

// King attack: attacking pieces near enemy king
static int eval_king_attack_units(const Board *bd, int phase) {
    int atk_units[2] = { 0, 0 };
    U64 occ = bd->occ[BOTH];

    U64 king_zone[2];
    king_zone[WHITE] = bd->bb[WK] ? king_attacks[LSB(bd->bb[WK])] | bd->bb[WK] : 0;
    king_zone[BLACK] = bd->bb[BK] ? king_attacks[LSB(bd->bb[BK])] | bd->bb[BK] : 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 enemy_zone = king_zone[!side];

        U64 bb = bd->bb[base + 1];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[1] * COUNT(knight_attacks[sq] & enemy_zone);
        }

        bb = bd->bb[base + 2];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[2] * COUNT(get_bishop_attacks(sq, occ) & enemy_zone);
        }

        bb = bd->bb[base + 3];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[3] * COUNT(get_rook_attacks(sq, occ) & enemy_zone);
        }

        bb = bd->bb[base + 4];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[4] * COUNT(get_queen_attacks(sq, occ) & enemy_zone);
        }
    }

    int mg = 0;
    for (int side = WHITE; side <= BLACK; side++) {
        int u = atk_units[side];
        if (u > 40) u = 40;
        mg += (side == WHITE ? 1 : -1) * (u * u / 4);
    }

    return mg * phase / 24;
}

// ---- Weight management ----

void eval_default_strategy_weights(EvalWeights *w) {
    for (int i = 0; i < WEIGHT_COUNT; i++) {
        w->weight[i] = 1.0f;
    }
}

int eval_load_strategy_weights(const char *path, EvalWeights *w) {
    FILE *f = fopen(path, "r");
    if (!f) {
        eval_default_strategy_weights(w);
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int idx;
        float wt;
        if (sscanf(line, "weight %d %f", &idx, &wt) == 2 && idx >= 0 && idx < WEIGHT_COUNT)
            w->weight[idx] = wt;
    }
    fclose(f);
    return 1;
}

int eval_save_strategy_weights(const char *path, const EvalWeights *w) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    for (int i = 0; i < WEIGHT_COUNT; i++)
        fprintf(f, "weight %d %f\n", i, w->weight[i]);
    fclose(f);
    return 1;
}

// ---- Main evaluation: sum all weighted terms ----

int eval_with_strategies(const Board *bd, const EvalWeights *w) {
    int phase;
    int score = 0;

    // Material (always evaluate to get phase)
    int material_score = eval_material(bd, &phase);
    score += (int)(material_score * w->weight[WEIGHT_MATERIAL]);

    // Pawn structure
    score += (int)(eval_passed_pawns(bd, phase) * w->weight[WEIGHT_PASSED_PAWN]);
    score += (int)(eval_isolated_pawns(bd, phase) * w->weight[WEIGHT_ISOLATED_PAWN]);
    score += (int)(eval_doubled_pawns(bd, phase) * w->weight[WEIGHT_DOUBLED_PAWN]);

    // Piece mobility
    score += (int)(eval_knight_mobility(bd, phase) * w->weight[WEIGHT_KNIGHT_MOBILITY]);
    score += (int)(eval_bishop_mobility(bd, phase) * w->weight[WEIGHT_BISHOP_MOBILITY]);
    score += (int)(eval_rook_mobility(bd, phase) * w->weight[WEIGHT_ROOK_MOBILITY]);
    score += (int)(eval_queen_mobility(bd, phase) * w->weight[WEIGHT_QUEEN_MOBILITY]);

    // Piece positioning
    score += (int)(eval_bishop_pair(bd, phase) * w->weight[WEIGHT_BISHOP_PAIR]);
    score += (int)(eval_rook_files(bd, phase) * w->weight[WEIGHT_ROOK_OPEN_FILE]);

    // King safety
    score += (int)(eval_king_shield(bd, phase) * w->weight[WEIGHT_KING_SHIELD]);
    score += (int)(eval_king_escape_squares(bd, phase) * w->weight[WEIGHT_KING_ESCAPE_SQUARES]);
    score += (int)(eval_king_back_rank_trap(bd, phase) * w->weight[WEIGHT_KING_BACK_RANK_TRAP]);

    // King attack
    score += (int)(eval_king_attack_units(bd, phase) * w->weight[WEIGHT_KING_ATTACK_UNITS]);

    return bd->side == WHITE ? score : -score;
}
