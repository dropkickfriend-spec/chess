// eval_strategy.h — Modular, phase-aware evaluation with self-calibration
#ifndef EVAL_STRATEGY_H
#define EVAL_STRATEGY_H

#include "board.h"

// Index transformation for flipped endgame evaluation (White: rank-flipped, Black: normal)
#define W_IDX(sq) ((7 - (sq) / 8) * 8 + (sq) % 8)
#define B_IDX(sq) (sq)

// Strategy types
typedef enum {
    // Opening strategies
    STRAT_DEVELOPMENT,
    STRAT_CENTER_CONTROL,
    STRAT_KING_SAFETY_OPENING,

    // Middlegame strategies
    STRAT_PIECE_ACTIVITY,
    STRAT_ATTACK_POTENTIAL,
    STRAT_PAWN_STRUCTURE,
    STRAT_DEFENDER_LOGISTICS,

    // Endgame strategies
    STRAT_KING_ACTIVITY,
    STRAT_PAWN_PROMOTION,
    STRAT_OPPOSITION,

    // All-phase
    STRAT_MATERIAL,
    STRAT_COORDINATION,   // pieces working together (own weight set below)
    STRAT_SQUARE_VALUE,   // piece-square tables, split out of MATERIAL
    STRAT_BLOCKADE,       // occupy the stop-square in front of an enemy passer
    STRAT_RESTRICTION,    // cover the squares the enemy wants (prophylaxis)

    STRAT_COUNT  // Total number of strategies
} StrategyType;

// Coordination feature weights (centipawns per instance), learned by the
// Texel tuner through the eval.c registry: MUTUAL_DEFENSE, BATTERY,
// OUTPOST, FOCAL_PRESSURE, PAWN_PIECE_SYNC.
#define COORD_N 5

// Strategy weight config (loaded from file, updated after each game)
typedef struct {
    float weight[STRAT_COUNT];
    int enabled[STRAT_COUNT];  // 1 = enabled, 0 = disabled
} StrategyWeights;

// Evaluate using active strategies for this phase
int eval_with_strategies(const Board *bd, const StrategyWeights *weights);

// Load strategy weights from file
int eval_load_strategy_weights(const char *path, StrategyWeights *weights);

// Get default strategy weights
void eval_default_strategy_weights(StrategyWeights *weights);

// Strategy names indexed by StrategyType
extern const char *strategy_names[STRAT_COUNT];

// Per-strategy breakdown for tooling: raw scores (White POV) and effective
// mean-normalised weights. Returns the total, White POV.
int eval_explain(const Board *bd, const StrategyWeights *w,
                 int raw_out[STRAT_COUNT], float eff_out[STRAT_COUNT]);

#endif
