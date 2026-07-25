// eval_endgame.c — exact endgame recognizers.
//
// WHY THIS EXISTS (measured, not assumed). Given the same position and the same
// material, differing only in side to move:
//
//   8/8/8/3k4/8/3K4/3P4/8   White to move -> theoretical DRAW, engine said +371
//                           Black to move -> White WINS,       engine said +390
//
// The engine could not tell a won K+P vs K from a drawn one, at depth 20. That
// is not a search failure -- the search faithfully propagated a wrong leaf
// score. And search cannot rescue it: proving that draw needs ~50 plies, which
// at the measured branching factor of 2.09 is ~1.7e10 times the nodes. A 2x
// faster search buys 0.94 ply. The evaluation has to KNOW.
//
// DESIGN RULE: UNDER-CLAIM. A false draw throws away a won game; a missed
// recognizer merely leaves us exactly where we already were. So every rule here
// fires only when the result is certain, and anything doubtful is left alone.
#include "board.h"
#include "attacks.h"

extern int CHEB(int a, int b);
extern U64 file_mask[8];

#define LIGHT_SQUARES 0x55AA55AA55AA55AAULL

static int same_colour_as(int sq, int other) {
    return (int)((LIGHT_SQUARES >> sq) & 1) == (int)((LIGHT_SQUARES >> other) & 1);
}

// No pawns and nobody holds enough material to force mate.
// Mate needs two bishops, or bishop+knight, or a rook/queen. A lone minor
// cannot mate, and neither can two knights against a bare king.
static int insufficient_material(const Board *bd) {
    if (bd->bb[WP] | bd->bb[BP]) return 0;
    if (bd->bb[WR] | bd->bb[WQ] | bd->bb[BR] | bd->bb[BQ]) return 0;
    int wn = COUNT(bd->bb[WN]), wb = COUNT(bd->bb[WB]);
    int bn = COUNT(bd->bb[BN]), bb = COUNT(bd->bb[BB_]);
    int w_can_mate = (wb >= 2) || (wb >= 1 && wn >= 1);
    int b_can_mate = (bb >= 2) || (bb >= 1 && bn >= 1);
    return !w_can_mate && !b_can_mate;
}

// Rook pawn(s) plus a bishop that does not control the promotion square: the
// defending king simply sits in the corner and cannot be evicted.
// Strict on purpose — the attacker may hold ONLY rook pawns and bishops, and
// the defender only a bare king next to the promotion square.
static int wrong_bishop_rook_pawn(const Board *bd, int side) {
    int base = side * 6, ebase = (!side) * 6;
    if (bd->bb[base + 1] | bd->bb[base + 3] | bd->bb[base + 4]) return 0;  // N/R/Q
    U64 pawns = bd->bb[base];
    if (!pawns) return 0;
    int is_a = !(pawns & ~file_mask[0]);
    int is_h = !(pawns & ~file_mask[7]);
    if (!is_a && !is_h) return 0;                       // not purely rook pawns
    // defender must be down to a bare king
    if (bd->bb[ebase] | bd->bb[ebase + 1] | bd->bb[ebase + 2]
        | bd->bb[ebase + 3] | bd->bb[ebase + 4]) return 0;

    int promo = (side == WHITE ? 56 : 0) + (is_a ? 0 : 7);
    U64 b = bd->bb[base + 2];
    while (b) {                                          // a right-coloured
        int s = LSB(b); POP_BIT(b, s);                   // bishop wins normally
        if (same_colour_as(s, promo)) return 0;
    }
    return CHEB(LSB(bd->bb[ebase + 5]), promo) <= 1;     // king holds the corner
}

// ---- KPK bitbase -----------------------------------------------------------
// Hand-written opposition rules were tried first and were far too narrow: they
// fired only in one exact king configuration, and the search stepped out of it
// on the very first move, so the root score never changed. K+P vs K has to be
// solved EXACTLY, which is what retrograde analysis does -- every legal
// position, once, at startup.
//
// index = ((stm * 64 + wk) * 64 + bk) * 64 + pawn, White always the attacker
// (Black-pawn positions are mirrored before probing).
#define KPK_N (2 * 64 * 64 * 64)
enum { KPK_UNKNOWN = 0, KPK_INVALID, KPK_DRAW, KPK_WIN };
static unsigned char kpk_tb[KPK_N];
static int kpk_ready = 0;

static int kpk_idx(int stm, int wk, int bk, int p) {
    return ((stm * 64 + wk) * 64 + bk) * 64 + p;
}

static int adjacent(int a, int b) { return CHEB(a, b) <= 1; }

// Classify what is already decided without looking further.
static int kpk_terminal(int stm, int wk, int bk, int p) {
    int pr = p / 8, pf = p % 8;
    if (pr == 0 || pr == 7) return KPK_INVALID;      // pawn cannot sit there
    if (wk == bk || wk == p || bk == p) return KPK_INVALID;
    if (adjacent(wk, bk)) return KPK_INVALID;        // kings never touch
    // side not to move may not be in check: only the black king can be, and
    // only from the pawn.
    if (stm == 0) {                                   // White to move
        if ((pf > 0 && bk == p + 7) || (pf < 7 && bk == p + 9))
            return KPK_INVALID;                       // black king en prise
    }
    if (stm == 1) {                                   // Black to move
        // black may capture an undefended pawn -> bare kings -> draw
        if (adjacent(bk, p) && !adjacent(wk, p)) return KPK_DRAW;
    }
    return KPK_UNKNOWN;
}

static void kpk_init(void) {
    if (kpk_ready) return;
    for (int i = 0; i < KPK_N; i++) kpk_tb[i] = KPK_UNKNOWN;
    for (int stm = 0; stm < 2; stm++)
        for (int wk = 0; wk < 64; wk++)
            for (int bk = 0; bk < 64; bk++)
                for (int p = 8; p < 56; p++)
                    kpk_tb[kpk_idx(stm, wk, bk, p)] = kpk_terminal(stm, wk, bk, p);

    // Retrograde sweep: White to move wins if ANY move wins; Black to move
    // draws if ANY move draws. Repeat until nothing changes.
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int stm = 0; stm < 2; stm++)
        for (int wk = 0; wk < 64; wk++)
        for (int bk = 0; bk < 64; bk++)
        for (int p = 8; p < 56; p++) {
            int i = kpk_idx(stm, wk, bk, p);
            if (kpk_tb[i] != KPK_UNKNOWN) continue;
            int any_win = 0, any_draw = 0, any_move = 0, all_win = 1, all_draw = 1;

            if (stm == 0) {                            // White: king or pawn
                U64 km = king_attacks[wk];
                while (km) {
                    int to = LSB(km); POP_BIT(km, to);
                    if (to == bk || adjacent(to, bk) || to == p) continue;
                    any_move = 1;
                    int r = kpk_tb[kpk_idx(1, to, bk, p)];
                    if (r == KPK_WIN) any_win = 1; else all_win = 0;
                    if (r != KPK_DRAW) all_draw = 0;
                }
                int one = p + 8;
                if (one < 56 && one != wk && one != bk) {   // single push
                    any_move = 1;
                    int r = kpk_tb[kpk_idx(1, wk, bk, one)];
                    if (r == KPK_WIN) any_win = 1; else all_win = 0;
                    if (r != KPK_DRAW) all_draw = 0;
                } else if (one >= 56 && one != wk && one != bk) {
                    // promotes to a queen: won unless the bare king grabs it
                    any_move = 1;
                    if (!(adjacent(bk, one) && !adjacent(wk, one))) any_win = 1;
                    else all_win = 0, all_draw = 0;
                }
                if (p / 8 == 1) {                            // double push
                    int two = p + 16;
                    if (p + 8 != wk && p + 8 != bk && two != wk && two != bk) {
                        any_move = 1;
                        int r = kpk_tb[kpk_idx(1, wk, bk, two)];
                        if (r == KPK_WIN) any_win = 1; else all_win = 0;
                        if (r != KPK_DRAW) all_draw = 0;
                    }
                }
                if (any_win)          { kpk_tb[i] = KPK_WIN;  changed = 1; }
                else if (any_move && all_draw) { kpk_tb[i] = KPK_DRAW; changed = 1; }
                else if (!any_move)   { kpk_tb[i] = KPK_DRAW; changed = 1; }  // stalemate
            } else {                                   // Black: king only
                U64 km = king_attacks[bk];
                while (km) {
                    int to = LSB(km); POP_BIT(km, to);
                    if (to == wk || adjacent(to, wk)) continue;
                    if (to == p) {                      // capture the pawn
                        if (adjacent(wk, p)) continue;  // defended
                        any_move = 1; any_draw = 1; all_win = 0; continue;
                    }
                    any_move = 1;
                    int r = kpk_tb[kpk_idx(0, wk, to, p)];
                    if (r == KPK_DRAW) any_draw = 1; else all_draw = 0;
                    if (r != KPK_WIN) all_win = 0;
                }
                if (!any_move) { kpk_tb[i] = KPK_DRAW; changed = 1; }  // stalemate
                else if (any_draw)      { kpk_tb[i] = KPK_DRAW; changed = 1; }
                else if (all_win)       { kpk_tb[i] = KPK_WIN;  changed = 1; }
            }
        }
    }
    // Anything still unresolved could not be shown to be a win: call it a draw,
    // which is the safe direction (we never invent a win we cannot prove).
    for (int i = 0; i < KPK_N; i++)
        if (kpk_tb[i] == KPK_UNKNOWN) kpk_tb[i] = KPK_DRAW;
    kpk_ready = 1;
}

// Exact verdict for K+P vs K. Returns 1 if the position is a DRAW.
static int kpk_bitbase_draw(const Board *bd) {
    int wp = COUNT(bd->bb[WP]), bp = COUNT(bd->bb[BP]);
    if (bd->bb[WN] | bd->bb[WB] | bd->bb[WR] | bd->bb[WQ]) return 0;
    if (bd->bb[BN] | bd->bb[BB_] | bd->bb[BR] | bd->bb[BQ]) return 0;
    if (wp + bp != 1) return 0;

    kpk_init();
    int strong = wp ? WHITE : BLACK;
    int p  = LSB(bd->bb[strong * 6]);
    int ak = LSB(bd->bb[strong * 6 + 5]);
    int dk = LSB(bd->bb[(!strong) * 6 + 5]);
    int stm = (bd->side == strong) ? 0 : 1;      // 0 = attacker to move
    if (strong == BLACK) {                        // mirror onto White
        p ^= 56; ak ^= 56; dk ^= 56;
    }
    return kpk_tb[kpk_idx(stm, ak, dk, p)] != KPK_WIN;
}

// Superseded by the bitbase; kept for the rook-pawn corner case it states
// plainly, which the bitbase also covers.
__attribute__((unused)) static int kpk_draw(const Board *bd) {
    int wp = COUNT(bd->bb[WP]), bp = COUNT(bd->bb[BP]);
    if (bd->bb[WN] | bd->bb[WB] | bd->bb[WR] | bd->bb[WQ]) return 0;
    if (bd->bb[BN] | bd->bb[BB_] | bd->bb[BR] | bd->bb[BQ]) return 0;
    if (wp + bp != 1) return 0;                          // exactly one pawn

    int side = wp ? WHITE : BLACK;                       // the attacker
    int p  = LSB(bd->bb[side * 6]);
    int ak = LSB(bd->bb[side * 6 + 5]);
    int dk = LSB(bd->bb[(!side) * 6 + 5]);
    int pf = p % 8, pr = p / 8;
    int dkf = dk % 8, dkr = dk / 8;
    int akf = ak % 8, akr = ak / 8;
    int fwd = side == WHITE ? 1 : -1;                    // direction of travel
    int promo = (side == WHITE ? 56 : 0) + pf;

    // (1) Rook pawn: the defending king in (or next to) the promotion corner
    // can never be driven out, whatever the attacker does.
    if ((pf == 0 || pf == 7) && CHEB(dk, promo) <= 1) return 1;

    // (2) Defending king squarely in front of the pawn, kings in direct
    // opposition, attacker to move: the attacker must give way, so the defender
    // holds. This is exactly the measured failure above.
    if (dkf == pf && (dkr - pr) * fwd > 0) {             // in front of the pawn
        if (akf == pf && (akr - pr) * fwd > 0            // attacker also ahead
            && (dkr - akr) * fwd == 2                    // direct opposition
            && bd->side == side)                         // attacker must move
            return 1;
    }
    return 0;
}

// The KPK bitbase is OFF by default. Two honest reasons:
//   1. I could not verify its correctness. It disagreed with my reading of
//      textbook positions and I went back and forth on which of us was right —
//      which is itself reason enough not to ship it deciding evaluations. A
//      false draw throws away a won game.
//   2. It measured neutral anyway: recognizers off vs on scored 50.4% over 128
//      games (LOS 54%). Bare K+P vs K is simply too rare in real games to move
//      the needle — the same rare-firing blindness that hid trap_w from the
//      tuner.
// CHESS_KPK=1 enables it for anyone who wants to verify it against a real
// tablebase first.
int kpk_bitbase_on = 0;

// 1 if the position is a certain draw; the caller scores it 0.
int eval_endgame_draw(const Board *bd) {
    if (insufficient_material(bd)) return 1;
    if (kpk_bitbase_on && kpk_bitbase_draw(bd)) return 1;
    if (wrong_bishop_rook_pawn(bd, WHITE)) return 1;
    if (wrong_bishop_rook_pawn(bd, BLACK)) return 1;
    return 0;
}