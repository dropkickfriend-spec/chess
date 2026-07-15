// perft.c — Part 6: node-count move generator tests
// Walking the legal move tree to a fixed depth and counting leaves is the
// standard way to catch move generator bugs: any wrong/missing/extra move
// shows up as a node-count mismatch against known-good values.
#include <stdio.h>
#include "perft.h"
#include "move.h"

long long perft(Board *bd, int depth) {
    if (depth == 0) return 1;

    MoveList ml;
    generate_legal_moves(bd, &ml);
    if (depth == 1) return ml.count;

    long long nodes = 0;
    for (int i = 0; i < ml.count; i++) {
        Undo undo;
        make_move(bd, ml.moves[i], &undo);
        nodes += perft(bd, depth - 1);
        unmake_move(bd, ml.moves[i], &undo);
    }
    return nodes;
}

// Per-root-move breakdown, handy for bisecting against a reference engine.
void perft_divide(Board *bd, int depth) {
    MoveList ml;
    generate_legal_moves(bd, &ml);

    long long total = 0;
    for (int i = 0; i < ml.count; i++) {
        char buf[6];
        move_to_str(ml.moves[i], buf);

        Undo undo;
        make_move(bd, ml.moves[i], &undo);
        long long nodes = depth > 1 ? perft(bd, depth - 1) : 1;
        unmake_move(bd, ml.moves[i], &undo);

        printf("%s: %lld\n", buf, nodes);
        total += nodes;
    }
    printf("total: %lld\n", total);
}
