// magic.c — magic number search + attack tables
// Idea: for each square, hash the (masked) occupancy with a magic multiply
// so every possible blocker arrangement maps to a unique table slot.
#include <string.h>
#include "magic.h"
#include "attacks.h"

static U64 rook_magics[64], bishop_magics[64];

// Precomputed magics: the search below is deterministic (fixed-seed xorshift),
// so it found these exact numbers on every startup -- ~377 ms of brute force
// per launch to rediscover constants. They are used directly and VERIFIED while
// the attack tables are filled; if one ever fails (a changed mask or a different
// build), that square silently falls back to searching for a fresh magic, so
// correctness never depends on this table being right.
static const U64 ROOK_MAGIC_INIT[64] = {
    0x2080002080400010ULL,    0x00c0002001401000ULL,
    0x2100110008402002ULL,    0x0880080081041000ULL,
    0x0200020020041008ULL,    0x2300040008010012ULL,
    0x0c00283004008201ULL,    0x0180010000407a80ULL,
    0x0168800080400020ULL,    0x0010400040201000ULL,
    0x1001002001001048ULL,    0x1001002408100100ULL,
    0x0801000408010012ULL,    0x4001000209000400ULL,
    0x08a20004c8020001ULL,    0x2002801145002280ULL,
    0x0080860021004200ULL,    0x001000c009402002ULL,
    0x00b0002004002800ULL,    0x100a808010020800ULL,
    0x9400808004000800ULL,    0x0090808004000200ULL,
    0x0000040010810208ULL,    0x2000020000448534ULL,
    0x4104400480008033ULL,    0x0000810100204000ULL,
    0x0440430900200010ULL,    0x4600240900100100ULL,
    0x0804080100110004ULL,    0x0001000300080400ULL,
    0x0004084400011002ULL,    0x0023040200008041ULL,
    0x0580050043002080ULL,    0x0400804002802008ULL,
    0x0001002001004010ULL,    0x0080200a02001040ULL,
    0x600d480280802400ULL,    0x400b800201800c00ULL,
    0x2408211004004208ULL,    0x0200211082000844ULL,
    0x0020804010208000ULL,    0x5030004020104000ULL,
    0xa042084080220010ULL,    0x4088080010008080ULL,
    0x5002080100110004ULL,    0x2012002010040400ULL,
    0x0040318210440008ULL,    0x0120941040820001ULL,
    0x1000800100402100ULL,    0x0040002010004840ULL,
    0x8108450020001900ULL,    0x0200204008120200ULL,
    0x0080800c00180180ULL,    0x0885000400420900ULL,
    0x230802011008c400ULL,    0x3801740891432200ULL,
    0x0a00250212024082ULL,    0x0000882040001105ULL,
    0x0042102082000a42ULL,    0xc401210810000501ULL,
    0x0241001002480005ULL,    0x0081000400880241ULL,
    0x0000009008024124ULL,    0x0048122980410402ULL,
};
static const U64 BISHOP_MAGIC_INIT[64] = {
    0x1862221006220044ULL,    0x2104a14202020060ULL,
    0x2804081220444001ULL,    0x2102408900010001ULL,
    0x0002021000040002ULL,    0x08c3100805004300ULL,
    0x1084040124920050ULL,    0x8900440043382010ULL,
    0x2401410802140040ULL,    0x0901200454208020ULL,
    0x0000090216020541ULL,    0x1283844040800804ULL,
    0x0521840420000803ULL,    0x0800010402400c40ULL,
    0x0000408e10100404ULL,    0x0009810048420800ULL,
    0x2004211004286808ULL,    0x13080a1001380080ULL,
    0x0008801004220020ULL,    0x0024000802480800ULL,
    0x1461001190400401ULL,    0x0020400200500440ULL,
    0x0003000409019000ULL,    0x000c20820d011802ULL,
    0x000804002164100cULL,    0x00048400a0011404ULL,
    0x5018110308044100ULL,    0x0048a00804010020ULL,
    0x0007840000802000ULL,    0x8808a20075004220ULL,
    0x8014040000822100ULL,    0x110c03000e251101ULL,
    0x0081094820202010ULL,    0x0008041000044100ULL,
    0x00c1202808940800ULL,    0x8108100821040400ULL,
    0x1240010010010041ULL,    0x0810004080011000ULL,
    0x00a20c0401804a00ULL,    0x40014c0020050500ULL,
    0x5805082012042480ULL,    0x2004022144031000ULL,
    0x2082002024204808ULL,    0x0800004200800800ULL,
    0x0410020204100a02ULL,    0x80c1204080804101ULL,
    0x0010104e01800042ULL,    0x000800810c400208ULL,
    0x100080b008201210ULL,    0x8000440605112101ULL,
    0x000202008c440040ULL,    0x9004002210442200ULL,
    0x2032014088222045ULL,    0x0c00202222c20000ULL,
    0x0140040820a50100ULL,    0x0222104c29024018ULL,
    0x0200110121202004ULL,    0x0800104200b00802ULL,
    0x0000401424020801ULL,    0x4000000004208840ULL,
    0x0802e00040104100ULL,    0x03000020a0424080ULL,
    0x0011c00408188121ULL,    0x0848020822040013ULL,
};

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

// Fill one square's attack table with a candidate magic. Returns 0 if the magic
// is unusable (two occupancies with different attack sets collide in one slot).
static int fill_table(int sq, int is_bishop, U64 magic) {
    if (!magic) return 0;
    U64 mask = is_bishop ? bishop_masks_t[sq] : rook_masks_t[sq];
    int bits = is_bishop ? bishop_bits[sq] : rook_bits[sq];
    int n = 1 << bits;
    U64 *tab = is_bishop ? bishop_table[sq] : rook_table[sq];
    memset(tab, 0, (size_t)n * sizeof(U64));
    static char seen[4096];
    memset(seen, 0, (size_t)n);
    for (int i = 0; i < n; i++) {
        U64 occ = occupancy_variant(i, bits, mask);
        U64 att = is_bishop ? bishop_attacks_slow(sq, occ)
                            : rook_attacks_slow(sq, occ);
        int idx = (int)((occ * magic) >> (64 - bits));
        if (!seen[idx]) { seen[idx] = 1; tab[idx] = att; }
        else if (tab[idx] != att) return 0;   // destructive collision
    }
    return 1;
}

void init_slider_attacks(void) {
    for (int sq = 0; sq < 64; sq++) {
        rook_masks_t[sq] = rook_mask(sq);
        rook_bits[sq] = COUNT(rook_masks_t[sq]);
        rook_magics[sq] = ROOK_MAGIC_INIT[sq];
        if (!fill_table(sq, 0, rook_magics[sq])) {          // fallback: search
            rook_magics[sq] = find_magic(sq, 0);
            fill_table(sq, 0, rook_magics[sq]);
        }

        bishop_masks_t[sq] = bishop_mask(sq);
        bishop_bits[sq] = COUNT(bishop_masks_t[sq]);
        bishop_magics[sq] = BISHOP_MAGIC_INIT[sq];
        if (!fill_table(sq, 1, bishop_magics[sq])) {
            bishop_magics[sq] = find_magic(sq, 1);
            fill_table(sq, 1, bishop_magics[sq]);
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
