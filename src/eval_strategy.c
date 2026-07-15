// eval_strategy.c — Strategy-based evaluation with phase detection and self-tuning
#include <stdio.h>
#include <string.h>
#include "eval_strategy.h"
#include "eval.h"
#include "attacks.h"
#include "magic.h"

// Detect current game phase based on remaining material
GamePhase eval_detect_phase(const Board *bd) {
    int material = 0;
    static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

    for (int i = WN; i <= BQ; i++) {
        material += COUNT(bd->bb[i]) * phase_w[i % 6];
    }

    // Full board = 24, opening ~20+, middlegame ~10-20, endgame <10
    if (material >= 20) return PHASE_OPENING;
    if (material >= 10) return PHASE_MIDDLEGAME;
    return PHASE_ENDGAME;
}

// Material balance (all phases)
static int strategy_material(const Board *bd) {
    int score = 0;
    extern int material_mg[6], material_eg[6];
    static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

    int phase = 0;
    for (int i = WN; i <= BQ; i++)
        phase += COUNT(bd->bb[i]) * phase_w[i % 6];
    if (phase > 24) phase = 24;

    for (int i = WP; i <= WQ; i++) {
        U64 bb = bd->bb[i];
        while (bb) {
            int sq = __builtin_ctzll(bb);
            bb &= bb - 1;
            int mg = material_mg[i];
            int eg = material_eg[i];
            score += (mg * phase + eg * (24 - phase)) / 24;
        }
    }
    for (int i = BP; i <= BQ; i++) {
        U64 bb = bd->bb[i];
        while (bb) {
            int sq = __builtin_ctzll(bb);
            bb &= bb - 1;
            int mg = material_mg[i % 6];
            int eg = material_eg[i % 6];
            score -= (mg * phase + eg * (24 - phase)) / 24;
        }
    }
    return score;
}

// Piece activity and centralization
static int strategy_piece_activity(const Board *bd) {
    int score = 0;
    extern int pst_mg[6][64], pst_eg[6][64], pawn_eg[64], king_eg[64];
    static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

    int phase = 0;
    for (int i = WN; i <= BQ; i++)
        phase += COUNT(bd->bb[i]) * phase_w[i % 6];
    if (phase > 24) phase = 24;

    // PST evaluation (simplified)
    for (int i = WP; i <= WQ; i++) {
        U64 bb = bd->bb[i];
        while (bb) {
            int sq = __builtin_ctzll(bb);
            bb &= bb - 1;
            int mg = pst_mg[i][sq];
            int eg = pst_eg[i][sq];
            score += (mg * phase + eg * (24 - phase)) / 24;
        }
    }
    for (int i = BP; i <= BQ; i++) {
        U64 bb = bd->bb[i];
        while (bb) {
            int sq = __builtin_ctzll(bb);
            bb &= bb - 1;
            int mg = pst_mg[i % 6][sq];
            int eg = pst_eg[i % 6][sq];
            score -= (mg * phase + eg * (24 - phase)) / 24;
        }
    }
    return score;
}

// King safety (opening/middlegame)
static int strategy_king_safety(const Board *bd) {
    int score = 0;
    extern int SHIELD_BONUS;

    // Simplified king safety: shield bonus for pawns near king
    U64 our_king = bd->bb[bd->side == WHITE ? WK : BK];
    U64 their_king = bd->bb[bd->side == WHITE ? BK : WK];

    if (our_king & (our_king - 1)) return 0;  // Multiple kings, invalid

    int our_king_sq = __builtin_ctzll(our_king);
    int their_king_sq = __builtin_ctzll(their_king);

    // Reward pawns near our king
    U64 our_pawns = bd->bb[bd->side == WHITE ? WP : BP];
    U64 near_our_king = 0;
    if (our_king_sq <= 7 || our_king_sq >= 56) {
        near_our_king = (1ULL << (our_king_sq - 8)) | (1ULL << (our_king_sq + 8)) |
                       (1ULL << (our_king_sq - 1)) | (1ULL << (our_king_sq + 1));
    }
    score += SHIELD_BONUS * COUNT(our_pawns & near_our_king);

    // Penalize pawns near their king
    U64 their_pawns = bd->bb[bd->side == WHITE ? BP : WP];
    U64 near_their_king = 0;
    if (their_king_sq <= 7 || their_king_sq >= 56) {
        near_their_king = (1ULL << (their_king_sq - 8)) | (1ULL << (their_king_sq + 8)) |
                         (1ULL << (their_king_sq - 1)) | (1ULL << (their_king_sq + 1));
    }
    score -= SHIELD_BONUS * COUNT(their_pawns & near_their_king);

    return score;
}

// Pawn structure (middlegame/endgame)
static int strategy_pawn_structure(const Board *bd) {
    int score = 0;
    extern int ISOLATED_MG, ISOLATED_EG, DOUBLED_MG, DOUBLED_EG;

    // Simplified: penalize isolated and doubled pawns
    U64 our_pawns = bd->bb[bd->side == WHITE ? WP : BP];
    U64 their_pawns = bd->bb[bd->side == WHITE ? BP : WP];

    // Count isolated pawns (no friendly pawns on adjacent files)
    U64 our_files = 0, their_files = 0;
    for (int file = 0; file < 8; file++) {
        if (our_pawns & (0x0101010101010101ULL << file)) our_files |= (0x0101010101010101ULL << file);
        if (their_pawns & (0x0101010101010101ULL << file)) their_files |= (0x0101010101010101ULL << file);
    }

    score -= ISOLATED_MG * COUNT(our_pawns & ~our_files);
    score += ISOLATED_MG * COUNT(their_pawns & ~their_files);

    return score;
}

// Passed pawns (endgame)
static int strategy_pawn_promotion(const Board *bd) {
    int score = 0;
    extern int passed_mg[6], passed_eg[6];
    static const int phase_w[6] = { 0, 1, 1, 2, 4, 0 };

    int phase = 0;
    for (int i = WN; i <= BQ; i++)
        phase += COUNT(bd->bb[i]) * phase_w[i % 6];
    if (phase > 24) phase = 24;

    // Simplified passed pawn detection
    U64 our_pawns = bd->bb[bd->side == WHITE ? WP : BP];
    U64 their_pawns = bd->bb[bd->side == WHITE ? BP : WP];

    // Count advanced pawns
    if (bd->side == WHITE) {
        U64 advanced = our_pawns & 0x00FFFFFFFFFFFFFF00ULL;  // Not on rank 1 or 8
        score += passed_mg[5] * COUNT(advanced);
        advanced = their_pawns & 0x00FFFFFFFFFFFFFF00ULL;
        score -= passed_mg[5] * COUNT(advanced);
    }

    return score;
}

// Default weights: all strategies enabled at weight 1.0
void eval_default_strategy_weights(StrategyWeights *weights) {
    for (int i = 0; i < STRAT_COUNT; i++) {
        weights->weight[i] = 1.0f;
        weights->enabled[i] = 1;
    }
}

// Load strategy weights from file
int eval_load_strategy_weights(const char *path, StrategyWeights *weights) {
    FILE *f = fopen(path, "r");
    if (!f) {
        eval_default_strategy_weights(weights);
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int idx;
        float w;
        if (sscanf(line, "weight %d %f", &idx, &w) == 2 && idx >= 0 && idx < STRAT_COUNT) {
            weights->weight[idx] = w;
        }
    }
    fclose(f);
    return 1;
}

// Save strategy weights to file
int eval_save_strategy_weights(const char *path, const StrategyWeights *weights) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    for (int i = 0; i < STRAT_COUNT; i++) {
        fprintf(f, "weight %d %f\n", i, weights->weight[i]);
    }
    fclose(f);
    return 1;
}

// Main strategy-based evaluation
int eval_with_strategies(const Board *bd, const StrategyWeights *weights) {
    GamePhase phase = eval_detect_phase(bd);
    int score = 0;

    // Material (always active)
    if (weights->enabled[STRAT_MATERIAL]) {
        score += strategy_material(bd) * weights->weight[STRAT_MATERIAL];
    }

    // Phase-specific strategies
    if (phase == PHASE_OPENING) {
        if (weights->enabled[STRAT_KING_SAFETY_OPENING])
            score += strategy_king_safety(bd) * weights->weight[STRAT_KING_SAFETY_OPENING];
    } else if (phase == PHASE_MIDDLEGAME) {
        if (weights->enabled[STRAT_PIECE_ACTIVITY])
            score += strategy_piece_activity(bd) * weights->weight[STRAT_PIECE_ACTIVITY];
        if (weights->enabled[STRAT_PAWN_STRUCTURE])
            score += strategy_pawn_structure(bd) * weights->weight[STRAT_PAWN_STRUCTURE];
    } else {
        if (weights->enabled[STRAT_PAWN_PROMOTION])
            score += strategy_pawn_promotion(bd) * weights->weight[STRAT_PAWN_PROMOTION];
    }

    // All-phase strategies
    if (weights->enabled[STRAT_PIECE_ACTIVITY])
        score += strategy_piece_activity(bd) * weights->weight[STRAT_PIECE_ACTIVITY];

    return bd->side == WHITE ? score : -score;
}
