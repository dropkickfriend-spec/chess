// eval_strategy.h — Granular term-based evaluation with self-calibration
#ifndef EVAL_STRATEGY_H
#define EVAL_STRATEGY_H

#include "board.h"

// Index transformation for flipped endgame evaluation (White: rank-flipped, Black: normal)
#define W_IDX(sq) ((7 - (sq) / 8) * 8 + (sq) % 8)
#define B_IDX(sq) (sq)

// Granular evaluation term weights (each term independently weighted)
typedef enum {
    // Material (base piece values)
    WEIGHT_MATERIAL,

    // Pawn structure
    WEIGHT_PASSED_PAWN,
    WEIGHT_ISOLATED_PAWN,
    WEIGHT_DOUBLED_PAWN,
    WEIGHT_UNSTOPPABLE_PASSER,

    // Piece mobility
    WEIGHT_KNIGHT_MOBILITY,
    WEIGHT_BISHOP_MOBILITY,
    WEIGHT_ROOK_MOBILITY,
    WEIGHT_QUEEN_MOBILITY,

    // Piece positioning
    WEIGHT_BISHOP_PAIR,
    WEIGHT_ROOK_OPEN_FILE,
    WEIGHT_ROOK_SEMIOPEN_FILE,

    // King safety
    WEIGHT_KING_SHIELD,
    WEIGHT_KING_ESCAPE_SQUARES,
    WEIGHT_KING_BACK_RANK_TRAP,

    // King attack
    WEIGHT_KING_ATTACK_UNITS,

    WEIGHT_COUNT  // Total number of terms
} EvalWeight;

// Weight config (loaded from file, updated after each game)
typedef struct {
    float weight[WEIGHT_COUNT];
} EvalWeights;

// Evaluate with granular term weighting
int eval_with_strategies(const Board *bd, const EvalWeights *weights);

// Load weights from file
int eval_load_strategy_weights(const char *path, EvalWeights *weights);

// Save weights to file
int eval_save_strategy_weights(const char *path, const EvalWeights *weights);

// Get default weights
void eval_default_strategy_weights(EvalWeights *weights);

#endif
