// uci.c — minimal UCI: enough to play under any GUI / match runner.
// Supported: uci, isready, ucinewgame, position, go (movetime/wtime/btime/
// winc/binc/depth/infinite), quit. Search is synchronous — fine for an
// engine without pondering.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uci.h"
#include "board.h"
#include "movegen.h"
#include "move.h"
#include "search.h"

static Board pos;
static U64 game_hist[1024];
static int game_hist_len;

// Match a "e2e4"/"e7e8q" string against the legal moves of the position.
static int parse_move(const Board *bd, const char *str) {
    MoveList ml;
    generate_legal_moves(bd, &ml);
    for (int i = 0; i < ml.count; i++) {
        char buf[6];
        move_to_str(ml.moves[i], buf);
        if (strcmp(buf, str) == 0) return ml.moves[i];
    }
    return 0;
}

static void cmd_position(char *line) {
    char *ptr = line + 9;  // past "position "
    game_hist_len = 0;

    if (strncmp(ptr, "startpos", 8) == 0) {
        board_from_fen(&pos, START_FEN);
        ptr += 8;
    } else if (strncmp(ptr, "fen", 3) == 0) {
        ptr += 4;
        board_from_fen(&pos, ptr);
        char *m = strstr(ptr, "moves");
        ptr = m ? m : ptr + strlen(ptr);
    }

    game_hist[game_hist_len++] = board_hash(&pos);

    char *moves = strstr(ptr, "moves");
    if (moves) {
        moves += 6;
        char *tok = strtok(moves, " \n");
        while (tok) {
            int move = parse_move(&pos, tok);
            if (!move) break;  // illegal/garbage: stop applying
            Undo undo;
            make_move(&pos, move, &undo);
            if (game_hist_len < 1024)
                game_hist[game_hist_len++] = board_hash(&pos);
            tok = strtok(NULL, " \n");
        }
    }
}

static void cmd_go(char *line) {
    extern long long search_node_limit;   // search.c: 0 = unlimited
    int movetime = 0, depth = 0;
    int wtime = 0, btime = 0, winc = 0, binc = 0;
    long long node_limit = 0;

    char *tok = strtok(line + 3, " \n");
    while (tok) {
        if (strcmp(tok, "infinite") == 0) movetime = 3600000;
        else {
            char *val = strtok(NULL, " \n");
            if (!val) break;
            if      (strcmp(tok, "movetime") == 0) movetime = atoi(val);
            else if (strcmp(tok, "depth")    == 0) depth = atoi(val);
            else if (strcmp(tok, "nodes")    == 0) node_limit = atoll(val);
            else if (strcmp(tok, "wtime")    == 0) wtime = atoi(val);
            else if (strcmp(tok, "btime")    == 0) btime = atoi(val);
            else if (strcmp(tok, "winc")     == 0) winc = atoi(val);
            else if (strcmp(tok, "binc")     == 0) binc = atoi(val);
        }
        tok = strtok(NULL, " \n");
    }

    search_node_limit = node_limit;
    if (node_limit && !movetime) movetime = 3600000;   // nodes cap the search

    if (!movetime) {
        int mytime = pos.side == WHITE ? wtime : btime;
        int myinc  = pos.side == WHITE ? winc  : binc;
        if (mytime > 0) {
            movetime = mytime / 30 + myinc / 2;   // simple clock allocation
            if (movetime > mytime / 2) movetime = mytime / 2;
            if (movetime < 10) movetime = 10;
        } else movetime = depth ? 3600000 : 1000; // depth-limited or default 1s
    }

    search_set_history(game_hist, game_hist_len);
    int best = search_best_move(&pos, movetime, depth);

    char buf[6] = "0000";
    if (best) move_to_str(best, buf);
    printf("bestmove %s\n", buf);
    fflush(stdout);
}

void uci_loop(void) {
    setbuf(stdout, NULL);
    board_from_fen(&pos, START_FEN);
    game_hist[0] = board_hash(&pos);
    game_hist_len = 1;

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        if      (strncmp(line, "uci", 3) == 0 && line[3] != 'n') {
            printf("id name chess-bb\n");
            printf("id author dropkickfriend\n");
            printf("uciok\n");
        }
        else if (strncmp(line, "isready", 7) == 0)   printf("readyok\n");
        else if (strncmp(line, "ucinewgame", 10) == 0) {
            board_from_fen(&pos, START_FEN);
            game_hist[0] = board_hash(&pos);
            game_hist_len = 1;
        }
        else if (strncmp(line, "position", 8) == 0)  cmd_position(line);
        else if (strncmp(line, "go", 2) == 0)        cmd_go(line);
        else if (strncmp(line, "quit", 4) == 0)      break;
        else if (strncmp(line, "d", 1) == 0 && (line[1] == '\n' || line[1] == 0))
            board_print(&pos);  // debug convenience, like stockfish's "d"
        fflush(stdout);
    }
}
