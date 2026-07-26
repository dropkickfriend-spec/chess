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
extern int pst_mg[6][64];
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
extern U64 plan_squares[2];        // search.c: the current game plan
extern int LOGI_PLAN;              // eval.c: plan-blockage price multiplier
extern int LINE_KING, LINE_HEAVY;  // eval.c: line-clearance toward king / heavy
extern int SQV_PLAN;               // eval.c: square-value plan amplifier

static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

// Strategy: MATERIAL — base piece values
// Strategy: MATERIAL — INTRINSIC piece worth only (base values), no squares.
// Separated from SQUARE_VALUE so the two carry independent learnable weights
// and compete in the shared budget (raising one dilutes the other).
static int eval_material(const Board *bd, int *phase_out) {
    int mg = 0, eg = 0, phase = 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        for (int pt = 0; pt < 6; pt++) {
            U64 bb = bd->bb[side * 6 + pt];
            while (bb) {
                int sq = LSB(bb); POP_BIT(bb, sq);
                phase += phase_w[pt];
                mg += sign * material_mg[pt];
                eg += sign * material_eg[pt];
            }
        }
    }

    if (phase > 24) phase = 24;
    *phase_out = phase;
    return (mg * phase + eg * (24 - phase)) / 24;
}

// Union of all of a side's piece attacks (used by RESTRICTION).
// Per-position attack cache. The same piece's attack set (under the full
// occupancy) was previously recomputed independently in PIECE_ACTIVITY,
// ATTACK_POTENTIAL, COORDINATION, RESTRICTION, TRANSIT and side_attacks —
// 4-5 magic sliding lookups per piece, per eval. Computing it once and
// sharing it is output-identical and removes the bulk of the eval's cost.
typedef struct {
    U64 att[64];        // attack set of the piece occupying each square
    U64 side_att[2];    // union of each side's attacks (pawns..king)
    U64 pawn_att[2];    // pawns only: squares a piece can be evicted from
} AttackInfo;

// Can the piece standing on `sq` actually hold it? side_att is already built,
// so both questions are a single AND. Note a piece's own attack set never
// includes its own square, so side_att[side] & sq means "defended by some
// OTHER friendly piece", which is exactly what is wanted.
static inline int sq_contested(const AttackInfo *ai, int side, int sq) {
    return (ai->side_att[!side] >> sq) & 1ULL;
}
static inline int sq_held(const AttackInfo *ai, int side, int sq) {
    return (ai->side_att[side] >> sq) & 1ULL;
}
static inline int sq_kicked(const AttackInfo *ai, int side, int sq) {
    return (ai->pawn_att[!side] >> sq) & 1ULL;
}

// att[sq] is written for every occupied square; empty squares are never read
// by any consumer, so the array needs no zeroing.
static void compute_attacks(const Board *bd, AttackInfo *ai) {
    U64 occ = bd->occ[BOTH];
    ai->side_att[WHITE] = ai->side_att[BLACK] = 0;
    ai->pawn_att[WHITE] = ai->pawn_att[BLACK] = 0;
    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 u = 0, p = 0, bb, a;
        bb = bd->bb[base];     while (bb) { int s = LSB(bb); POP_BIT(bb, s); a = pawn_attacks[side][s]; ai->att[s] = a; u |= a; p |= a; }
        bb = bd->bb[base + 1]; while (bb) { int s = LSB(bb); POP_BIT(bb, s); a = knight_attacks[s];      ai->att[s] = a; u |= a; }
        bb = bd->bb[base + 2]; while (bb) { int s = LSB(bb); POP_BIT(bb, s); a = get_bishop_attacks(s, occ); ai->att[s] = a; u |= a; }
        bb = bd->bb[base + 3]; while (bb) { int s = LSB(bb); POP_BIT(bb, s); a = get_rook_attacks(s, occ);   ai->att[s] = a; u |= a; }
        bb = bd->bb[base + 4]; while (bb) { int s = LSB(bb); POP_BIT(bb, s); a = get_queen_attacks(s, occ);  ai->att[s] = a; u |= a; }
        bb = bd->bb[base + 5]; if (bb)    { int s = LSB(bb);                 a = king_attacks[s];            ai->att[s] = a; u |= a; }
        ai->side_att[side] = u;
        ai->pawn_att[side] = p;
    }
}

extern int action_w[3];

// Strategy: BLOCKADE — occupying the stop-square directly in front of an
// enemy passed pawn freezes it. Knights are the classic blockaders (they
// still do their job from the blockade square); scaled by how advanced the
// passer is (blocking a runner matters more).
static int eval_blockade(const Board *bd, int phase) {
    (void)phase;
    int score = 0;
    U64 wp = bd->bb[WP], bp = bd->bb[BP];
    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int ebase = side == WHITE ? BP : WP;         // ENEMY pawns we blockade
        U64 our_pawns   = side == WHITE ? wp : bp;
        U64 pp = bd->bb[ebase];
        while (pp) {
            int sq = LSB(pp); POP_BIT(pp, sq);
            if (passed_mask[!side][sq] & our_pawns) continue;   // not passed
            int stop = ((!side) == WHITE) ? sq + 8 : sq - 8;      // square ahead of enemy pawn
            if (stop < 0 || stop > 63) continue;
            if (!GET_BIT(bd->occ[side], stop)) continue;        // we must occupy it
            int erank = ((!side) == WHITE) ? sq / 8 : 7 - sq / 8; // enemy pawn advancement
            int mult = 1;
            // knight/bishop on the stop square = a proper blockader
            if (GET_BIT(bd->bb[side * 6 + 1] | bd->bb[side * 6 + 2], stop)) mult = 2;
            score += sign * action_w[0] * erank * mult;
        }
    }
    return score;
}

// Strategy: RESTRICTION — prophylaxis. Count the squares each enemy piece
// would like to move to that WE already control; denying an enemy piece its
// squares is exactly the positional judgement the engine otherwise lacks.
static int eval_restriction(const Board *bd, int phase, const AttackInfo *ai) {
    (void)phase;
    int score = 0;
    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        U64 our_att = ai->side_att[side];
        int ebase = side == WHITE ? BP : WP;
        U64 enemy_occ = bd->occ[!side];
        int restricted = 0;
        for (int pt = 1; pt <= 4; pt++) {                // enemy N,B,R,Q
            U64 bb = bd->bb[ebase + pt];
            while (bb) {
                int s = LSB(bb); POP_BIT(bb, s);
                U64 tgt = ai->att[s];                    // this piece's attacks
                restricted += COUNT(tgt & ~enemy_occ & our_att);
            }
        }
        score += sign * action_w[1] * restricted;
    }
    return score;
}

// Strategy: SQUARE_VALUE — piece-square tables only (where each piece stands),
// with the same tapered mg/eg blend. Pairs with MATERIAL; the search's PV is
// built from the combined eval, so this term automatically reshapes the game
// plan, and gather_raw runs it through the same best-move certainty filter as
// MATERIAL/GAME_PLAN.
// Measured neutral: disabling it scored 49.6% over 128 games (-2.7 Elo, LOS
// 45.6%), i.e. no detectable strength either way, while costing ~6% nps. Off by
// default; CHESS_SQV=1 opts back in to retest (e.g. against a retuned
// SQV_LOOSE/SQV_KICK, which were set by hand and never tuned).
int sqv_context_on = 0;

static int eval_square_value(const Board *bd, int phase, const AttackInfo *ai) {
    extern int SQV_LOOSE, SQV_KICK;
    int mg = 0, eg = 0;
    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        U64 plan = plan_squares[side];
        for (int pt = 0; pt < 6; pt++) {
            U64 bb = bd->bb[side * 6 + pt];
            while (bb) {
                int sq = LSB(bb); POP_BIT(bb, sq);
                int idx = side == WHITE ? W_IDX(sq) : B_IDX(sq);
                int pmg = pst_mg[pt][idx];
                int peg = pt == 0 ? pawn_eg[idx] : pt == 5 ? king_eg[idx]
                                                           : pst_mg[pt][idx];
                // Context: the same square is worth more when the piece on it
                // serves the plan the search is executing (SQV_PLAN percent).
                if (SQV_PLAN && plan && (plan & (1ULL << sq))) {
                    pmg = pmg * (10 + SQV_PLAN) / 10;
                    peg = peg * (10 + SQV_PLAN) / 10;
                }
                // Per-position context: a piece is only really ON a good square
                // if it can hold it. A knight on e5 that ...d6 evicts next move
                // was never on e5 in any lasting sense.
                //
                // Only the POSITIVE part is discounted. A square bonus is
                // something you have to hold to bank; a square PENALTY (a
                // knight on a1) is bad whether or not the square is contested,
                // and scaling that toward zero would pay a piece for hanging.
                if (sqv_context_on && pt != 5 && (pmg > 0 || peg > 0)) {
                    int realise = 100;
                    if (sq_contested(ai, side, sq) && !sq_held(ai, side, sq))
                        realise -= SQV_LOOSE;
                    // Pawn eviction costs the opponent nothing, so it is a real
                    // discount for pieces. Excluded for pawns: pawn-attacks-pawn
                    // is an even trade, not an eviction.
                    if (pt >= 1 && sq_kicked(ai, side, sq))
                        realise -= SQV_KICK;
                    if (realise < 0) realise = 0;
                    if (realise < 100) {
                        if (pmg > 0) pmg = pmg * realise / 100;
                        if (peg > 0) peg = peg * realise / 100;
                    }
                }
                mg += sign * pmg;
                eg += sign * peg;
            }
        }
    }
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
static int eval_piece_activity(const Board *bd, int phase, const AttackInfo *ai) {
    int mg = 0, eg = 0;
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
            U64 att = ai->att[sq];
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
            U64 att = ai->att[sq];
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
            U64 att = ai->att[sq];
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
static int eval_attack_potential(const Board *bd, int phase, const AttackInfo *ai) {
    int atk_units[2] = { 0, 0 };

    U64 king_zone[2];
    king_zone[WHITE] = bd->bb[WK] ? king_attacks[LSB(bd->bb[WK])] | bd->bb[WK] : 0;
    king_zone[BLACK] = bd->bb[BK] ? king_attacks[LSB(bd->bb[BK])] | bd->bb[BK] : 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int base = side == WHITE ? WP : BP;
        U64 enemy_zone = king_zone[!side];

        // Knight, bishop, rook, queen attacks on the enemy king zone
        for (int pt = 1; pt <= 4; pt++) {
            U64 bb = bd->bb[base + pt];
            while (bb) {
                int sq = LSB(bb); POP_BIT(bb, sq);
                atk_units[side] += king_atk_weight[pt] * COUNT(ai->att[sq] & enemy_zone);
            }
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
// each other, priced by what the GAME PLAN needs. Blockage is measured as
// latent mobility (attacks with own pawns lifted / all own men lifted minus
// actual), but its cost is plan-relative: blocking a piece the plan uses,
// or sealing squares the plan's routes run through, costs (10+LOGI_PLAN)/10
// times more than cramping a bystander. So the term maps each move's
// logistical increase or decrease AGAINST the plan — opening lines for the
// pieces executing it scores highest, and cramping them hurts most. Pawn
// blockage stays full price (slow walls), piece blockage half (a friend can
// vacate next move). LOGI_PLAN is a learned registry knob.
int eval_logistics_on = 1;   // CHESS_NOLOGI=1 disables DEFENDER_LOGISTICS (~8% nps)

static int eval_pawn_logistics(const Board *bd, int phase, const AttackInfo *ai) {
    if (!eval_logistics_on) return 0;   // measured toggle: is it worth its cost?
    int mg = 0, eg = 0;
    U64 occ = bd->occ[BOTH];

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = bd->bb[base];
        U64 occ_np   = occ & ~own_pawns;   // our pawns lifted
        U64 occ_free = occ & ~own;         // all our men lifted
        U64 plan = plan_squares[side];

        // Line-clearance targets: a ray opened by moving our own blockers is
        // worth more if it would bear on the enemy king zone or a heavy piece.
        int ek = LSB(bd->bb[(!side) * 6 + 5]);
        U64 ekzone = king_attacks[ek] | (1ULL << ek);
        U64 eheavy = bd->bb[(!side) * 6 + 3] | bd->bb[(!side) * 6 + 4];

        // Sliders: latent squares gained if own men stepped aside; the
        // blockage cost scales up when the piece is a plan participant or
        // its denied squares lie on the plan's routes.
        static const int wmg[3] = { 3, 2, 1 }, weg[3] = { 2, 2, 1 };
        for (int pt = 2; pt <= 4; pt++) {
            U64 bb = bd->bb[base + pt];
            while (bb) {
                int sq = LSB(bb); POP_BIT(bb, sq);
                U64 a_now = ai->att[sq], a_np, a_fr;   // a_now: full-occ (cached)
                if (pt == 2) {
                    a_np  = get_bishop_attacks(sq, occ_np);
                    a_fr  = get_bishop_attacks(sq, occ_free);
                } else if (pt == 3) {
                    a_np  = get_rook_attacks(sq, occ_np);
                    a_fr  = get_rook_attacks(sq, occ_free);
                } else {
                    a_np  = get_queen_attacks(sq, occ_np);
                    a_fr  = get_queen_attacks(sq, occ_free);
                }
                int by_pawn = COUNT(a_np) - COUNT(a_now);
                int by_piece = COUNT(a_fr) - COUNT(a_np);
                if (pt == 4) by_piece = (by_piece + 1) / 2;

                int w = wmg[pt - 2];
                int cmg = by_pawn * w + (by_piece * w + 1) / 2;
                int ceg = by_pawn * weg[pt - 2] + by_piece * weg[pt - 2] / 2;

                // Plan-relative pricing: this piece is in the plan, or the
                // squares it is denied are ones the plan travels through.
                U64 denied = a_fr & ~a_now;
                if (plan && ((plan & (1ULL << sq)) || (denied & plan))) {
                    cmg = cmg * (10 + LOGI_PLAN) / 10;
                    ceg = ceg * (10 + LOGI_PLAN) / 10;
                }
                // Target-value clearance: the blocked ray, once opened, would
                // bear on the enemy king zone or a heavy piece — so the men
                // in the way are costlier to leave standing there.
                if (denied & ekzone) { cmg += LINE_KING; ceg += LINE_KING / 2; }
                U64 dh = denied & eheavy;
                if (dh) { int n = COUNT(dh);
                    cmg += LINE_HEAVY * n; ceg += LINE_HEAVY * n / 2; }
                mg -= sign * cmg;
                eg -= sign * ceg;
            }
        }

        // Knights: own men squatting on landing squares (pawns full price,
        // pieces half); a plan-knight's crowding costs plan price too.
        U64 bb = bd->bb[base + 1];
        while (bb) {
            int sq = LSB(bb); POP_BIT(bb, sq);
            int crowd_pawn  = COUNT(knight_attacks[sq] & own_pawns);
            int crowd_piece = COUNT(knight_attacks[sq] & own & ~own_pawns);
            int cmg = crowd_pawn * 2 + crowd_piece;
            int ceg = crowd_pawn;
            if (plan && ((plan & (1ULL << sq))
                         || (knight_attacks[sq] & own & plan))) {
                cmg = cmg * (10 + LOGI_PLAN) / 10;
                ceg = ceg * (10 + LOGI_PLAN) / 10;
            }
            mg -= sign * cmg;
            eg -= sign * ceg;
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

// Centre-manhattan distance: 0 in the middle, 6 in the corners. Driving a
// bare king to a high-CMD square is how K+Q / K+R vs K is won.
static const int centre_dist[64] = {
    6, 5, 4, 3, 3, 4, 5, 6,
    5, 4, 3, 2, 2, 3, 4, 5,
    4, 3, 2, 1, 1, 2, 3, 4,
    3, 2, 1, 0, 0, 1, 2, 3,
    3, 2, 1, 0, 0, 1, 2, 3,
    4, 3, 2, 1, 1, 2, 3, 4,
    5, 4, 3, 2, 2, 3, 4, 5,
    6, 5, 4, 3, 3, 4, 5, 6,
};

// Strategy: KING_ACTIVITY — endgame king centralisation, pawn proximity, and
// bare-king mating drive.
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

    // Bare-king mate drive: if one side has only its king and the other holds
    // a rook or queen, the flat material lead gives the search no gradient to
    // follow, so it shuffles until the 50-move rule. Reward pushing the lone
    // king to a corner and marching our own king up to it.
    U64 w_men = bd->bb[WN]|bd->bb[WB]|bd->bb[WR]|bd->bb[WQ]|bd->bb[WP];
    U64 b_men = bd->bb[BN]|bd->bb[BB_]|bd->bb[BR]|bd->bb[BQ]|bd->bb[BP];
    if (!b_men && (bd->bb[WR] | bd->bb[WQ]))
        eg += 15 * centre_dist[bk] + 5 * (7 - CHEB(wk, bk));
    else if (!w_men && (bd->bb[BR] | bd->bb[BQ]))
        eg -= 15 * centre_dist[wk] + 5 * (7 - CHEB(bk, wk));

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

// Slot order as it stood when weight files were keyed by integer index. Kept so
// old files still load correctly after a strategy is deleted from the middle of
// the enum: without it, dropping GAME_PLAN (old index 12) would silently shift
// SQUARE_VALUE/BLOCKADE/RESTRICTION down one and hand each of them the wrong
// weight. A name here that no longer exists in strategy_names is simply
// skipped, which is exactly what should happen to a retired strategy.
static const char *legacy_slot_names[] = {
    "DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
    "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE",
    "DEFENDER_LOGISTICS", "KING_ACTIVITY", "PAWN_PROMOTION",
    "OPPOSITION", "MATERIAL", "COORDINATION", "GAME_PLAN", "SQUARE_VALUE",
    "BLOCKADE", "RESTRICTION", "TRANSIT"
};
#define LEGACY_SLOT_N ((int)(sizeof(legacy_slot_names) / sizeof(legacy_slot_names[0])))

// -1 when the name is not a live strategy (retired, or a typo).
static int strategy_index_by_name(const char *name) {
    for (int i = 0; i < STRAT_COUNT; i++)
        if (strcmp(strategy_names[i], name) == 0) return i;
    return -1;
}

int eval_load_strategy_weights(const char *path, StrategyWeights *w) {
    FILE *f = fopen(path, "r");
    if (!f) {
        eval_default_strategy_weights(w);
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float wt;
        if (sscanf(line, "weight %63s %f", key, &wt) != 2) continue;
        // Preferred form: "weight SQUARE_VALUE 1.1353". Names survive enum
        // edits; integer indices do not.
        int idx = strategy_index_by_name(key);
        if (idx < 0) {
            // Legacy form: "weight 13 1.1353". Translate through the old slot
            // order rather than trusting the number to still mean the same
            // strategy.
            char *end;
            long slot = strtol(key, &end, 10);
            if (*end || end == key || slot < 0 || slot >= LEGACY_SLOT_N) continue;
            idx = strategy_index_by_name(legacy_slot_names[slot]);
        }
        if (idx >= 0) w->weight[idx] = wt;
    }
    fclose(f);
    return 1;
}

const char *strategy_names[STRAT_COUNT] = {
    "DEVELOPMENT", "CENTER_CONTROL", "KING_SAFETY_OPENING",
    "PIECE_ACTIVITY", "ATTACK_POTENTIAL", "PAWN_STRUCTURE",
    "DEFENDER_LOGISTICS", "KING_ACTIVITY", "PAWN_PROMOTION",
    "OPPOSITION", "MATERIAL", "COORDINATION", "SQUARE_VALUE",
    "BLOCKADE", "RESTRICTION"
};

extern int PLAN_PART, PLAN_IDLE, PLAN_ENGAGE, CERT_FLOOR, LOGI_PLAN;
extern int CERT_LOOSE;
extern int plan_certainty;

// Measured +80 Elo: disabling it (falling back to the root-global plan_certainty)
// scored 38.7% over 128 games, LOS 0.1%. On by default; CHESS_NOCERT=1 restores
// the old root-global behaviour for re-measurement.
int cert_loose_on = 1;

// GAME_PLAN used to live here: the last completed iteration's PV was treated as
// "the plan", and every piece was priced by its degree of involvement in it.
// Ablation measured the whole plan machinery at -137 Elo (LOS 0%), with the
// GAME_PLAN term itself the largest single contributor at -85, so the knobs
// were zeroed and the strategy is now removed outright. plan_squares survives
// because SQUARE_VALUE, PIECE_ACTIVITY and COORDINATION still read it.

extern int coord_w[COORD_N];

// Strategy: COORDINATION — a piece coordinates when it follows the game
// plan AND is defended AND activates friends. Per piece, one combined
// score instead of disconnected pattern counters:
//
//   base = coord_w[0] * defenders_it_has      (friendly cover of its square)
//        + coord_w[1] * pieces_it_activates   (own men its attacks support)
//        + coord_w[2] * battery               (R/Q file, B/Q diagonal)
//        + coord_w[3] * outpost               (pawn-guarded, unassailable)
//
//   plan multiplier: a piece standing on its side's plan squares has its
//   whole base scaled by (10 + coord_w[4]) / 10 — following the game plan
//   amplifies everything else the piece contributes. All five weights are
//   learned from outcomes through the tuning registry.
static int eval_coordination(const Board *bd, int phase, const AttackInfo *ai) {
    (void)phase;
    int score = 0;

    for (int side = WHITE; side <= BLACK; side++) {
        int sign = side == WHITE ? 1 : -1;
        int base_i = side == WHITE ? WP : BP;
        U64 own = bd->occ[side];
        U64 own_pawns = bd->bb[base_i];
        U64 enemy_pawns = bd->bb[side == WHITE ? BP : WP];

        // First pass: per-square friendly cover and per-piece attack sets
        U64 pawn_cover = 0;
        U64 pb = own_pawns;
        while (pb) {
            int s = LSB(pb); POP_BIT(pb, s);
            pawn_cover |= pawn_attacks[side][s];
        }
        int cover[64] = {0};           // friendly attackers per square
        {
            U64 pc = pawn_cover;       // pawns contribute cover once each sq
            while (pc) { int s = LSB(pc); POP_BIT(pc, s); cover[s]++; }
        }
        U64 att_of[16]; int sq_of[16], pt_of[16], n_pieces = 0;
        for (int pt = 1; pt <= 5; pt++) {
            U64 b = bd->bb[base_i + pt];
            while (b && n_pieces < 16) {
                int sq = LSB(b); POP_BIT(b, sq);
                U64 att = ai->att[sq];
                att_of[n_pieces] = att;
                sq_of[n_pieces] = sq;
                pt_of[n_pieces] = pt;
                n_pieces++;
                U64 t = att;
                while (t) { int s = LSB(t); POP_BIT(t, s); cover[s]++; }
            }
        }

        // Second pass: combined per-piece coordination
        for (int i = 0; i < n_pieces; i++) {
            int sq = sq_of[i], pt = pt_of[i];

            int defenders = cover[sq];   // own square never in own attack set
            int activates = COUNT(att_of[i] & own & ~(1ULL << sq));

            int battery = 0;
            if (pt == 3 && (att_of[i] & file_mask[sq % 8]
                            & (bd->bb[base_i + 3] | bd->bb[base_i + 4])))
                battery = 1;
            if (pt == 2 && (att_of[i] & bd->bb[base_i + 4]))
                battery = 1;

            int outpost = 0;
            if ((pt == 1 || pt == 2)
                && (pawn_cover & (1ULL << sq))
                && !(passed_mask[side][sq] & adj_files[sq % 8] & enemy_pawns)) {
                int rel_rank = side == WHITE ? sq / 8 : 7 - sq / 8;
                if (rel_rank >= 3 && rel_rank <= 5) outpost = 1;
            }

            int piece_base = coord_w[0] * defenders
                           + coord_w[1] * activates
                           + coord_w[2] * battery
                           + coord_w[3] * outpost;

            // Following the plan amplifies the piece's whole contribution
            if (plan_squares[side] & (1ULL << sq))
                piece_base = piece_base * (10 + coord_w[4]) / 10;

            score += sign * piece_base;
        }
    }

    return score;
}

// Shared core: raw per-strategy scores (White POV, danger discount applied)
// and the phase-weighted mean of enabled weights.
static void gather_raw(const Board *bd, int *phase_out, int raw[STRAT_COUNT]) {
    int phase;
    AttackInfo ai;
    compute_attacks(bd, &ai);   // one attack sweep shared by every strategy
    raw[STRAT_MATERIAL]            = eval_material(bd, &phase);
    raw[STRAT_PAWN_STRUCTURE]      = eval_pawn_structure(bd, phase);
    raw[STRAT_PIECE_ACTIVITY]      = eval_piece_activity(bd, phase, &ai);
    raw[STRAT_KING_SAFETY_OPENING] = eval_king_safety(bd, phase);
    raw[STRAT_ATTACK_POTENTIAL]    = eval_attack_potential(bd, phase, &ai);
    raw[STRAT_DEFENDER_LOGISTICS]  = eval_pawn_logistics(bd, phase, &ai);
    raw[STRAT_DEVELOPMENT]         = eval_development(bd, phase);
    raw[STRAT_CENTER_CONTROL]      = eval_center_control(bd, phase);
    raw[STRAT_KING_ACTIVITY]       = eval_king_activity(bd, phase);
    raw[STRAT_OPPOSITION]          = eval_opposition(bd, phase);
    raw[STRAT_PAWN_PROMOTION]      = eval_pawn_promotion(bd, phase);
    raw[STRAT_COORDINATION]        = eval_coordination(bd, phase, &ai);
    raw[STRAT_SQUARE_VALUE]        = eval_square_value(bd, phase, &ai);
    raw[STRAT_BLOCKADE]            = eval_blockade(bd, phase);
    raw[STRAT_RESTRICTION]         = eval_restriction(bd, phase, &ai);

    // A queen is worth less when someone is getting mated — yours if the
    // attack is on you, theirs if you can throw it at their king. Material's
    // say shrinks as king danger (either side's, either sign) grows.
    int danger = abs(raw[STRAT_KING_SAFETY_OPENING])
               + abs(raw[STRAT_ATTACK_POTENTIAL]);
    if (danger > 400) danger = 400;
    raw[STRAT_MATERIAL] = raw[STRAT_MATERIAL] * 400 / (400 + danger);

    // Certainty pricing: learned values stay inflated as full-execution worth,
    // and realise CERT_FLOOR percent of it when the position is contested,
    // rising to 100 percent when nothing is hanging.
    //
    // "Contested" used to mean plan_certainty from search.c — PV overlap
    // measured ONCE PER ITERATION AT THE ROOT. Being a single global scalar it
    // was identical at every leaf, so it could not tell two positions apart; it
    // was also reset per search and assigned only after an iteration finished,
    // so the first iteration always ran at the floor and later ones used the
    // previous iteration's number. In tooling, with no lookahead at all, it was
    // pinned at the floor forever.
    //
    // It now comes from the position: the share of each side's material that is
    // attacked and undefended. Loose material is precisely when a static
    // evaluation is least trustworthy, and unlike PV wobble it is a property of
    // the node being evaluated.
    int floor_ = CERT_FLOOR < 0 ? 0 : CERT_FLOOR > 100 ? 100 : CERT_FLOOR;
    int certainty;
    if (cert_loose_on) {
        int loose = 0, total = 0;
        for (int side = WHITE; side <= BLACK; side++) {
            int base = side == WHITE ? WP : BP;
            for (int pt = 0; pt <= 4; pt++) {       // king cannot hang
                int val = material_mg[pt];
                U64 bb = bd->bb[base + pt];
                while (bb) {
                    int sq = LSB(bb); POP_BIT(bb, sq);
                    total += val;
                    if (sq_contested(&ai, side, sq) && !sq_held(&ai, side, sq))
                        loose += val;
                }
            }
        }
        int frac = total > 0 ? CERT_LOOSE * loose / total : 0;
        certainty = frac >= 100 ? 0 : 100 - frac;
    } else {
        certainty = plan_certainty;
    }
    int realise = floor_ + (100 - floor_) * certainty / 100;
    raw[STRAT_MATERIAL]     = raw[STRAT_MATERIAL]     * realise / 100;
    raw[STRAT_SQUARE_VALUE] = raw[STRAT_SQUARE_VALUE] * realise / 100;

    *phase_out = phase;
}

static float weight_mean(const Board *bd, const StrategyWeights *w, int phase,
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
    act[STRAT_SQUARE_VALUE]        = 1.0f;
    act[STRAT_BLOCKADE]            = 1.0f;
    act[STRAT_RESTRICTION]         = 1.0f;
    act[STRAT_KING_SAFETY_OPENING] = po;
    act[STRAT_ATTACK_POTENTIAL]    = po;
    act[STRAT_DEVELOPMENT]         = po;
    act[STRAT_CENTER_CONTROL]      = po;
    act[STRAT_KING_ACTIVITY]       = pe;
    act[STRAT_OPPOSITION]          = pe;
    act[STRAT_PAWN_PROMOTION]      = pe;

    // Lookahead engages the "things" weights too: the plan's measurable
    // character (which squares/men the PV fights over) raises the activity
    // of the strategies executing it, and the shared budget automatically
    // dilutes the bystanders. The gain (PLAN_ENGAGE percent) is a single
    // learned knob, not a per-strategy hand-set.
    U64 plan_all = PLAN_ENGAGE ? (plan_squares[WHITE] | plan_squares[BLACK]) : 0;
    if (plan_all) {
        // Certainty gates the plan's authority over the whole budget: a
        // churning plan barely reweights the other strategies; a plan the
        // deepening search cannot refute reallocates at full PLAN_ENGAGE.
        // This closes the recursion — depth N's certainty re-prices raw
        // scores AND redistributes every strategy weight at depth N+1.
        float gain = 1.0f + (float)PLAN_ENGAGE / 100.0f
                          * (float)plan_certainty / 100.0f;
        U64 kings = bd->bb[WK] | bd->bb[BK];
        U64 kzone = 0;
        if (bd->bb[WK]) kzone |= king_attacks[LSB(bd->bb[WK])];
        if (bd->bb[BK]) kzone |= king_attacks[LSB(bd->bb[BK])];

        // Plan converges on a king: attack/safety are the executing things
        if (plan_all & (kings | kzone)) {
            act[STRAT_ATTACK_POTENTIAL]    *= gain;
            act[STRAT_KING_SAFETY_OPENING] *= gain;
            act[STRAT_KING_ACTIVITY]       *= gain;
        }
        // Plan moves pawns: structure, promotion, and the logistics they gate
        if (plan_all & (bd->bb[WP] | bd->bb[BP])) {
            act[STRAT_PAWN_STRUCTURE]     *= gain;
            act[STRAT_PAWN_PROMOTION]     *= gain;
            act[STRAT_DEFENDER_LOGISTICS] *= gain;
        }
        // Plan fights over the centre
        const U64 CENTER = (1ULL<<27)|(1ULL<<28)|(1ULL<<35)|(1ULL<<36);
        if (plan_all & CENTER)
            act[STRAT_CENTER_CONTROL] *= gain;
        // Broad plan using many squares: cooperation is doing the work
        if (COUNT(plan_all) >= 10)
            act[STRAT_COORDINATION] *= gain;
    }

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
int eval_endgame_on = 1;   // CHESS_NOEG=1 disables the endgame recognizers

int eval_with_strategies(const Board *bd, const StrategyWeights *w) {
    int phase, raw[STRAT_COUNT];
    float eff[STRAT_COUNT];

    // Exact endgame knowledge first: a recognised theoretical draw is 0, full
    // stop. No weighting of material or squares can be right about a position
    // whose result is already proven — the engine used to score a dead K+P vs K
    // draw at +371 and could not distinguish it from the won version.
    {
        extern int eval_endgame_draw(const Board *bd);
        if (eval_endgame_on && eval_endgame_draw(bd)) return 0;
    }

    gather_raw(bd, &phase, raw);
    weight_mean(bd, w, phase, eff);

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
    weight_mean(bd, w, phase, eff_out);

    float total = 0.0f;
    for (int i = 0; i < STRAT_COUNT; i++)
        total += raw_out[i] * eff_out[i];
    return (int)total;
}

