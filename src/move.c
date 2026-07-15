// move.c — Part 5: make/unmake + legal move filtering
#include "move.h"

// Squares that, once touched (as either the from- or to-square of a move),
// permanently strip the corresponding castling right — covers king moves,
// rook moves, and rooks getting captured on their home square.
static int castle_mask(int sq) {
    switch (sq) {
        case A1: return ~WQ_CASTLE;
        case E1: return ~(WK_CASTLE | WQ_CASTLE);
        case H1: return ~WK_CASTLE;
        case A8: return ~BQ_CASTLE;
        case E8: return ~(BK_CASTLE | BQ_CASTLE);
        case H8: return ~BK_CASTLE;
        default: return ~0;
    }
}

void make_move(Board *bd, int move, Undo *undo) {
    int from = M_FROM(move), to = M_TO(move);
    int piece = M_PIECE(move), promo = M_PROMO(move);
    int us = bd->side, them = !us;

    undo->ep = bd->ep;
    undo->castle = bd->castle;
    undo->halfmove = bd->halfmove;
    undo->fullmove = bd->fullmove;
    undo->captured = -1;

    POP_BIT(bd->bb[piece], from);

    if (M_EP(move)) {
        int cap_sq = us == WHITE ? to - 8 : to + 8;
        int their_pawn = us == WHITE ? BP : WP;
        undo->captured = their_pawn;
        POP_BIT(bd->bb[their_pawn], cap_sq);
    } else if (M_CAP(move)) {
        int lo = them == WHITE ? WP : BP, hi = them == WHITE ? WK : BK;
        for (int p = lo; p <= hi; p++)
            if (GET_BIT(bd->bb[p], to)) { undo->captured = p; POP_BIT(bd->bb[p], to); break; }
    }

    int placed = promo ? promo : piece;
    SET_BIT(bd->bb[placed], to);

    if (M_CASTLE(move)) {
        if (to == G1)      { POP_BIT(bd->bb[WR], H1); SET_BIT(bd->bb[WR], F1); }
        else if (to == C1) { POP_BIT(bd->bb[WR], A1); SET_BIT(bd->bb[WR], D1); }
        else if (to == G8) { POP_BIT(bd->bb[BR], H8); SET_BIT(bd->bb[BR], F8); }
        else if (to == C8) { POP_BIT(bd->bb[BR], A8); SET_BIT(bd->bb[BR], D8); }
    }

    bd->castle &= castle_mask(from) & castle_mask(to);
    bd->ep = M_DBL(move) ? (from + to) / 2 : NO_SQ;
    bd->halfmove = (piece == WP || piece == BP || M_CAP(move)) ? 0 : bd->halfmove + 1;
    if (us == BLACK) bd->fullmove++;

    bd->side = them;
    update_occ(bd);
}

void unmake_move(Board *bd, int move, const Undo *undo) {
    int from = M_FROM(move), to = M_TO(move);
    int piece = M_PIECE(move), promo = M_PROMO(move);
    int us = piece <= WK ? WHITE : BLACK;

    bd->side = us;
    bd->ep = undo->ep;
    bd->castle = undo->castle;
    bd->halfmove = undo->halfmove;
    bd->fullmove = undo->fullmove;

    int placed = promo ? promo : piece;
    POP_BIT(bd->bb[placed], to);
    SET_BIT(bd->bb[piece], from);

    if (M_CASTLE(move)) {
        if (to == G1)      { POP_BIT(bd->bb[WR], F1); SET_BIT(bd->bb[WR], H1); }
        else if (to == C1) { POP_BIT(bd->bb[WR], D1); SET_BIT(bd->bb[WR], A1); }
        else if (to == G8) { POP_BIT(bd->bb[BR], F8); SET_BIT(bd->bb[BR], H8); }
        else if (to == C8) { POP_BIT(bd->bb[BR], D8); SET_BIT(bd->bb[BR], A8); }
    }

    if (M_EP(move)) {
        int cap_sq = us == WHITE ? to - 8 : to + 8;
        SET_BIT(bd->bb[undo->captured], cap_sq);
    } else if (undo->captured != -1) {
        SET_BIT(bd->bb[undo->captured], to);
    }

    update_occ(bd);
}

void generate_legal_moves(const Board *bd, MoveList *ml) {
    MoveList pseudo;
    generate_moves(bd, &pseudo);
    ml->count = 0;

    int K = bd->side == WHITE ? WK : BK;
    Board work = *bd;
    for (int i = 0; i < pseudo.count; i++) {
        int move = pseudo.moves[i];
        Undo undo;
        make_move(&work, move, &undo);
        int king_sq = LSB(work.bb[K]);
        if (!is_square_attacked(&work, king_sq, work.side))
            ml->moves[ml->count++] = move;
        unmake_move(&work, move, &undo);
    }
}
