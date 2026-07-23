// main.c — Part 7: driver + self-test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "attacks.h"
#include "magic.h"
#include "movegen.h"
#include "move.h"
#include "perft.h"
#include "eval.h"
#include "eval_strategy.h"
#include "search.h"
#include "tune.h"
#include "uci.h"

// Kiwipete: the standard second perft test position, exercises castling,
// en passant, and promotions much earlier than the start position does.
#define KIWIPETE "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"

static void self_test(void) {
    struct { const char *fen; int depth; long long expected; } cases[] = {
        { START_FEN, 1, 20 },
        { START_FEN, 2, 400 },
        { START_FEN, 3, 8902 },
        { START_FEN, 4, 197281 },
        { KIWIPETE,  1, 48 },
        { KIWIPETE,  2, 2039 },
        { KIWIPETE,  3, 97862 },
    };
    int n = sizeof(cases) / sizeof(cases[0]);
    int pass = 0;
    for (int i = 0; i < n; i++) {
        Board bd;
        board_from_fen(&bd, cases[i].fen);
        long long got = perft(&bd, cases[i].depth);
        int ok = got == cases[i].expected;
        pass += ok;
        printf("[%s] depth %d: got %lld, expected %lld\n",
               ok ? "PASS" : "FAIL", cases[i].depth, got, cases[i].expected);
    }
    printf("%d/%d perft self-tests passed\n", pass, n);
}

int main(int argc, char **argv) {
    init_stepper_attacks();
    init_slider_attacks();
    eval_init();

    // Texel-tuned square/piece values: relearned once from logged games,
    // then reloaded on every startup. CHESS_WEIGHTS overrides the default.
    const char *wf = getenv("CHESS_WEIGHTS");
    if (!wf || !*wf) wf = "learned_values.txt";
    {
        int n = eval_load_weights(wf);
        if (n) fprintf(stderr, "loaded %d weights from %s\n", n, wf);
    }

    // Route book: permanent cache of root-search verdicts. Enabled when the
    // file exists or CHESS_BOOK names one explicitly; silent otherwise.
    const char *bf = getenv("CHESS_BOOK");
    {
        int force = bf && *bf;
        if (!force) bf = "route_book.txt";
        int n = book_load(bf, force);
        if (n) fprintf(stderr, "loaded %d book routes from %s\n", n, bf);
    }

    if (argc >= 2 && strcmp(argv[1], "perft") == 0) {
        int depth = argc >= 3 ? atoi(argv[2]) : 5;
        const char *fen = argc >= 4 ? argv[3] : START_FEN;

        Board bd;
        board_from_fen(&bd, fen);
        board_print(&bd);

        clock_t start = clock();
        perft_divide(&bd, depth);
        double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
        printf("(%.2fs)\n", secs);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        self_test();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "tune") == 0) {
        tune_run(argv[2]);
        return 0;
    }

    // Batch eval explainer: FEN per stdin line -> per-strategy breakdown.
    // Output per line: "name raw eff_weight" x11 then "TOTAL <cp>".
    if (argc >= 2 && strcmp(argv[1], "evalfens") == 0) {
        StrategyWeights sw;
        eval_default_strategy_weights(&sw);
        eval_load_strategy_weights("strategy_weights.txt", &sw);
        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            Board bd;
            board_from_fen(&bd, line);
            int raw[STRAT_COUNT];
            float eff[STRAT_COUNT];
            int total = eval_explain(&bd, &sw, raw, eff);
            for (int i = 0; i < STRAT_COUNT; i++)
                printf("%s %d %.3f\n", strategy_names[i], raw[i], eff[i]);
            printf("TOTAL %d\n", total);
        }
        return 0;
    }

    // Opening-book generator: each stdin line is a UCI move sequence from the
    // start position (e.g. "e2e4 e7e5 g1f3 b8c6"). For every position along
    // the line we emit a trusted route-book entry recommending the next move,
    // keyed by the engine's own board_hash so search.c probes it exactly. The
    // output is appended/merged into the route book (dedup with sort -u).
    if (argc >= 2 && strcmp(argv[1], "mkbook") == 0) {
        extern U64 board_hash(const Board *bd);
        char line[2048];
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || line[0] == '#') continue;
            Board bd;
            board_from_fen(&bd, START_FEN);
            for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
                MoveList ml;
                generate_legal_moves(&bd, &ml);
                int mv = 0;
                for (int i = 0; i < ml.count; i++) {
                    char buf[6];
                    move_to_str(ml.moves[i], buf);
                    if (strcmp(buf, tok) == 0) { mv = ml.moves[i]; break; }
                }
                if (!mv) break;   // typo / illegal in this line: stop it here
                // depth 12, cert 100 => trusted for instant root play
                printf("%016llx 12 20 100 %s\n",
                       (unsigned long long)board_hash(&bd), tok);
                Undo u;
                make_move(&bd, mv, &u);
            }
        }
        return 0;
    }

    // Default: speak UCI on stdin/stdout (how GUIs and match runners drive us)
    uci_loop();
    return 0;
}
