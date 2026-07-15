// board.h — Part 1: bitboard representation
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

typedef uint64_t U64;

// Squares: a1=0 ... h8=63 (little-endian rank-file)
enum {
    A1,B1,C1,D1,E1,F1,G1,H1, A2,B2,C2,D2,E2,F2,G2,H2,
    A3,B3,C3,D3,E3,F3,G3,H3, A4,B4,C4,D4,E4,F4,G4,H4,
    A5,B5,C5,D5,E5,F5,G5,H5, A6,B6,C6,D6,E6,F6,G6,H6,
    A7,B7,C7,D7,E7,F7,G7,H7, A8,B8,C8,D8,E8,F8,G8,H8,
    NO_SQ = 64
};

enum { WHITE, BLACK, BOTH };

// Piece indices into bb[12]
enum { WP, WN, WB, WR, WQ, WK, BP, BN, BB_, BR, BQ, BK };

// Castling rights bits
enum { WK_CASTLE = 1, WQ_CASTLE = 2, BK_CASTLE = 4, BQ_CASTLE = 8 };

typedef struct {
    U64 bb[12];      // one bitboard per piece type
    U64 occ[3];      // white, black, both occupancy
    int side;        // side to move
    int ep;          // en passant square or NO_SQ
    int castle;      // castling rights bitmask
    int halfmove;    // 50-move clock
    int fullmove;
} Board;

// Bit ops
#define SET_BIT(b, sq)   ((b) |=  (1ULL << (sq)))
#define GET_BIT(b, sq)   ((b) &   (1ULL << (sq)))
#define POP_BIT(b, sq)   ((b) &= ~(1ULL << (sq)))
#define COUNT(b)         __builtin_popcountll(b)
#define LSB(b)           __builtin_ctzll(b)      // index of least significant set bit

void board_from_fen(Board *bd, const char *fen);
void board_print(const Board *bd);
void update_occ(Board *bd);

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

#endif
