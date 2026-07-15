// eval.h — Part 8: static evaluation
#ifndef EVAL_H
#define EVAL_H

#include "board.h"

void eval_init(void);   // build pawn-structure / king-shield masks

// Score in centipawns from the side-to-move's perspective (negamax convention).
int evaluate(const Board *bd);

// Texel tuning: registry of every tunable weight block
typedef struct {
    const char *name;
    int *ptr;
    int count;
} ParamBlock;

extern const ParamBlock eval_params[];
extern const int eval_params_n;

// Apply a weights file written by tune mode; returns weights applied.
int eval_load_weights(const char *path);

// Mate certainty system: evaluates position for mating attack potential
typedef struct {
    int total_potential;
    int actually_reachable;
    int blocked_by_own;
    int blocked_by_our_control;
    int interference_cost;
} DefenderAnalysis;

typedef struct {
    float base_certainty;
    float move_reinforcement;
    float total_certainty;
    int phase;
} MateContext;

// Compute mate certainty from board position
MateContext eval_compute_mate_context(const Board *bd);

#endif
