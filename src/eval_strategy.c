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
        
        // Bishops: mobility plus the pawn-complex test — a bishop is worth
        // more when the opponent's pawns sit on its colour (fixed targets)
        // and less when our own pawns share its colour (bad bishop).
        U64 their_pawns = side == WHITE ? bp : wp;
        bb = bd->bb[base + 2];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            U64 att = get_bishop_attacks(sq, occ);
            int mob = COUNT(att & ~own) - 6;
            mg += sign * mob * 3;
            eg += sign * mob * 3;

            const U64 LIGHT = 0x55AA55AA55AA55AAULL;
            U64 same_colour = GET_BIT(LIGHT, sq) ? LIGHT : ~LIGHT;
            int targets  = COUNT(their_pawns & same_colour);
            int blockers = COUNT(own_pawns  & same_colour);
            mg += sign * (targets * 2 - blockers * 3);
            eg += sign * (targets * 3 - blockers * 5);
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

// Strategy: DEFENDER_LOGISTICS — the logistical freedom our own men deny
// each other; a main purpose of any move is releasing it. For every
// bishop/rook/queen we compare actual mobility against mobility with (a)
// our pawns lifted and (b) all our men lifted: the gaps are latent activity
// that a pawn push or a piece stepping aside would unlock. Pawn blockage
// costs full price (pawns are slow, semi-permanent walls); piece blockage
// half (a friend can vacate next move). Every line-opening move — e4
// freeing the f1-bishop, a knight leaving the c1-bishop's diagonal — gains
// through this term, and its learned strategy weight can rise to supersede
// the others in the relative-weight budget when outcomes back it.
static int eval_pawn_logistics(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = bd->bb[base];
        U64 occ_np   = occ & ~own_pawns;   // our pawns lifted
        U64 occ_free = occ & ~own;         // all our men lifted

        // Bishops: sealed diagonals hurt most in the middlegame
        U64 bb = bd->bb[base + 2];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int now = COUNT(get_bishop_attacks(sq, occ));
            int np  = COUNT(get_bishop_attacks(sq, occ_np));
            int fr  = COUNT(get_bishop_attacks(sq, occ_free));
            int by_pawn = np - now, by_piece = fr - np;
            mg -= sign * (by_pawn * 3 + (by_piece * 3 + 1) / 2);
            eg -= sign * (by_pawn * 2 + by_piece);
        }

        // Rooks: plugged files/ranks
        bb = bd->bb[base + 3];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int now = COUNT(get_rook_attacks(sq, occ));
            int np  = COUNT(get_rook_attacks(sq, occ_np));
            int fr  = COUNT(get_rook_attacks(sq, occ_free));
            int by_pawn = np - now, by_piece = fr - np;
            mg -= sign * (by_pawn * 2 + by_piece);
            eg -= sign * (by_pawn * 2 + by_piece);
        }

        // Queens: half weight, a queen usually has alternate routes
        bb = bd->bb[base + 4];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int now = COUNT(get_queen_attacks(sq, occ));
            int np  = COUNT(get_queen_attacks(sq, occ_np));
            int fr  = COUNT(get_queen_attacks(sq, occ_free));
            int by_pawn = np - now, by_piece = (fr - np + 1) / 2;
            mg -= sign * (by_pawn + by_piece);
            eg -= sign * (by_pawn + by_piece);
        }

        // Knights: own men squatting on landing squares (pawns full price,
        // pieces half — they can step aside)
        bb = bd->bb[base + 1];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int crowd_pawn  = COUNT(knight_attacks[sq] & own_pawns);
            int crowd_piece = COUNT(knight_attacks[sq] & own & ~own_pawns);
            mg -= sign * (crowd_pawn * 2 + crowd_piece);
            eg -= sign * crowd_pawn;
        }
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Strategy: PAWN_PROMOTION — promotion certainty of each passed pawn:
// quadratic by rank, doubled with a clear path, plus a rule-of-square
// bonus when the defending king can't catch it. Split out of logistics
// so the per-move reweighting balances "this push cramps my pieces"
// against "this push gets closer to queening" through relative weights.
static int eval_pawn_promotion(const Board *bd, int phase) {
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        int foe  = side == WHITE ? BP : WP;
        U64 their_pawns = bd->bb[foe];

        U64 bb = bd->bb[base];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            if (passed_mask[side][sq] & their_pawns) continue;  // not passed

            int f = sq % 8;
            int rel_rank = side == WHITE ? sq / 8 : 7 - sq / 8;
            int cert = rel_rank * rel_rank;              // 1,4,9,16,25,36
            if (!(passed_mask[side][sq] & file_mask[f] & occ))
                cert *= 2;                               // path already clear

            int promo_sq = side == WHITE ? 56 + f : f;
            int ek = LSB(bd->bb[foe + 5]);
            int steps = 7 - rel_rank - (rel_rank == 1);
            if (CHEB(ek, promo_sq) - (bd->side != side) > steps)
                cert += 40;                              // king can't catch it

            mg += sign * cert / 2;
            eg += sign * cert;
        }
    }

    return (mg * phase + eg * (24 - phase)) / 24;
}

// Strategy: DEVELOPMENT — minors off their home squares, no early queen
// sorties while behind in development, castled king credit. Opening only.
static int eval_development(const Board *bd, int phase) {
    int mg = 0;
    const U64 W_HOME = (1ULL<<B1)|(1ULL<<C1)|(1ULL<<F1)|(1ULL<<G1);
    const U64 B_HOME = (1ULL<<B8)|(1ULL<<C8)|(1ULL<<F8)|(1ULL<<G8);

    int w_home = COUNT((bd->bb[WN] | bd->bb[WB]) & W_HOME);
    int b_home = COUNT((bd->bb[BN] | bd->bb[BB_]) & B_HOME);
    mg -= 12 * w_home;
    mg += 12 * b_home;

    if (w_home >= 2 && bd->bb[WQ] && !(bd->bb[WQ] & (1ULL<<D1))) mg -= 15;
    if (b_home >= 2 && bd->bb[BQ] && !(bd->bb[BQ] & (1ULL<<D8))) mg += 15;

    if (bd->bb[WK] & ((1ULL<<G1)|(1ULL<<C1))) mg += 20;
    if (bd->bb[BK] & ((1ULL<<G8)|(1ULL<<C8))) mg -= 20;

    return mg * phase / 24;
}

// Strategy: CENTER_CONTROL — occupation of and attacks on d4/e4/d5/e5.
static int eval_center_control(const Board *bd, int phase) {
    const U64 CENTER = (1ULL<<27)|(1ULL<<28)|(1ULL<<35)|(1ULL<<36);
    int mg = 8 * (COUNT(bd->bb[WP] & CENTER) - COUNT(bd->bb[BP] & CENTER));

    U64 c = CENTER;
    while (c) {
        int sq = LSB(c); POP_BIT(c, sq);
        mg += 3 * COUNT(pawn_attacks[BLACK][sq] & bd->bb[WP]);
        mg -= 3 * COUNT(pawn_attacks[WHITE][sq] & bd->bb[BP]);
        mg += 2 * COUNT(knight_attacks[sq] & bd->bb[WN]);
        mg -= 2 * COUNT(knight_attacks[sq] & bd->bb[BN]);
    }
    return mg * phase / 24;
}

// Strategy: KING_ACTIVITY — endgame king centralisation and pawn proximity.
static int eval_king_activity(const Board *bd, int phase) {
    if (!bd->bb[WK] || !bd->bb[BK]) return 0;
    int wk = LSB(bd->bb[WK]), bk = LSB(bd->bb[BK]);

    int wc = CHEB(wk, 27) < CHEB(wk, 36) ? CHEB(wk, 27) : CHEB(wk, 36);
    int bc = CHEB(bk, 27) < CHEB(bk, 36) ? CHEB(bk, 27) : CHEB(bk, 36);
    int eg = 6 * (bc - wc);

    U64 p = bd->bb[WP] | bd->bb[BP];
    while (p) {
        int sq = LSB(p); POP_BIT(p, sq);
        eg += 2 * (CHEB(bk, sq) - CHEB(wk, sq));
    }
    return eg * (24 - phase) / 24;
}

// Strategy: OPPOSITION — in pure pawn endings, direct/diagonal opposition
// belongs to the side NOT to move (the mover must give way).
static int eval_opposition(const Board *bd, int phase) {
    U64 pieces = bd->bb[WN]|bd->bb[WB]|bd->bb[WR]|bd->bb[WQ]
               | bd->bb[BN]|bd->bb[BB_]|bd->bb[BR]|bd->bb[BQ];
    if (pieces || !bd->bb[WK] || !bd->bb[BK]) return 0;

    int wk = LSB(bd->bb[WK]), bk = LSB(bd->bb[BK]);
    int rd = abs(wk/8 - bk/8), fd = abs(wk%8 - bk%8);
    int eg = 0;
    if ((rd == 2 && fd == 0) || (rd == 0 && fd == 2) || (rd == 2 && fd == 2))
        eg = bd->side == WHITE ? -25 : 25;

    return eg * (24 - phase) / 24;
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

const char *strategy_names[STRAT_COUNT] = {
    "DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
    "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE",
    "DEFENDER_LOGISTICS", "KING_ACTIVITY", "PAWN_PROMOTION",
    "OPPOSITION", "MATERIAL", "COORDINATION"
};

extern int coord_w[COORD_N];

// Strategy: COORDINATION — pieces working together, five patterns each
// priced by its own learnable weight (coord_w, tuned from outcomes):
//   0 mutual defense: non-pawn pieces covered by a friendly attacker
//   1 batteries: R/Q doubled on a file, B/Q sharing a live diagonal
//   2 outposts: N/B on a pawn-guarded square no enemy pawn can ever attack
//   3 focal pressure: enemy-occupied squares hit by 2+ of our pieces
//   4 pawn-piece sync: pawn advance squares covered by our own pieces —
//     a push (like e4) is worth more when the army stands behind it
static int eval_coordination(const Board *bd, int phase) {
    (void)phase;
    int score = 0;
    U64 occ = bd->occ[BOTH];

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = bd->bb[base];
        U64 enemy = bd->occ[!side];
        U64 enemy_pawns = bd->bb[side == WHITE ? BP : WP];

        U64 pawn_cover = 0;
        U64 pb = own_pawns;
        while (pb) {
            int s = LSB(pb); POP_BIT(pb, s);
            pawn_cover |= pawn_attacks[side][s];
        }

        U64 all_att = pawn_cover;      // union of our attack coverage
        int hits[64] = {0};            // attackers per enemy-occupied square
        int batteries = 0, outposts = 0;

        for (int pt = 1; pt <= 5; pt++) {
            U64 b = bd->bb[base + pt];
            while (b) {
                int sq = LSB(b); POP_BIT(b, sq);
                U64 att;
                switch (pt) {
                    case 1: att = knight_attacks[sq]; break;
                    case 2: att = get_bishop_attacks(sq, occ); break;
                    case 3: att = get_rook_attacks(sq, occ); break;
                    case 4: att = get_queen_attacks(sq, occ); break;
                    default: att = king_attacks[sq]; break;
                }
                all_att |= att;

                U64 t = att & enemy;
                while (t) { int e = LSB(t); POP_BIT(t, e); hits[e]++; }

                // Batteries: rook sees own rook/queen down its file;
                // bishop sees own queen down its diagonal.
                if (pt == 3 && (get_rook_attacks(sq, occ) & file_mask[sq % 8]
                                & (bd->bb[base + 3] | bd->bb[base + 4])))
                    batteries++;
                if (pt == 2 && (get_bishop_attacks(sq, occ) & bd->bb[base + 4]))
                    batteries++;

                // Outposts: minor piece on a pawn-guarded square, past the
                // reach of every enemy pawn on the adjacent files.
                if ((pt == 1 || pt == 2)
                    && (pawn_cover & (1ULL << sq))
                    && !(passed_mask[side][sq] & adj_files[sq % 8] & enemy_pawns)) {
                    int rel_rank = side == WHITE ? sq / 8 : 7 - sq / 8;
                    if (rel_rank >= 3 && rel_rank <= 5) outposts++;
                }
            }
        }

        int mutual = COUNT(all_att & own & ~own_pawns);

        int focal = 0;
        U64 e = enemy;
        while (e) {
            int s = LSB(e); POP_BIT(e, s);
            if (hits[s] > 1) focal += hits[s] - 1;
        }

        int pawn_sync = 0;
        pb = own_pawns;
        while (pb) {
            int s = LSB(pb); POP_BIT(pb, s);
            int front = side == WHITE ? s + 8 : s - 8;
            if (front >= 0 && front < 64 && (all_att & (1ULL << front)))
                pawn_sync++;
        }

        score += sign * (coord_w[0] * mutual
                       + coord_w[1] * batteries
                       + coord_w[2] * outposts
                       + coord_w[3] * focal
                       + coord_w[4] * pawn_sync);
    }

    return score;
}

// Shared core: raw per-strategy scores (White POV, danger discount applied)
// and the phase-weighted mean of enabled weights.
static void gather_raw(const Board *bd, int *phase_out, int raw[STRAT_COUNT]) {
    int phase;
    raw[STRAT_MATERIAL]            = eval_material(bd, &phase);
    raw[STRAT_PAWN_STRUCTURE]      = eval_pawn_structure(bd, phase);
    raw[STRAT_PIECE_ACTIVITY]      = eval_piece_activity(bd, phase);
    raw[STRAT_KING_SAFETY_OPENING] = eval_king_safety(bd, phase);
    raw[STRAT_ATTACK_POTENTIAL]    = eval_attack_potential(bd, phase);
    raw[STRAT_DEFENDER_LOGISTICS]  = eval_pawn_logistics(bd, phase);
    raw[STRAT_DEVELOPMENT]         = eval_development(bd, phase);
    raw[STRAT_CENTER_CONTROL]      = eval_center_control(bd, phase);
    raw[STRAT_KING_ACTIVITY]       = eval_king_activity(bd, phase);
    raw[STRAT_OPPOSITION]          = eval_opposition(bd, phase);
    raw[STRAT_PAWN_PROMOTION]      = eval_pawn_promotion(bd, phase);
    raw[STRAT_COORDINATION]        = eval_coordination(bd, phase);

    // A queen is worth less when someone is getting mated — yours if the
    // attack is on you, theirs if you can throw it at their king. Material's
    // say shrinks as king danger (either side's, either sign) grows.
    int danger = abs(raw[STRAT_KING_SAFETY_OPENING])
               + abs(raw[STRAT_ATTACK_POTENTIAL]);
    if (danger > 400) danger = 400;
    raw[STRAT_MATERIAL] = raw[STRAT_MATERIAL] * 400 / (400 + danger);

    *phase_out = phase;
}

static float weight_mean(const StrategyWeights *w, int phase,
                         float eff_out[STRAT_COUNT]) {
    // Per-move activity: how much of each strategy participates right now.
    // Phase-tapered terms only claim their share of the weight budget.
    float po = (float)phase / 24.0f, pe = 1.0f - po;
    float act[STRAT_COUNT];
    act[STRAT_MATERIAL]            = 1.0f;
    act[STRAT_PAWN_STRUCTURE]      = 1.0f;
    act[STRAT_PIECE_ACTIVITY]      = 1.0f;
    act[STRAT_DEFENDER_LOGISTICS]  = 1.0f;
    act[STRAT_COORDINATION]        = 1.0f;
    act[STRAT_KING_SAFETY_OPENING] = po;
    act[STRAT_ATTACK_POTENTIAL]    = po;
    act[STRAT_DEVELOPMENT]         = po;
    act[STRAT_CENTER_CONTROL]      = po;
    act[STRAT_KING_ACTIVITY]       = pe;
    act[STRAT_OPPOSITION]          = pe;
    act[STRAT_PAWN_PROMOTION]      = pe;

    float wsum = 0.0f, asum = 0.0f;
    for (int i = 0; i < STRAT_COUNT; i++)
        if (w->enabled[i]) { wsum += w->weight[i] * act[i]; asum += act[i]; }
    float wmean = asum > 0.001f ? wsum / asum : 1.0f;
    if (wmean < 0.05f) wmean = 0.05f;   // degenerate weight files stay sane

    if (eff_out)
        for (int i = 0; i < STRAT_COUNT; i++)
            eff_out[i] = w->enabled[i] ? w->weight[i] / wmean : 0.0f;
    return wmean;
}

// Main strategy-based evaluation.
//
// Strategies compete for a fixed evaluation budget: every weight is divided
// by the phase-weighted mean of all enabled weights on this move, so only
// ratios matter — raising PAWN_PROMOTION (or KING_SAFETY) automatically
// dilutes MATERIAL and everything else on that same move, and uniformly
// inflating all weights changes nothing.
int eval_with_strategies(const Board *bd, const StrategyWeights *w) {
    int phase, raw[STRAT_COUNT];
    float eff[STRAT_COUNT];

    gather_raw(bd, &phase, raw);
    weight_mean(w, phase, eff);

    float total = 0.0f;
    for (int i = 0; i < STRAT_COUNT; i++)
        total += raw[i] * eff[i];

    // No learned bonus may ever rival a mate: search scores mates ±32000,
    // and this clamp keeps runaway relative weights out of that window.
    int score = (int)total;
    if (score >  16000) score =  16000;
    if (score < -16000) score = -16000;

    return bd->side == WHITE ? score : -score;
}

// Tooling hook: per-strategy raw scores (White POV) plus each strategy's
// effective (mean-normalised) weight in this position. Returns the total,
// White POV, unclamped by side to move.
int eval_explain(const Board *bd, const StrategyWeights *w,
                 int raw_out[STRAT_COUNT], float eff_out[STRAT_COUNT]) {
    int phase;
    gather_raw(bd, &phase, raw_out);
    weight_mean(w, phase, eff_out);

    float total = 0.0f;
    for (int i = 0; i < STRAT_COUNT; i++)
        total += raw_out[i] * eff_out[i];
    return (int)total;
}
