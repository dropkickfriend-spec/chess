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

// ---- Transposition table ----
// 4M entries * 16 bytes = 64MB. Persistent across moves so later searches
// reuse earlier work. Scores are stored ply-adjusted so mate distances stay
// correct when a position is reached at a different depth in the tree.
#define TT_SIZE (1 << 22)
enum { TT_EXACT, TT_ALPHA, TT_BETA };   // exact / upper bound / lower bound

typedef struct {
    U64 key;
    int move;
    short score;
    unsigned char depth;
    unsigned char flag;
} TTEntry;

static TTEntry tt[TT_SIZE];
#define TT_MISS (-INF_SCORE - 1)

static int tt_probe(U64 key, int depth, int alpha, int beta, int ply, int *move) {
    TTEntry *e = &tt[key & (TT_SIZE - 1)];
    *move = 0;
    if (e->key != key) return TT_MISS;
    *move = e->move;                    // ordering hint even if depth too low
    if (e->depth < depth) return TT_MISS;

    int score = e->score;
    if (score > MATE_SCORE - 2 * MAX_PLY) score -= ply;
    else if (score < -MATE_SCORE + 2 * MAX_PLY) score += ply;

    if (e->flag == TT_EXACT) return score;
    if (e->flag == TT_ALPHA && score <= alpha) return alpha;
    if (e->flag == TT_BETA && score >= beta) return beta;
    return TT_MISS;
}

static void tt_store(U64 key, int depth, int score, int flag, int move, int ply) {
    TTEntry *e = &tt[key & (TT_SIZE - 1)];
    if (score > MATE_SCORE - 2 * MAX_PLY) score += ply;
    else if (score < -MATE_SCORE + 2 * MAX_PLY) score -= ply;
    e->key = key; e->move = move;
    e->score = (short)score;
    e->depth = (unsigned char)(depth > 0 ? depth : 0);
    e->flag = (unsigned char)flag;
}

// ---- Search state ----
static U64 hist[2048];          // game history + current search path
static int hist_base;           // entries provided by the game
static int hist_len;

static long long nodes;
static int stop_search;
static struct timespec deadline;
static int killers[MAX_PLY][2];
static int history_tab[12][64];  // quiet-move ordering, bumped on cutoffs

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

static int score_move(const Board *bd, int move, int ply, int tt_move) {
    if (move == tt_move) return 1000000;   // hash move first, always
    int s = 0;
    if (M_CAP(move)) {
        int victim = M_EP(move) ? 0 : victim_on(bd, M_TO(move), !bd->side);
        s = 100000 + piece_val[victim] * 10 - piece_val[M_PIECE(move) % 6];
    } else if (move == killers[ply][0]) s = 90000;
    else if (move == killers[ply][1])   s = 80000;
    else s = history_tab[M_PIECE(move)][M_TO(move)];  // quiets by history
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
    for (int i = 0; i < ml.count; i++) scores[i] = score_move(bd, ml.moves[i], MAX_PLY - 1, 0);

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

// Side to move has any non-pawn piece (null-move zugzwang guard)
static int has_big_pieces(const Board *bd) {
    int lo = bd->side == WHITE ? WN : BN;
    return (bd->bb[lo] | bd->bb[lo+1] | bd->bb[lo+2] | bd->bb[lo+3]) != 0;
}

static int negamax(Board *bd, int depth, int alpha, int beta, int ply, int can_null) {
    pv_len[ply] = ply;
    int is_pv = beta - alpha > 1;

    U64 key = board_hash(bd);
    if (ply > 0) {
        hist[hist_len] = key;
        if (is_repetition(key) || bd->halfmove >= 100) return 0;
    }
    if (ply >= MAX_PLY - 1) return evaluate(bd);

    int K = bd->side == WHITE ? WK : BK;
    int in_check = is_square_attacked(bd, LSB(bd->bb[K]), !bd->side);
    if (in_check) depth++;  // check extension

    if (depth <= 0) return quiescence(bd, alpha, beta);

    if ((++nodes & 4095) == 0) check_time();
    if (stop_search) return 0;

    // TT probe (never cut at PV nodes or the root — keeps the PV honest)
    int tt_move;
    int tt_score = tt_probe(key, depth, alpha, beta, ply, &tt_move);
    if (tt_score != TT_MISS && ply > 0 && !is_pv) return tt_score;

    // Null-move pruning: hand the opponent a free move; if the reduced
    // search still fails high, a real move surely would too. Skipped when
    // in check, in possible zugzwang (pawn-only), or right after a null.
    if (can_null && !in_check && !is_pv && depth >= 3 && ply > 0
        && has_big_pieces(bd)
        && beta < MATE_SCORE - 2 * MAX_PLY && beta > -MATE_SCORE + 2 * MAX_PLY) {
        int old_ep = bd->ep;
        bd->side = !bd->side;
        bd->ep = NO_SQ;
        int score = -negamax(bd, depth - 3, -beta, -beta + 1, ply + 1, 0);
        bd->side = !bd->side;
        bd->ep = old_ep;
        if (stop_search) return 0;
        if (score >= beta) return beta;
    }

    MoveList ml;
    generate_legal_moves(bd, &ml);
    if (ml.count == 0)
        return in_check ? -MATE_SCORE + ply : 0;

    int scores[256];
    for (int i = 0; i < ml.count; i++) scores[i] = score_move(bd, ml.moves[i], ply, tt_move);

    int best_move = 0;
    int flag = TT_ALPHA;
    for (int i = 0; i < ml.count; i++) {
        pick_move(&ml, scores, i);
        int move = ml.moves[i];

        Undo undo;
        make_move(bd, move, &undo);
        hist_len++;

        int score;
        if (i == 0) {
            score = -negamax(bd, depth - 1, -beta, -alpha, ply + 1, 1);
        } else {
            // Late move reductions: quiet moves sorted far down the list
            // rarely raise alpha — try them shallower with a null window,
            // and only pay full price if they surprise us.
            int red = 0;
            if (depth >= 3 && i >= 4 && !in_check
                && !M_CAP(move) && !M_PROMO(move))
                red = 1 + (i >= 12);
            score = -negamax(bd, depth - 1 - red, -alpha - 1, -alpha, ply + 1, 1);
            if (score > alpha)  // fail high → verify at full depth/window
                score = -negamax(bd, depth - 1, -beta, -alpha, ply + 1, 1);
        }

        hist_len--;
        unmake_move(bd, move, &undo);
        if (stop_search) return 0;

        if (score >= beta) {
            if (!M_CAP(move)) {  // quiet cutoff → killers + history credit
                killers[ply][1] = killers[ply][0];
                killers[ply][0] = move;
                history_tab[M_PIECE(move)][M_TO(move)] += depth * depth;
            }
            tt_store(key, depth, beta, TT_BETA, move, ply);
            return beta;
        }
        if (score > alpha) {
            alpha = score;
            best_move = move;
            flag = TT_EXACT;
            pv_tab[ply][ply] = move;
            for (int j = ply + 1; j < pv_len[ply + 1]; j++)
                pv_tab[ply][j] = pv_tab[ply + 1][j];
            pv_len[ply] = pv_len[ply + 1];
        }
    }
    tt_store(key, depth, alpha, flag, best_move ? best_move : tt_move, ply);
    return alpha;
}

int search_best_move(Board *bd, int movetime_ms, int max_depth) {
    nodes = 0;
    stop_search = 0;
    memset(killers, 0, sizeof(killers));
    memset(history_tab, 0, sizeof(history_tab));
    hist_len = hist_base;
    hist[hist_len] = board_hash(bd);

    long long start = now_ms(), dl = start + movetime_ms;
    deadline.tv_sec = dl / 1000; deadline.tv_nsec = (dl % 1000) * 1000000LL;

    if (max_depth <= 0 || max_depth >= MAX_PLY) max_depth = MAX_PLY - 1;

    int best = 0;
    for (int depth = 1; depth <= max_depth; depth++) {
        int score = negamax(bd, depth, -INF_SCORE, INF_SCORE, 0, 1);
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
