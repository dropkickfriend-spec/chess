// eval_strategy.c — Complete strategy-based evaluation (replaces evaluate())
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "eval_strategy.h"
#include "eval.h"
#include "attacks.h"
#include "magic.h"

// Globals from eval.c (these define all evaluation constants/tables)
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
extern U64 shield_mask[2][64];
extern int king_atk_weight[6];
extern int CHEB(int a, int b);

static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

GamePhase eval_detect_phase(const Board *bd) {
    int material = 0;
    for (int i = WN; i <= BQ; i++)
        material += COUNT(bd->bb[i]) * phase_w[i % 6];
    
    if (material >= 20) return PHASE_OPENING;
    if (material >= 10) return PHASE_MIDDLEGAME;
    return PHASE_ENDGAME;
}

// Strategy: MATERIAL — base piece values
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

// Strategy: PAWN_STRUCTURE — passed, isolated, doubled pawns
static int eval_pawn_structure(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 own_pawns = side == WHITE ? wp : bp;
        U64 their_pawns = side == WHITE ? bp : wp;
        
        U64 bb = bd->bb[base];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int f = sq % 8;
            int rel_rank = side == WHITE ? sq / 8 : 7 - sq / 8;
            
            // Passed pawn
            if (!(passed_mask[side][sq] & their_pawns)) {
                mg += passed_mg[rel_rank];
                eg += passed_eg[rel_rank];
                
                int enemy_base = side == WHITE ? BP : WP;
                U64 enemy_pieces = bd->bb[enemy_base + 1] | bd->bb[enemy_base + 2]
                                 | bd->bb[enemy_base + 3] | bd->bb[enemy_base + 4];
                int ek = LSB(bd->bb[enemy_base + 5]);
                int ok = LSB(bd->bb[base + 5]);
                int promo_sq = side == WHITE ? 56 + f : f;
                
                if (!enemy_pieces && !(passed_mask[side][sq] & file_mask[f] & occ)) {
                    int steps = 7 - rel_rank - (rel_rank == 1);
                    int kdist = CHEB(ek, promo_sq) - (bd->side != side);
                    if (kdist > steps) eg += UNSTOPPABLE_EG;
                }
                int front = side == WHITE ? sq + 8 : sq - 8;
                eg += 2 * rel_rank * CHEB(ek, front);
                eg -= rel_rank * CHEB(ok, front);
            }
            
            // Isolated pawn
            if (!(adj_files[f] & own_pawns)) {
                mg += ISOLATED_MG; eg += ISOLATED_EG;
            }
            
            // Doubled pawn
            if (passed_mask[side][sq] & file_mask[f] & own_pawns) {
                mg += DOUBLED_MG; eg += DOUBLED_EG;
            }
        }
    }
    
    return (mg * phase + eg * (24 - phase)) / 24;
}

// Strategy: PIECE_ACTIVITY — mobility and positioning
static int eval_piece_activity(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];
    U64 wp = bd->bb[WP], bp = bd->bb[BP];
    
    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = side == WHITE ? wp : bp;
        
        // Knights
        U64 bb = bd->bb[base + 1];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            U64 att = knight_attacks[sq];
            int mob = COUNT(att & ~own) - 4;
            mg += sign * mob * 4;
            eg += sign * mob * 4;
        }
        
        // Bishops
        bb = bd->bb[base + 2];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            U64 att = get_bishop_attacks(sq, occ);
            int mob = COUNT(att & ~own) - 6;
            mg += sign * mob * 3;
            eg += sign * mob * 3;
        }
        
        // Rooks
        bb = bd->bb[base + 3];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            U64 att = get_rook_attacks(sq, occ);
            int mob = COUNT(att & ~own) - 7;
            mg += sign * mob * 2;
            eg += sign * mob * 4;
            
            int f = sq % 8;
            if (!(file_mask[f] & (wp | bp))) mg += sign * ROOK_OPEN;
            else if (!(file_mask[f] & own_pawns)) mg += sign * ROOK_SEMIOPEN;
        }
    }
    
    // Bishop pair
    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        if (COUNT(bd->bb[side * 6 + 2]) >= 2) {
            mg += sign * BISHOP_PAIR_MG;
            eg += sign * BISHOP_PAIR_EG;
        }
    }
    
    return (mg * phase + eg * (24 - phase)) / 24;
}

// Strategy: KING_SAFETY_OPENING — shield and escape squares
static int eval_king_safety(const Board *bd, int phase) {
    int mg = 0;
    U64 wp = bd->bb[WP], bp = bd->bb[BP];

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        U64 own_pawns = side == WHITE ? wp : bp;
        
        // Shield bonus
        U64 king_bb = bd->bb[side == WHITE ? WK : BK];
        if (king_bb) {
            int ksq = LSB(king_bb);
            mg += sign * SHIELD_BONUS * COUNT(shield_mask[side][ksq] & own_pawns);
        }
        
        // Escape square penalty
        if (king_bb) {
            int ksq = LSB(king_bb);
            U64 escape = king_attacks[ksq] & ~bd->occ[side];
            int escape_count = COUNT(escape);
            int king_rank = ksq / 8;
            int back_rank = (side == WHITE) ? 0 : 7;
            
            if (escape_count == 0 && king_rank == back_rank) {
                U64 enemy_rq = bd->bb[side == WHITE ? BR : WR]
                             | bd->bb[side == WHITE ? BQ : WQ];
                if (enemy_rq) mg -= sign * 150;
            } else if (escape_count == 1) {
                mg -= sign * 20;
            }
        }
    }
    
    return mg * phase / 24;  // Only middlegame
}

// Strategy: ATTACK_POTENTIAL — king attack bonus
static int eval_attack_potential(const Board *bd, int phase) {
    int atk_units[2] = { 0, 0 };
    U64 occ = bd->occ[BOTH];
    
    U64 king_zone[2];
    king_zone[WHITE] = bd->bb[WK] ? king_attacks[LSB(bd->bb[WK])] | bd->bb[WK] : 0;
    king_zone[BLACK] = bd->bb[BK] ? king_attacks[LSB(bd->bb[BK])] | bd->bb[BK] : 0;
    
    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 enemy_zone = king_zone[!side];
        
        // Knight attacks
        U64 bb = bd->bb[base + 1];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[1] * COUNT(knight_attacks[sq] & enemy_zone);
        }
        
        // Bishop attacks
        bb = bd->bb[base + 2];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[2] * COUNT(get_bishop_attacks(sq, occ) & enemy_zone);
        }
        
        // Rook attacks
        bb = bd->bb[base + 3];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            atk_units[side] += king_atk_weight[3] * COUNT(get_rook_attacks(sq, occ) & enemy_zone);
        }
        
        // Queen attacks
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
    
    return mg * phase / 24;  // Only middlegame
}

void eval_default_strategy_weights(StrategyWeights *w) {
    for (int i = 0; i < STRAT_COUNT; i++) {
        w->weight[i] = 1.0f;
        w->enabled[i] = 1;
    }
}

int eval_load_strategy_weights(const char *path, StrategyWeights *w) {
    FILE *f = fopen(path, "r");
    if (!f) {
        eval_default_strategy_weights(w);
        return 0;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int idx;
        float wt;
        if (sscanf(line, "weight %d %f", &idx, &wt) == 2 && idx >= 0 && idx < STRAT_COUNT)
            w->weight[idx] = wt;
    }
    fclose(f);
    return 1;
}

int eval_save_strategy_weights(const char *path, const StrategyWeights *w) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    
    for (int i = 0; i < STRAT_COUNT; i++)
        fprintf(f, "weight %d %f\n", i, w->weight[i]);
    fclose(f);
    return 1;
}

// Main strategy-based evaluation
int eval_with_strategies(const Board *bd, const StrategyWeights *w) {
    int phase;
    int score = 0;

    // MATERIAL — always evaluate to get phase, but only add score if enabled
    int material_score = eval_material(bd, &phase);
    if (w->enabled[STRAT_MATERIAL])
        score += (int)(material_score * w->weight[STRAT_MATERIAL]);
    
    // PAWN_STRUCTURE
    if (w->enabled[STRAT_PAWN_STRUCTURE])
        score += (int)(eval_pawn_structure(bd, phase) * w->weight[STRAT_PAWN_STRUCTURE]);
    
    // PIECE_ACTIVITY
    if (w->enabled[STRAT_PIECE_ACTIVITY])
        score += (int)(eval_piece_activity(bd, phase) * w->weight[STRAT_PIECE_ACTIVITY]);
    
    // KING_SAFETY (opening/middlegame)
    if (w->enabled[STRAT_KING_SAFETY_OPENING] && eval_detect_phase(bd) <= PHASE_MIDDLEGAME)
        score += (int)(eval_king_safety(bd, phase) * w->weight[STRAT_KING_SAFETY_OPENING]);
    
    // ATTACK_POTENTIAL
    if (w->enabled[STRAT_ATTACK_POTENTIAL])
        score += (int)(eval_attack_potential(bd, phase) * w->weight[STRAT_ATTACK_POTENTIAL]);
    
    return bd->side == WHITE ? score : -score;
}
