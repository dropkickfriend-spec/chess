# chess-bb

A bitboard chess engine in C: magic-bitboard move generation, alpha-beta
search, UCI protocol. Verified with perft; plays matches against Stockfish.

## Build & run

```sh
make            # builds ./chess
./chess test    # perft self-test (startpos + Kiwipete, all known node counts)
./chess perft 5 # perft divide from startpos (or pass a FEN)
./chess         # UCI mode (default) — point any chess GUI at it
```

## Play it against Stockfish

```sh
pip install chess                       # python-chess
python3 tools/match.py --games 10 --movetime 0.1 --skill 0
python3 tools/match.py --games 2  --movetime 0.1          # full strength
```

Results at 100 ms/move vs Stockfish 16 (engine commit `4d367d9`):

| Opponent            | W  | D | L | Score |
|---------------------|----|---|---|-------|
| Stockfish skill 0   | 9  | 1 | 0 | 95%   |
| Stockfish skill 3   | 4  | 4 | 2 | 60%   |
| Stockfish skill 5   | 2  | 3 | 5 | 35%   |
| Stockfish full      | 0  | 0 | 2 | 0%    |

Full PGNs in `matches/`.

## Supabase match history

Match results are stored in Supabase (`chessbb_matches` + `chessbb_games`,
public read, service-role write). Upload a new match with:

```sh
export SUPABASE_URL=https://<project>.supabase.co
export SUPABASE_KEY=<service role key>
python3 tools/match.py --games 10 --movetime 0.1 --skill 3 --upload
```

## Architecture

| File            | Part | What it does |
|-----------------|------|--------------|
| `src/board.*`   | 1    | Bitboard position, FEN parsing, ASCII display |
| `src/attacks.*` | 2/3a | Pawn/knight/king attack tables; slider masks + slow ray attacks |
| `src/magic.*`   | 3b   | Magic bitboards: per-square perfect-hash slider lookup tables |
| `src/movegen.*` | 4    | Move encoding, pseudo-legal generation, square-attacked test |
| `src/move.*`    | 5    | Make/unmake with undo, legal move filtering |
| `src/perft.*`   | 6    | Node-count correctness tests + divide |
| `src/main.c`    | 7    | CLI driver (uci / test / perft) |
| `src/eval.*`    | 8    | Material + piece-square tables (separate endgame king PST) |
| `src/search.*`  | 9    | Iterative deepening alpha-beta, quiescence, MVV-LVA + killers, check extension, repetition/50-move draws |
| `src/uci.c`     | 10   | UCI protocol |
