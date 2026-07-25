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
#include "attacks.h"
#include "magic.h"

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
    if (e->key == key && e->depth > depth) return;  // keep the deeper result
    if (score > MATE_SCORE - 2 * MAX_PLY) score += ply;
    else if (score < -MATE_SCORE + 2 * MAX_PLY) score -= ply;
    e->key = key; e->move = move;
    e->score = (short)score;
    e->depth = (unsigned char)(depth > 0 ? depth : 0);
    e->flag = (unsigned char)flag;
}

// ---- Route book: persistent root-search results across games ----
// The nondeterministic side of the problem, cached: every completed root
// search's verdict (position -> best move, depth, score, certainty) is kept
// forever. Recurring positions — and our games repeat lines heavily — route
// through the stored certificate instead of re-searching. Trust requires a
// deep, fully-certain entry; anything weaker only seeds move ordering and
// the aspiration window, so bad routes stay refutable.
#define BOOK_SIZE   (1 << 16)
#define BOOK_PROBE  8
#define BOOK_TRUST_DEPTH 8
#define BOOK_ANCHOR_CERT 50   // min certainty to inherit a book verdict mid-search

typedef struct {
    U64 key;
    char uci[6];
    short score;
    unsigned char depth, certainty;
} BookEntry;

static BookEntry book[BOOK_SIZE];
static char book_path[512];
static int book_enabled = 0;

static BookEntry *book_slot(U64 key, int for_insert) {
    BookEntry *shallowest = NULL;
    for (int i = 0; i < BOOK_PROBE; i++) {
        BookEntry *e = &book[(key + i) & (BOOK_SIZE - 1)];
        if (e->key == key) return e;
        if (for_insert) {
            if (!e->key) return e;
            if (!shallowest || e->depth < shallowest->depth) shallowest = e;
        }
    }
    return for_insert ? shallowest : NULL;
}

static void book_remember(U64 key, int depth, int score, int certainty,
                          const char *uci, int persist) {
    BookEntry *e = book_slot(key, 1);
    if (!e) return;
    if (e->key == key && e->depth >= depth) return;   // keep the deeper route
    e->key = key;
    e->score = (short)score;
    e->depth = (unsigned char)depth;
    e->certainty = (unsigned char)certainty;
    strncpy(e->uci, uci, 5);
    e->uci[5] = 0;

    if (persist && book_path[0]) {
        FILE *f = fopen(book_path, "a");
        if (f) {
            fprintf(f, "%016llx %d %d %d %s\n",
                    (unsigned long long)key, depth, score, certainty, uci);
            fclose(f);
        }
    }
}

int book_load(const char *path, int force_enable) {
    strncpy(book_path, path, sizeof(book_path) - 1);
    book_path[sizeof(book_path) - 1] = 0;
    int n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        unsigned long long key;
        int depth, score, cert;
        char uci[8];
        while (fscanf(f, "%llx %d %d %d %7s", &key, &depth, &score, &cert, uci) == 5) {
            book_remember((U64)key, depth, score, cert, uci, 0);
            n++;
        }
        fclose(f);
    }
    book_enabled = (n > 0) || force_enable;
    return n;
}

// ---- Search state ----
static U64 hist[2048];          // game history + current search path
static int hist_base;           // entries provided by the game
static int hist_len;

static long long nodes;
long long search_node_limit = 0;   // 0 = unlimited; set by uci.c "go nodes N"
int lmr_deep = 0;                  // CHESS_LMR2=1: add a depth term to LMR
static int stop_search;
static struct timespec deadline;
static int killers[MAX_PLY][2];
static int history_tab[12][64];   // quiet-move ordering, bumped on cutoffs
static int counter_move[12][64];  // quiet refutation of [prev piece][prev to]
static int move_stack[MAX_PLY];   // move played at each ply (0 after null)

static int pv_len[MAX_PLY];
static int pv_tab[MAX_PLY][MAX_PLY];

// The game plan the eval prices pieces against: squares the last completed
// iteration's PV fights over, per side. See search.h.
U64 plan_squares[2] = { 0, 0 };

// How certain is the plan's execution? Measured, not asserted: the share
// of the previous depth's opening PV moves that survived into this depth's
// PV. A plan no deeper search can refute is unstoppable (100); a PV that
// churns every iteration is contested (0). Inflated learned piece values
// are realised in proportion to this.
int plan_certainty = 0;
int search_last_score = 0;   // root score of the last search_best_move call
int search_quiet = 0;        // suppress "info depth" output (offline book build)
int book_anchor_on = 0;      // in-search book anchoring; measured neutral, off by
                             // default (CHESS_ANCHOR=1 opts in for experiments)

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
    if (search_node_limit && nodes >= search_node_limit) { stop_search = 1; return; }
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

// ---- Static exchange evaluation ----
// Play out the full capture sequence on one square, each side always
// recapturing with its least valuable attacker, then minimax the gains
// backwards. Recomputing the attacker set after every removal handles
// x-rays (rook behind rook, bishop behind pawn) for free.
static U64 attackers_to(const Board *bd, int sq, U64 occ) {
    return (pawn_attacks[WHITE][sq] & bd->bb[BP])
         | (pawn_attacks[BLACK][sq] & bd->bb[WP])
         | (knight_attacks[sq] & (bd->bb[WN] | bd->bb[BN]))
         | (king_attacks[sq]   & (bd->bb[WK] | bd->bb[BK]))
         | (get_bishop_attacks(sq, occ)
            & (bd->bb[WB] | bd->bb[BB_] | bd->bb[WQ] | bd->bb[BQ]))
         | (get_rook_attacks(sq, occ)
            & (bd->bb[WR] | bd->bb[BR] | bd->bb[WQ] | bd->bb[BQ]));
}

static int see(const Board *bd, int move) {
    if (M_CASTLE(move)) return 0;
    int to = M_TO(move);

    int gain[32], d = 0;
    gain[0] = M_EP(move) ? piece_val[0]
            : M_CAP(move) ? piece_val[victim_on(bd, to, !bd->side)] : 0;

    int apc = M_PIECE(move) % 6;      // piece currently standing on 'to'
    U64 occ = bd->occ[BOTH] ^ (1ULL << M_FROM(move));
    if (M_EP(move))
        occ ^= 1ULL << (bd->side == WHITE ? to - 8 : to + 8);
    int stm = !bd->side;              // side to recapture next

    while (d < 30) {
        U64 att = attackers_to(bd, to, occ) & occ;
        int base = stm == WHITE ? WP : BP;
        U64 fb = 0;
        int pt = -1;
        for (int p = 0; p < 6; p++) {  // least valuable attacker first
            U64 b = bd->bb[base + p] & att;
            if (b) { pt = p; fb = b & -b; break; }
        }
        if (pt < 0) break;
        d++;
        gain[d] = piece_val[apc] - gain[d - 1];  // capture the piece on 'to'
        occ ^= fb;
        apc = pt;
        stm = !stm;
    }
    while (d > 0) {                    // negamax the sequence backwards
        int stand = -gain[d - 1];
        gain[d - 1] = -(gain[d] > stand ? gain[d] : stand);
        d--;
    }
    return gain[0];
}

static int score_move(const Board *bd, int move, int ply, int tt_move, int prev) {
    if (move == tt_move) return 1000000;   // hash move first, always
    int s = 0;
    if (M_CAP(move)) {
        int victim = M_EP(move) ? 0 : victim_on(bd, M_TO(move), !bd->side);
        if (see(bd, move) >= 0)  // winning/even captures above killers
            s = 100000 + piece_val[victim] * 10 - piece_val[M_PIECE(move) % 6];
        else                     // losing captures below quiet history
            s = -30000 + piece_val[victim];
    } else if (move == killers[ply][0]) s = 90000;
    else if (move == killers[ply][1])   s = 80000;
    else if (prev && move == counter_move[M_PIECE(prev)][M_TO(prev)]) s = 75000;
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
static int quiescence(Board *bd, int alpha, int beta, int ply) {
    if ((++nodes & 4095) == 0) check_time();
    if (stop_search) return 0;
    if (ply >= MAX_PLY - 1) return evaluate(bd);

    U64 key = board_hash(bd);
    int tt_move;
    int tt_score = tt_probe(key, 0, alpha, beta, ply, &tt_move);
    if (tt_score != TT_MISS) return tt_score;

    int stand = evaluate(bd);
    if (stand >= beta) {
        tt_store(key, 0, beta, TT_BETA, 0, ply);
        return beta;
    }
    if (stand > alpha) alpha = stand;

    MoveList ml;
    generate_legal_moves(bd, &ml);
    int scores[256];
    for (int i = 0; i < ml.count; i++)
        scores[i] = score_move(bd, ml.moves[i], MAX_PLY - 1, tt_move, 0);

    int flag = TT_ALPHA, best_move = 0;
    for (int i = 0; i < ml.count; i++) {
        pick_move(&ml, scores, i);
        int move = ml.moves[i];
        if (!M_CAP(move) && !M_PROMO(move)) continue;

        if (M_CAP(move) && !M_PROMO(move)) {
            int victim = M_EP(move) ? 0 : victim_on(bd, M_TO(move), !bd->side);
            // Delta pruning: even winning this piece can't lift us to alpha
            if (stand + piece_val[victim] + 200 <= alpha) continue;
            // Losing exchanges aren't worth resolving in quiescence
            if (see(bd, move) < 0) continue;
        }

        Undo undo;
        make_move(bd, move, &undo);
        int score = -quiescence(bd, -beta, -alpha, ply + 1);
        unmake_move(bd, move, &undo);
        if (stop_search) return 0;

        if (score >= beta) {
            tt_store(key, 0, beta, TT_BETA, move, ply);
            return beta;
        }
        if (score > alpha) { alpha = score; flag = TT_EXACT; best_move = move; }
    }
    tt_store(key, 0, alpha, flag, best_move, ply);
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

    if (depth <= 0) return quiescence(bd, alpha, beta, ply);

    if ((++nodes & 4095) == 0) check_time();
    if (stop_search) return 0;

    // TT probe (never cut at PV nodes or the root — keeps the PV honest)
    int tt_move;
    int tt_score = tt_probe(key, depth, alpha, beta, ply, &tt_move);
    if (tt_score != TT_MISS && ply > 0 && !is_pv) return tt_score;

    // Borrowed horizon: this exact position may already carry an offline
    // opening verdict deeper than we can compute here. If the route book knows
    // it at least as deep, inherit that score — the search's horizon jumps
    // forward along theory lines, and transpositions hit for free (same Zobrist
    // key). The stored score is an EXACT deep verdict (like a tablebase hit),
    // so returning it is sound even at a PV node; we only skip the root (ply 0,
    // where the root probe already runs) and mate-range scores (whose distance
    // is ply-relative and not stored that way).
    if (book_enabled && book_anchor_on && ply > 0) {
        BookEntry *be = book_slot(key, 0);
        if (be && be->depth >= depth && be->certainty >= BOOK_ANCHOR_CERT
            && be->score <  MATE_SCORE - MAX_PLY
            && be->score > -MATE_SCORE + MAX_PLY)
            return be->score;
    }

    // Null-move pruning: hand the opponent a free move; if the reduced
    // search still fails high, a real move surely would too. Skipped when
    // in check, in possible zugzwang (pawn-only), or right after a null.
    if (can_null && !in_check && !is_pv && depth >= 3 && ply > 0
        && has_big_pieces(bd)
        && beta < MATE_SCORE - 2 * MAX_PLY && beta > -MATE_SCORE + 2 * MAX_PLY) {
        int old_ep = bd->ep;
        bd->side = !bd->side;
        bd->ep = NO_SQ;
        move_stack[ply] = 0;
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

    // Futility: at shallow depth with a hopeless static eval, quiet moves
    // that don't give check can't recover — skip them (never the first move)
    int futile = 0;
    if (depth <= 3 && !in_check && !is_pv
        && alpha > -MATE_SCORE + 2 * MAX_PLY && alpha < MATE_SCORE - 2 * MAX_PLY) {
        static const int margin[4] = { 0, 150, 300, 500 };
        if (evaluate(bd) + margin[depth] <= alpha) futile = 1;
    }

    int prev = ply > 0 ? move_stack[ply - 1] : 0;
    int scores[256];
    for (int i = 0; i < ml.count; i++)
        scores[i] = score_move(bd, ml.moves[i], ply, tt_move, prev);

    int best_move = 0;
    int flag = TT_ALPHA;
    for (int i = 0; i < ml.count; i++) {
        pick_move(&ml, scores, i);
        int move = ml.moves[i];

        Undo undo;
        make_move(bd, move, &undo);
        move_stack[ply] = move;
        hist_len++;

        if (futile && i > 0 && !M_CAP(move) && !M_PROMO(move)) {
            int ok = bd->side == WHITE ? WK : BK;  // side just moved's opponent
            if (!is_square_attacked(bd, LSB(bd->bb[ok]), !bd->side)) {
                hist_len--;
                unmake_move(bd, move, &undo);
                continue;
            }
        }

        int score;
        if (i == 0) {
            score = -negamax(bd, depth - 1, -beta, -alpha, ply + 1, 1);
        } else {
            // Late move reductions: quiet moves sorted far down the list
            // rarely raise alpha — try them shallower with a null window,
            // and only pay full price if they surprise us. Killers and
            // countermoves keep their full depth.
            int red = 0;
            if (depth >= 3 && i >= 3 && !in_check
                && !M_CAP(move) && !M_PROMO(move)
                && move != killers[ply][0] && move != killers[ply][1]) {
                red = 1 + (i >= 8) + (i >= 16);
                if (lmr_deep) red += (depth >= 6) + (depth >= 12);  // cut deeper
                if (red > depth - 2) red = depth - 2;
            }
            score = -negamax(bd, depth - 1 - red, -alpha - 1, -alpha, ply + 1, 1);
            if (score > alpha)  // fail high → verify at full depth/window
                score = -negamax(bd, depth - 1, -beta, -alpha, ply + 1, 1);
        }

        hist_len--;
        unmake_move(bd, move, &undo);
        if (stop_search) return 0;

        if (score >= beta) {
            if (!M_CAP(move)) {  // quiet cutoff → killers/counter/history credit
                killers[ply][1] = killers[ply][0];
                killers[ply][0] = move;
                if (prev)
                    counter_move[M_PIECE(prev)][M_TO(prev)] = move;
                int *h = &history_tab[M_PIECE(move)][M_TO(move)];
                *h += depth * depth;
                if (*h > 60000)  // keep quiets below the killer band
                    for (int p = 0; p < 12; p++)
                        for (int s = 0; s < 64; s++)
                            history_tab[p][s] /= 2;
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
    plan_squares[WHITE] = plan_squares[BLACK] = 0;  // fresh plan per search
    plan_certainty = 0;
    int prev_pv[8], prev_pv_len = 0;

    long long start = now_ms(), dl = start + movetime_ms;
    deadline.tv_sec = dl / 1000; deadline.tv_nsec = (dl % 1000) * 1000000LL;

    if (max_depth <= 0 || max_depth >= MAX_PLY) max_depth = MAX_PLY - 1;

    int best = 0, prev_score = 0, done_depth = 0;

    // Route book: reuse this position's stored certificate if we have one.
    U64 root_key = hist[hist_len];
    if (book_enabled) {
        BookEntry *be = book_slot(root_key, 0);
        if (be) {
            // Resolve the stored UCI against the CURRENT legal moves — the
            // book never plays an illegal or stale move.
            int bmove = 0;
            MoveList ml;
            generate_legal_moves(bd, &ml);
            for (int i = 0; i < ml.count; i++) {
                char buf[6];
                move_to_str(ml.moves[i], buf);
                if (strcmp(buf, be->uci) == 0) { bmove = ml.moves[i]; break; }
            }
            if (bmove) {
                // Repetition safety: never instant-trust a route through a
                // position this game has already visited.
                int seen = 0;
                for (int i = 0; i < hist_len; i++)
                    if (hist[i] == root_key) { seen = 1; break; }

                // Full trust only for deep, fully-certain certificates on
                // time-managed searches (depth-limited callers are probing).
                if (!seen && be->certainty == 100
                    && be->depth >= BOOK_TRUST_DEPTH
                    && max_depth >= MAX_PLY - 1) {
                    printf("info string book route depth %d score %d\n",
                           be->depth, be->score);
                    fflush(stdout);
                    return bmove;
                }
                // Otherwise seed: order the book move first via the TT and
                // start aspiration around the remembered score.
                printf("info string book seed %s depth %d\n", be->uci, be->depth);
                tt_store(root_key, 0, be->score, TT_EXACT, bmove, 0);
                prev_score = be->score;
            }
        }
    }
    for (int depth = 1; depth <= max_depth; depth++) {
        // Aspiration window around the last score; widen on a miss
        int alpha = depth >= 4 ? prev_score - 40 : -INF_SCORE;
        int beta  = depth >= 4 ? prev_score + 40 :  INF_SCORE;
        int score = negamax(bd, depth, alpha, beta, 0, 1);
        if (!stop_search && (score <= alpha || score >= beta))
            score = negamax(bd, depth, -INF_SCORE, INF_SCORE, 0, 1);
        if (stop_search) break;  // partial iteration: keep previous best
        prev_score = score;
        best = pv_tab[0][0];
        done_depth = depth;

        // Publish this depth's plan for the next iteration's eval: the
        // squares each side's part of the line moves through.
        plan_squares[WHITE] = plan_squares[BLACK] = 0;
        int mover = bd->side;
        for (int i = 0; i < pv_len[0]; i++) {
            plan_squares[mover] |= (1ULL << M_FROM(pv_tab[0][i]))
                                 | (1ULL << M_TO(pv_tab[0][i]));
            mover = !mover;
        }

        // Certainty: how much of the previous depth's plan survived the
        // deeper look. Same opening moves at greater depth means the
        // opponent has no refutation yet — full execution in sight.
        if (prev_pv_len > 0) {
            int check = prev_pv_len < pv_len[0] ? prev_pv_len : pv_len[0];
            if (check > 6) check = 6;
            int same = 0;
            for (int i = 0; i < check; i++)
                if (pv_tab[0][i] == prev_pv[i]) same++;
            plan_certainty = check ? 100 * same / check : 0;
        }
        prev_pv_len = pv_len[0] < 8 ? pv_len[0] : 8;
        for (int i = 0; i < prev_pv_len; i++) prev_pv[i] = pv_tab[0][i];

        if (!search_quiet) {
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
        }

        if (score > MATE_SCORE - MAX_PLY) break;          // mate found; stop
        if (now_ms() - start > movetime_ms / 2) break;    // next depth won't finish
    }

    if (!best) {  // never finished depth 1: fall back to first legal move
        MoveList ml;
        generate_legal_moves(bd, &ml);
        if (ml.count) best = ml.moves[0];
    }

    // Record the route: this position's verdict joins the permanent book.
    if (book_enabled && best && done_depth > 0) {
        char buf[6];
        move_to_str(best, buf);
        book_remember(root_key, done_depth, prev_score, plan_certainty, buf, 1);
    }
    search_last_score = prev_score;   // exposed for offline book building (mkbook)
    return best;
}
