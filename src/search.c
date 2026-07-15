// search.c — negamax alpha-beta with quiescence, MVV-LVA + killer ordering,
// check extension, repetition/50-move draws, and wall-clock time management.
#define _POSIX_C_SOURCE 199309L  // clock_gettime under -std=c11
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "search.h"
#include "movegen.h"
#include "move.h"
#include "eval.h"

// ---- Position hash (repetition detection only) ----
// splitmix64 finalizer over each bitboard; collision odds are irrelevant
// at the handful of positions a game history holds.
static U64 mix(U64 x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

U64 board_hash(const Board *bd) {
    U64 h = 0;
    for (int p = 0; p < 12; p++)
        h ^= mix(bd->bb[p] + (U64)p * 0x9E3779B97F4A7C15ULL);
    h ^= mix((U64)bd->side | ((U64)bd->ep << 1) | ((U64)bd->castle << 8));
    return h;
}

// ---- Search state ----
static U64 hist[2048];          // game history + current search path
static int hist_base;           // entries provided by the game
static int hist_len;

static long long nodes;
static int stop_search;
static struct timespec deadline;
static int killers[MAX_PLY][2];

static int pv_len[MAX_PLY];
static int pv_tab[MAX_PLY][MAX_PLY];

void search_set_history(const U64 *hashes, int n) {
    if (n > 1024) { hashes += n - 1024; n = 1024; }  // keep the recent tail
    memcpy(hist, hashes, n * sizeof(U64));
    hist_base = hist_len = n;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void check_time(void) {
    long long dl = deadline.tv_sec * 1000LL + deadline.tv_nsec / 1000000LL;
    if (now_ms() >= dl) stop_search = 1;
}

// Position repeated anywhere in game history or search path = draw.
// (Scoring the first repetition as a draw is standard engine practice.)
static int is_repetition(U64 h) {
    for (int i = hist_len - 2; i >= 0; i -= 1)
        if (hist[i] == h) return 1;
    return 0;
}

// ---- Move ordering ----
static const int piece_val[6] = { 100, 320, 330, 500, 900, 20000 };

static int victim_on(const Board *bd, int sq, int side) {
    int lo = side == WHITE ? WP : BP;
    for (int p = lo; p < lo + 6; p++)
        if (GET_BIT(bd->bb[p], sq)) return p % 6;
    return 0; // en passant target square is empty; victim is a pawn
}

static int score_move(const Board *bd, int move, int ply) {
    int s = 0;
    if (M_CAP(move)) {
        int victim = M_EP(move) ? 0 : victim_on(bd, M_TO(move), !bd->side);
        s = 100000 + piece_val[victim] * 10 - piece_val[M_PIECE(move) % 6];
    } else if (move == killers[ply][0]) s = 90000;
    else if (move == killers[ply][1])   s = 80000;
    if (M_PROMO(move)) s += 100000 + piece_val[M_PROMO(move) % 6];
    return s;
}

// Selection-sort the best remaining move to slot i (cheap for short lists,
// and most nodes cut off after the first move or two anyway).
static void pick_move(MoveList *ml, int *scores, int i) {
    int best = i;
    for (int j = i + 1; j < ml->count; j++)
        if (scores[j] > scores[best]) best = j;
    int tm = ml->moves[i]; ml->moves[i] = ml->moves[best]; ml->moves[best] = tm;
    int ts = scores[i];    scores[i] = scores[best];       scores[best] = ts;
}

// ---- Quiescence: resolve captures so eval isn't taken mid-exchange ----
static int quiescence(Board *bd, int alpha, int beta) {
    if ((++nodes & 4095) == 0) check_time();
    if (stop_search) return 0;

    int stand = evaluate(bd);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    MoveList ml;
    generate_legal_moves(bd, &ml);
    int scores[256];
    for (int i = 0; i < ml.count; i++) scores[i] = score_move(bd, ml.moves[i], MAX_PLY - 1);

    for (int i = 0; i < ml.count; i++) {
        pick_move(&ml, scores, i);
        int move = ml.moves[i];
        if (!M_CAP(move) && !M_PROMO(move)) continue;

        Undo undo;
        make_move(bd, move, &undo);
        int score = -quiescence(bd, -beta, -alpha);
        unmake_move(bd, move, &undo);
        if (stop_search) return 0;

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int negamax(Board *bd, int depth, int alpha, int beta, int ply) {
    pv_len[ply] = ply;

    if (ply > 0) {
        U64 h = board_hash(bd);
        hist[hist_len] = h;
        if (is_repetition(h) || bd->halfmove >= 100) return 0;
    }
    if (ply >= MAX_PLY - 1) return evaluate(bd);

    int K = bd->side == WHITE ? WK : BK;
    int in_check = is_square_attacked(bd, LSB(bd->bb[K]), !bd->side);
    if (in_check) depth++;  // check extension

    if (depth <= 0) return quiescence(bd, alpha, beta);

    if ((++nodes & 4095) == 0) check_time();
    if (stop_search) return 0;

    MoveList ml;
    generate_legal_moves(bd, &ml);
    if (ml.count == 0)
        return in_check ? -MATE_SCORE + ply : 0;

    int scores[256];
    for (int i = 0; i < ml.count; i++) scores[i] = score_move(bd, ml.moves[i], ply);

    for (int i = 0; i < ml.count; i++) {
        pick_move(&ml, scores, i);
        int move = ml.moves[i];

        Undo undo;
        make_move(bd, move, &undo);
        hist_len++;
        int score = -negamax(bd, depth - 1, -beta, -alpha, ply + 1);
        hist_len--;
        unmake_move(bd, move, &undo);
        if (stop_search) return 0;

        if (score >= beta) {
            if (!M_CAP(move)) {  // quiet cutoff → remember as killer
                killers[ply][1] = killers[ply][0];
                killers[ply][0] = move;
            }
            return beta;
        }
        if (score > alpha) {
            alpha = score;
            pv_tab[ply][ply] = move;
            for (int j = ply + 1; j < pv_len[ply + 1]; j++)
                pv_tab[ply][j] = pv_tab[ply + 1][j];
            pv_len[ply] = pv_len[ply + 1];
        }
    }
    return alpha;
}

int search_best_move(Board *bd, int movetime_ms, int max_depth) {
    nodes = 0;
    stop_search = 0;
    memset(killers, 0, sizeof(killers));
    hist_len = hist_base;
    hist[hist_len] = board_hash(bd);

    long long start = now_ms(), dl = start + movetime_ms;
    deadline.tv_sec = dl / 1000; deadline.tv_nsec = (dl % 1000) * 1000000LL;

    if (max_depth <= 0 || max_depth >= MAX_PLY) max_depth = MAX_PLY - 1;

    int best = 0;
    for (int depth = 1; depth <= max_depth; depth++) {
        int score = negamax(bd, depth, -INF_SCORE, INF_SCORE, 0);
        if (stop_search) break;  // partial iteration: keep previous best
        best = pv_tab[0][0];

        long long ms = now_ms() - start;
        printf("info depth %d score ", depth);
        if (score > MATE_SCORE - MAX_PLY)
            printf("mate %d", (MATE_SCORE - score + 1) / 2);
        else if (score < -MATE_SCORE + MAX_PLY)
            printf("mate %d", -(MATE_SCORE + score + 1) / 2);
        else
            printf("cp %d", score);
        printf(" nodes %lld time %lld pv", nodes, ms);
        for (int i = 0; i < pv_len[0]; i++) {
            char buf[6];
            move_to_str(pv_tab[0][i], buf);
            printf(" %s", buf);
        }
        printf("\n");
        fflush(stdout);

        if (score > MATE_SCORE - MAX_PLY) break;          // mate found; stop
        if (now_ms() - start > movetime_ms / 2) break;    // next depth won't finish
    }

    if (!best) {  // never finished depth 1: fall back to first legal move
        MoveList ml;
        generate_legal_moves(bd, &ml);
        if (ml.count) best = ml.moves[0];
    }
    return best;
}
