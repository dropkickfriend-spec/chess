// tune.c — Texel tuning: fit eval weights to real game outcomes.
// E = mean over positions of (result - sigmoid(K * eval_white))^2, where
// result is the game's final score for White. First K is fitted with the
// current weights, then coordinate descent nudges every registered weight,
// keeping changes that reduce E. Output is C source for eval.c's tables.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tune.h"
#include "board.h"
#include "eval.h"

typedef struct {
    Board bd;
    double result;   // 1 / 0.5 / 0 from White's perspective
} Sample;

static Sample *samples;
static int n_samples;

static double sigmoid(double k, int cp) {
    return 1.0 / (1.0 + pow(10.0, -k * cp / 400.0));
}

static int eval_white(const Board *bd) {
    int s = evaluate(bd);
    return bd->side == WHITE ? s : -s;
}

// ---- L2 regularization toward the starting priors ----
// Tuning on the engine's OWN games overweights material: in blunder-filled
// weak games a material edge is highly predictive of the result, so pure
// error-minimization inflates piece values (and can send eg-pawn negative).
// A mild penalty pulling each weight back toward its starting value (textbook
// priors, when tuning is launched from them) keeps values sane while still
// letting the data refine them. Strength set by TUNE_REG (0 = off).
static double reg_lambda = 0.0;
static double *prior_flat = NULL;
static int prior_n = 0;

static void snapshot_priors(void) {
    int tot = 0;
    for (int b = 0; b < eval_params_n; b++) tot += eval_params[b].count;
    prior_flat = malloc(tot * sizeof(double));
    int k = 0;
    for (int b = 0; b < eval_params_n; b++)
        for (int i = 0; i < eval_params[b].count; i++)
            prior_flat[k++] = eval_params[b].ptr[i];
    prior_n = tot;
    const char *r = getenv("TUNE_REG");
    reg_lambda = r ? atof(r) : 0.03;   // mild default; TUNE_REG=0 disables
}

// Mean squared relative deviation from priors; each weight's scale is its own
// magnitude (floored) so a 100 cp piece and a 20 cp bonus are penalized in
// proportion, not absolutely.
static double reg_penalty(void) {
    if (reg_lambda <= 0 || !prior_flat) return 0;
    double s = 0;
    int k = 0;
    for (int b = 0; b < eval_params_n; b++)
        for (int i = 0; i < eval_params[b].count; i++) {
            double prior = prior_flat[k++];
            double scale = prior < 0 ? -prior : prior;
            if (scale < 30) scale = 30;
            double d = (eval_params[b].ptr[i] - prior) / scale;
            s += d * d;
        }
    return reg_lambda * s;   // sum: only tuned weights deviate from prior
}

static double error_fn(double k) {
    double e = 0;
    for (int i = 0; i < n_samples; i++) {
        double diff = samples[i].result - sigmoid(k, eval_white(&samples[i].bd));
        e += diff * diff;
    }
    return e / n_samples + reg_penalty();
}

// Best sigmoid scale K for the eval as it stands right now. Eval scores do
// not depend on K, so we evaluate every position once, then sweep K as cheap
// arithmetic over the cached centipawns — this makes re-fitting K between
// descent passes nearly free (one eval scan vs the descent's ~50 per pass).
// Re-run each pass so K, not the pinned-anchor material values, carries the
// eval's overall scale.
static double fit_k(void) {
    static int *cp = NULL;
    if (!cp) cp = malloc(n_samples * sizeof(int));
    for (int i = 0; i < n_samples; i++)
        cp[i] = eval_white(&samples[i].bd);

    double best_k = 0.5, best_e = 1e18;
    for (double k = 0.10; k <= 3.0; k += 0.05) {
        double e = 0;
        for (int i = 0; i < n_samples; i++) {
            double d = samples[i].result - sigmoid(k, cp[i]);
            e += d * d;
        }
        if (e < best_e) { best_e = e; best_k = k; }
    }
    return best_k;
}

static int load_dataset(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }

    int cap = 1 << 16;
    samples = malloc(cap * sizeof(Sample));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *semi = strchr(line, ';');
        if (!semi) continue;
        *semi = 0;
        if (n_samples == cap) {
            cap *= 2;
            samples = realloc(samples, cap * sizeof(Sample));
        }
        board_from_fen(&samples[n_samples].bd, line);
        samples[n_samples].result = atof(semi + 1);
        n_samples++;
    }
    fclose(f);
    return n_samples;
}

// Machine format, one weight per line: "<block-name> <index> <value>".
// Loadable back into a running engine via eval_load_weights (CHESS_WEIGHTS).
static void dump_params(void) {
    for (int b = 0; b < eval_params_n; b++) {
        const ParamBlock *pb = &eval_params[b];
        for (int i = 0; i < pb->count; i++)
            printf("%s %d %d\n", pb->name, i, pb->ptr[i]);
    }
    fflush(stdout);
}

// Curriculum: only fit what the data can feed. Every parameter wants ~30
// positions behind it; blocks join in priority order — coarse, global
// quantities first, the 64-square tables last — so a small game log tunes
// material and scalars instead of memorising itself into 512 PST cells.
static const char *stage_priority[] = {
    "material_mg+1", "material_eg",
    "ISOLATED_MG", "ISOLATED_EG", "DOUBLED_MG", "DOUBLED_EG",
    "BISHOP_PAIR_MG", "BISHOP_PAIR_EG", "ROOK_OPEN", "ROOK_SEMIOPEN",
    "SHIELD_BONUS",
    "coord_w", "PLAN_PART", "PLAN_IDLE", "PLAN_ENGAGE",
    "passed_mg+1", "passed_eg+1",
    "pawn_eg", "king_eg",
    "pst_mg[0]", "pst_mg[5]", "pst_mg[1]", "pst_mg[2]", "pst_mg[3]", "pst_mg[4]",
};

void tune_run(const char *dataset_path) {
    if (!load_dataset(dataset_path)) return;
    fprintf(stderr, "loaded %d positions\n", n_samples);
    snapshot_priors();
    fprintf(stderr, "regularization lambda = %.3f (TUNE_REG)\n", reg_lambda);

    static int tunable[64];
    memset(tunable, 0, sizeof(tunable));
    int budget = n_samples / 30, used = 0, total = 0;
    for (int b = 0; b < eval_params_n; b++) total += eval_params[b].count;
    for (unsigned p = 0; p < sizeof(stage_priority) / sizeof(*stage_priority); p++) {
        for (int b = 0; b < eval_params_n; b++) {
            if (strcmp(eval_params[b].name, stage_priority[p]) != 0) continue;
            if (used == 0 || used + eval_params[b].count <= budget) {
                tunable[b] = 1;
                used += eval_params[b].count;
            }
        }
    }
    fprintf(stderr, "stage: tuning %d of %d params (budget %d positions/30)\n",
            used, total, budget);

    // Fit the sigmoid scale K on the untouched eval.
    double best_k = fit_k();
    double best_e = error_fn(best_k);
    fprintf(stderr, "K = %.2f, initial E = %.6f\n", best_k, best_e);

    // Coordinate descent, coarse steps first. A pass must cut E by a real
    // margin to earn another one — float-dust "improvements" after the data
    // is fully fitted otherwise keep the loop spinning forever — and each
    // step size is capped outright as a backstop.
    static const int steps[] = { 8, 4, 2, 1 };
    for (unsigned si = 0; si < sizeof(steps) / sizeof(steps[0]); si++) {
        int step = steps[si];
        int improved = 1;
        int pass = 0;
        while (improved && pass < 30) {
            // Re-fit K each pass. With a small dataset the curriculum tunes
            // only a few blocks (often just material), so a frozen K forces
            // those few values to absorb the entire eval *scale* — the pawn
            // is pinned as the anchor, so that pressure inflates the piece/
            // pawn ratios into nonsense (a past run reached knight ~= 20
            // pawns). Letting K move each pass absorbs the scale, keeping
            // descent focused on the relative values the data actually implies.
            best_k = fit_k();
            best_e = error_fn(best_k);
            double pass_start_e = best_e;
            improved = 0;
            pass++;
            for (int b = 0; b < eval_params_n; b++) {
                const ParamBlock *pb = &eval_params[b];
                if (!tunable[b]) continue;    // outside this stage's budget
                for (int i = 0; i < pb->count; i++) {
                    int orig = pb->ptr[i];
                    pb->ptr[i] = orig + step;
                    double e = error_fn(best_k);
                    if (e < best_e) { best_e = e; improved = 1; continue; }
                    pb->ptr[i] = orig - step;
                    e = error_fn(best_k);
                    if (e < best_e) { best_e = e; improved = 1; continue; }
                    pb->ptr[i] = orig;
                }
            }
            fprintf(stderr, "step %d pass %d: E = %.6f\n", step, pass, best_e);
            if (pass_start_e - best_e < 1e-7) break;   // converged at this step
        }
    }

    fprintf(stderr, "final E = %.6f\n", best_e);
    dump_params();
    free(samples);
}
