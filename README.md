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

| Opponent          | v1 `4d367d9` | v2 `b214d01` (search) | v3 `eb9e7a2` (eval) |
|-------------------|--------------|------------------------|----------------------|
| Stockfish skill 0 | 95%          | —                      | —                    |
| Stockfish skill 3 | 60%          | **75%**                | —                    |
| Stockfish skill 5 | 35%          | **55%**                | **60%** (+5 =2 -3)   |
| Stockfish skill 7 | —            | 30%                    | **50%** (+2 =6 -2)   |
| Stockfish skill 10| —            | —                      | 5% (+0 =1 -9)        |
| Stockfish full    | 0%           | 0%                     | 25% (=1!)            |

v3 took a draw off full-strength Stockfish as Black — Stockfish forced the
perpetual itself at move 32 (`matches/v3_full.pgn`, round 2).

Later versions: v4 (`db70a94`, SEE + futility + king attack) and v5
(`aabaab0`, rule-of-the-square + king-passer proximity) measured equal to
each other in 100-game self-play (48% ± 8) and equal to v3 within the noise
of 10-game Stockfish rungs. Lesson encoded in the workflow: 10-game matches
can't resolve <100 Elo — gate changes on 100+ game self-play instead.

Full PGNs in `matches/` (per-version prefixes).

## Supabase match history

Match results are stored in Supabase (`chessbb_matches` + `chessbb_games`).
Credentials live in `supabase.env` (anon key — it can only append match rows
and read public data; the service role key is never committed). Upload a new
match with:

```sh
python3 tools/match.py --games 10 --movetime 0.1 --skill 3 --upload
```

`SUPABASE_URL`/`SUPABASE_KEY` env vars override the file if set.

## Playing on Lichess

`tools/lichess_bot.py` bridges the engine to the Lichess Bot API: it accepts
standard-chess challenges, plays with the engine's own clock management, and
logs every finished game to Supabase like the match runner does.

One-time setup (the account is permanently marked as a bot):

1. Create a **new** lichess account (it must have zero games played).
2. Create a token with the `bot:play` scope:
   <https://lichess.org/account/oauth/token/create?scopes[]=bot:play>
3. Upgrade the account:
   `curl -d '' https://lichess.org/api/bot/account/upgrade -H "Authorization: Bearer <token>"`

Then run: `LICHESS_TOKEN=<token> python3 tools/lichess_bot.py`

## Running on a phone (Termux)

The engine is plain C11 and builds unchanged under Termux:

```sh
pkg install git && git clone <this repo> && cd chess
bash tools/termux-setup.sh
```

That installs clang/make/python/stockfish, builds, self-tests, and leaves you
able to run Stockfish matches (`--upload` logs them to Supabase) or the
lichess bot straight from the phone. Use `termux-wake-lock` to keep long
matches alive with the screen off.

## How "learning" works here

The engine itself is a static hand-crafted evaluation — playing games does
not change it by itself. The learning loop is:

1. Every game (match runner, lichess) is logged to Supabase with its PGN.
2. Periodically, the logged positions + game results become training data
   for **Texel tuning**: fit all eval weights (PSTs, pawn terms, king
   safety...) so the eval best predicts actual game outcomes.
3. The tuned weights become the next engine version, gated by a 100+ game
   self-play match against the previous version.

More games logged = better tuning data, so leaving the phone farming
Stockfish games genuinely feeds the pipeline.

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
