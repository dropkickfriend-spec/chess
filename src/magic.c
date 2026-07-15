// magic.c — magic number search + attack tables
// Idea: for each square, hash the (masked) occupancy with a magic multiply
// so every possible blocker arrangement maps to a unique table slot.
#include <string.h>
#include "magic.h"
#include "attacks.h"

static U64 rook_magics[64], bishop_magics[64];
static U64 rook_masks_t[64], bishop_masks_t[64];
static int rook_bits[64], bishop_bits[64];         // relevant bit counts
static U64 rook_table[64][4096];                   // 2^12 max for rooks
static U64 bishop_table[64][512];                  // 2^9 max for bishops

// xorshift64 PRNG — deterministic, no libc rand nonsense
static U64 rng_state = 0x9E3779B97F4A7C15ULL;
static U64 rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
// Magic candidates want few set bits
static U64 rng_sparse(void) { return rng() & rng() & rng(); }

// Enumerate the idx-th blocker subset of a mask (Carry-Rippler-ish by index)
static U64 occupancy_variant(int idx, int bits, U64 mask) {
    U64 occ = 0;
    for (int i = 0; i < bits; i++) {
        int sq = LSB(mask);
        POP_BIT(mask, sq);
        if (idx & (1 << i)) SET_BIT(occ, sq);
    }
    return occ;
}

// Brute-force a magic for one square: try sparse randoms until no collisions
static U64 find_magic(int sq, int is_bishop) {
    U64 mask = is_bishop ? bishop_mask(sq) : rook_mask(sq);
    int bits = COUNT(mask);
    int n = 1 << bits;

    U64 occs[4096], atts[4096], used[4096];
    for (int i = 0; i < n; i++) {
        occs[i] = occupancy_variant(i, bits, mask);
        atts[i] = is_bishop ? bishop_attacks_slow(sq, occs[i])
                            : rook_attacks_slow(sq, occs[i]);
    }

    for (int tries = 0; tries < 100000000; tries++) {
        U64 magic = rng_sparse();
        // Heuristic: top byte of hashed mask needs enough bits
        if (COUNT((mask * magic) & 0xFF00000000000000ULL) < 6) continue;

        memset(used, 0, n * sizeof(U64));
        int fail = 0;
        for (int i = 0; i < n && !fail; i++) {
            int idx = (int)((occs[i] * magic) >> (64 - bits));
            if (used[idx] == 0) used[idx] = atts[i];
            else if (used[idx] != atts[i]) fail = 1;  // destructive collision
        }
        if (!fail) return magic;
    }
    return 0; // should never happen
}

void init_slider_attacks(void) {
    for (int sq = 0; sq < 64; sq++) {
        // Rooks
        rook_masks_t[sq] = rook_mask(sq);
        rook_bits[sq] = COUNT(rook_masks_t[sq]);
        rook_magics[sq] = find_magic(sq, 0);
        int n = 1 << rook_bits[sq];
        for (int i = 0; i < n; i++) {
            U64 occ = occupancy_variant(i, rook_bits[sq], rook_masks_t[sq]);
            int idx = (int)((occ * rook_magics[sq]) >> (64 - rook_bits[sq]));
            rook_table[sq][idx] = rook_attacks_slow(sq, occ);
        }
        // Bishops
        bishop_masks_t[sq] = bishop_mask(sq);
        bishop_bits[sq] = COUNT(bishop_masks_t[sq]);
        bishop_magics[sq] = find_magic(sq, 1);
        n = 1 << bishop_bits[sq];
        for (int i = 0; i < n; i++) {
            U64 occ = occupancy_variant(i, bishop_bits[sq], bishop_masks_t[sq]);
            int idx = (int)((occ * bishop_magics[sq]) >> (64 - bishop_bits[sq]));
            bishop_table[sq][idx] = bishop_attacks_slow(sq, occ);
        }
    }
}

U64 get_rook_attacks(int sq, U64 occ) {
    occ &= rook_masks_t[sq];
    occ *= rook_magics[sq];
    occ >>= 64 - rook_bits[sq];
    return rook_table[sq][occ];
}

U64 get_bishop_attacks(int sq, U64 occ) {
    occ &= bishop_masks_t[sq];
    occ *= bishop_magics[sq];
    occ >>= 64 - bishop_bits[sq];
    return bishop_table[sq][occ];
}

U64 get_queen_attacks(int sq, U64 occ) {
    return get_rook_attacks(sq, occ) | get_bishop_attacks(sq, occ);
}
