// movegen.c — pseudo-legal move generation
#include <stdio.h>
#include "movegen.h"
#include "attacks.h"
#include "magic.h"

static void add(MoveList *ml, int m) { ml->moves[ml->count++] = m; }

// Is sq attacked by by_side? Uses symmetry: e.g. a white pawn on sq's
// attack squares means a black-perspective lookup catches it.
int is_square_attacked(const Board *bd, int sq, int by_side) {
    int P = by_side == WHITE ? WP : BP;
    int N = by_side == WHITE ? WN : BN;
    int B = by_side == WHITE ? WB : BB_;
    int R = by_side == WHITE ? WR : BR;
    int Q = by_side == WHITE ? WQ : BQ;
    int K = by_side == WHITE ? WK : BK;

    if (pawn_attacks[!by_side][sq] & bd->bb[P]) return 1;
    if (knight_attacks[sq] & bd->bb[N]) return 1;
    if (king_attacks[sq] & bd->bb[K]) return 1;
    U64 occ = bd->occ[BOTH];
    if (get_bishop_attacks(sq, occ) & (bd->bb[B] | bd->bb[Q])) return 1;
    if (get_rook_attacks(sq, occ)   & (bd->bb[R] | bd->bb[Q])) return 1;
    return 0;
}

void generate_moves(const Board *bd, MoveList *ml) {
    ml->count = 0;
    int us = bd->side, them = !us;
    U64 our = bd->occ[us], their = bd->occ[them], occ = bd->occ[BOTH];

    // ---- Pawns ----
    int P = us == WHITE ? WP : BP;
    int push = us == WHITE ? 8 : -8;
    int start_rank = us == WHITE ? 1 : 6;
    int promo_rank = us == WHITE ? 6 : 1;
    int promos[4] = { us==WHITE?WQ:BQ, us==WHITE?WR:BR, us==WHITE?WB:BB_, us==WHITE?WN:BN };

    U64 pawns = bd->bb[P];
    while (pawns) {
        int from = LSB(pawns); POP_BIT(pawns, from);
        int rank = from / 8;
        int to = from + push;

        // Quiet push
        if (to >= 0 && to < 64 && !GET_BIT(occ, to)) {
            if (rank == promo_rank)
                for (int i = 0; i < 4; i++) add(ml, ENCODE(from,to,P,promos[i],0,0,0,0));
            else {
                add(ml, ENCODE(from,to,P,0,0,0,0,0));
                // Double push
                int to2 = to + push;
                if (rank == start_rank && !GET_BIT(occ, to2))
                    add(ml, ENCODE(from,to2,P,0,0,1,0,0));
            }
        }
        // Captures
        U64 caps = pawn_attacks[us][from] & their;
        while (caps) {
            int t = LSB(caps); POP_BIT(caps, t);
            if (rank == promo_rank)
                for (int i = 0; i < 4; i++) add(ml, ENCODE(from,t,P,promos[i],1,0,0,0));
            else add(ml, ENCODE(from,t,P,0,1,0,0,0));
        }
        // En passant
        if (bd->ep != NO_SQ && (pawn_attacks[us][from] & (1ULL << bd->ep)))
            add(ml, ENCODE(from,bd->ep,P,0,1,0,1,0));
    }

    // ---- Knights ----
    int N = us == WHITE ? WN : BN;
    U64 b = bd->bb[N];
    while (b) {
        int from = LSB(b); POP_BIT(b, from);
        U64 att = knight_attacks[from] & ~our;
        while (att) {
            int t = LSB(att); POP_BIT(att, t);
            add(ml, ENCODE(from,t,N,0, GET_BIT(their,t)?1:0, 0,0,0));
        }
    }

    // ---- Bishops / Rooks / Queens ----
    int B_ = us == WHITE ? WB : BB_;
    b = bd->bb[B_];
    while (b) {
        int from = LSB(b); POP_BIT(b, from);
        U64 att = get_bishop_attacks(from, occ) & ~our;
        while (att) { int t = LSB(att); POP_BIT(att, t);
            add(ml, ENCODE(from,t,B_,0, GET_BIT(their,t)?1:0, 0,0,0)); }
    }
    int R = us == WHITE ? WR : BR;
    b = bd->bb[R];
    while (b) {
        int from = LSB(b); POP_BIT(b, from);
        U64 att = get_rook_attacks(from, occ) & ~our;
        while (att) { int t = LSB(att); POP_BIT(att, t);
            add(ml, ENCODE(from,t,R,0, GET_BIT(their,t)?1:0, 0,0,0)); }
    }
    int Q = us == WHITE ? WQ : BQ;
    b = bd->bb[Q];
    while (b) {
        int from = LSB(b); POP_BIT(b, from);
        U64 att = get_queen_attacks(from, occ) & ~our;
        while (att) { int t = LSB(att); POP_BIT(att, t);
            add(ml, ENCODE(from,t,Q,0, GET_BIT(their,t)?1:0, 0,0,0)); }
    }

    // ---- King ----
    int K = us == WHITE ? WK : BK;
    b = bd->bb[K];
    if (b) {
        int from = LSB(b);
        U64 att = king_attacks[from] & ~our;
        while (att) { int t = LSB(att); POP_BIT(att, t);
            add(ml, ENCODE(from,t,K,0, GET_BIT(their,t)?1:0, 0,0,0)); }

        // Castling: squares empty, king not in / passing through check
        if (us == WHITE) {
            if ((bd->castle & WK_CASTLE) && !GET_BIT(occ,F1) && !GET_BIT(occ,G1)
                && !is_square_attacked(bd,E1,BLACK) && !is_square_attacked(bd,F1,BLACK))
                add(ml, ENCODE(E1,G1,K,0,0,0,0,1));
            if ((bd->castle & WQ_CASTLE) && !GET_BIT(occ,D1) && !GET_BIT(occ,C1) && !GET_BIT(occ,B1)
                && !is_square_attacked(bd,E1,BLACK) && !is_square_attacked(bd,D1,BLACK))
                add(ml, ENCODE(E1,C1,K,0,0,0,0,1));
        } else {
            if ((bd->castle & BK_CASTLE) && !GET_BIT(occ,F8) && !GET_BIT(occ,G8)
                && !is_square_attacked(bd,E8,WHITE) && !is_square_attacked(bd,F8,WHITE))
                add(ml, ENCODE(E8,G8,K,0,0,0,0,1));
            if ((bd->castle & BQ_CASTLE) && !GET_BIT(occ,D8) && !GET_BIT(occ,C8) && !GET_BIT(occ,B8)
                && !is_square_attacked(bd,E8,WHITE) && !is_square_attacked(bd,D8,WHITE))
                add(ml, ENCODE(E8,C8,K,0,0,0,0,1));
        }
    }
}

void move_to_str(int move, char *buf) {
    static const char promo_chars[] = " nbrq";
    int from = M_FROM(move), to = M_TO(move), promo = M_PROMO(move);
    buf[0] = 'a' + from % 8; buf[1] = '1' + from / 8;
    buf[2] = 'a' + to % 8;   buf[3] = '1' + to / 8;
    if (promo) {
        int base = promo >= BP ? promo - BP : promo;  // normalise to white index
        buf[4] = promo_chars[base]; buf[5] = 0;
    } else buf[4] = 0;
}
