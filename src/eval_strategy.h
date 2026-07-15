// eval_strategy.h — Modular, phase-aware evaluation with self-calibration
#ifndef EVAL_STRATEGY_H
#define EVAL_STRATEGY_H

#include "board.h"

// Index transformation for flipped endgame evaluation (White: rank-flipped, Black: normal)
#define W_IDX(sq) ((7 - (sq) / 8) * 8 + (sq) % 8)
#define B_IDX(sq) (sq)

// Game phases
typedef enum {
    PHASE_OPENING,
    PHASE_MIDDLEGAME,
    PHASE_ENDGAME
} GamePhase;

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

    STRAT_COUNT  // Total number of strategies
} StrategyType;

// Strategy weight config (loaded from file, updated after each game)
typedef struct {
    float weight[STRAT_COUNT];
    int enabled[STRAT_COUNT];  // 1 = enabled, 0 = disabled
} StrategyWeights;

// Detected game phase
GamePhase eval_detect_phase(const Board *bd);

// Evaluate using active strategies for this phase
int eval_with_strategies(const Board *bd, const StrategyWeights *weights);

// Load strategy weights from file
int eval_load_strategy_weights(const char *path, StrategyWeights *weights);

// Save strategy weights to file
int eval_save_strategy_weights(const char *path, const StrategyWeights *weights);

// Get default strategy weights
void eval_default_strategy_weights(StrategyWeights *weights);

#endif
