// board.c — FEN parsing, occupancy, ASCII display
#include <stdio.h>
#include <string.h>
#include "board.h"

static const char piece_chars[12] = {'P','N','B','R','Q','K','p','n','b','r','q','k'};

static int char_to_piece(char c) {
    for (int i = 0; i < 12; i++) if (piece_chars[i] == c) return i;
    return -1;
}

void update_occ(Board *bd) {
    bd->occ[WHITE] = bd->occ[BLACK] = 0;
    for (int p = WP; p <= WK; p++) bd->occ[WHITE] |= bd->bb[p];
    for (int p = BP; p <= BK; p++) bd->occ[BLACK] |= bd->bb[p];
    bd->occ[BOTH] = bd->occ[WHITE] | bd->occ[BLACK];
}

void board_from_fen(Board *bd, const char *fen) {
    memset(bd, 0, sizeof(Board));
    bd->ep = NO_SQ;

    // 1. Piece placement (FEN starts at rank 8)
    int rank = 7, file = 0;
    while (*fen && *fen != ' ') {
        char c = *fen++;
        if (c == '/') { rank--; file = 0; }
        else if (c >= '1' && c <= '8') file += c - '0';
        else {
            int p = char_to_piece(c);
            if (p >= 0) SET_BIT(bd->bb[p], rank * 8 + file);
            file++;
        }
    }
    fen++; // skip space

    // 2. Side to move
    bd->side = (*fen == 'w') ? WHITE : BLACK;
    fen += 2;

    // 3. Castling rights
    while (*fen && *fen != ' ') {
        switch (*fen++) {
            case 'K': bd->castle |= WK_CASTLE; break;
            case 'Q': bd->castle |= WQ_CASTLE; break;
            case 'k': bd->castle |= BK_CASTLE; break;
            case 'q': bd->castle |= BQ_CASTLE; break;
        }
    }
    fen++;

    // 4. En passant
    if (*fen != '-') {
        int f = fen[0] - 'a', r = fen[1] - '1';
        bd->ep = r * 8 + f;
        fen += 2;
    } else fen++;

    // 5–6. Clocks (optional in some FENs)
    if (*fen == ' ') sscanf(fen, " %d %d", &bd->halfmove, &bd->fullmove);

    update_occ(bd);
}

void board_print(const Board *bd) {
    for (int rank = 7; rank >= 0; rank--) {
        printf(" %d ", rank + 1);
        for (int file = 0; file < 8; file++) {
            int sq = rank * 8 + file;
            char c = '.';
            for (int p = 0; p < 12; p++)
                if (GET_BIT(bd->bb[p], sq)) { c = piece_chars[p]; break; }
            printf(" %c", c);
        }
        printf("\n");
    }
    printf("\n    a b c d e f g h\n\n");
    printf(" side: %s  castle: %c%c%c%c  ep: %d  half: %d\n",
        bd->side == WHITE ? "white" : "black",
        bd->castle & WK_CASTLE ? 'K' : '-',
        bd->castle & WQ_CASTLE ? 'Q' : '-',
        bd->castle & BK_CASTLE ? 'k' : '-',
        bd->castle & BQ_CASTLE ? 'q' : '-',
        bd->ep, bd->halfmove);
}
