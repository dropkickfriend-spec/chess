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

Results at 100 ms/move vs Stockfish 16:

| Opponent            | v1 `4d367d9` | v2 `b214d01` (TT + null-move + LMR) |
|---------------------|--------------|--------------------------------------|
| Stockfish skill 0   | 95%          | —                                    |
| Stockfish skill 3   | 60%          | **75%** (+7 =1 -2)                   |
| Stockfish skill 5   | 35%          | **55%** (+3 =5 -2)                   |
| Stockfish skill 7   | —            | 30% (+1 =4 -5)                       |
| Stockfish full      | 0%           | 0%                                   |

Full PGNs in `matches/` (v2 games prefixed `v2_`).

## Supabase match history

Match results are stored in Supabase (`chessbb_matches` + `chessbb_games`).
Credentials live in `supabase.env` (anon key — it can only append match rows
and read public data; the service role key is never committed). Upload a new
match with:

```sh
python3 tools/match.py --games 10 --movetime 0.1 --skill 3 --upload
```

`SUPABASE_URL`/`SUPABASE_KEY` env vars override the file if set.

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
