# Scoping the endgame problem

## The measurement that decides everything

Same position, same material, only the side to move differs:

| `8/8/8/3k4/8/3K4/3P4/8` | truth | engine (depth 20) |
|---|---|---|
| White to move | **theoretical DRAW** (Black has the opposition) | **+371** |
| Black to move | **White WINS** | **+390** |

**The engine cannot distinguish a won K+P vs K from a drawn one.** 19 cp apart is
noise. It scores a dead draw as nearly four pawns up.

That is not a search failure. The search is doing its job — it is faithfully
propagating a leaf evaluation that is simply wrong.

## Why "more depth" cannot fix this

Depth is already good, and better in endgames than middlegames:

| position | depth in 1 s | effective branching factor |
|---|---|---|
| middlegame | 9 | 3.78 |
| R+B endgame | 13 | 2.78 |
| pawn endgame | **18** | **2.09** |
| minor-piece endgame | **18** | 2.12 |

An EBF of 2.09 is already better than sqrt(branching) for these positions — move
ordering and the TT are working well. There is no easy factor sitting there.

And the economics are brutal:

```
+1 ply           = 2.1x nodes
+4 ply           = 19x nodes
a 2x faster search buys 0.94 extra ply
proving a K+P vs K draw by search (~50 plies) = 1.7e10 x nodes
```

**A doubling of search speed buys one ply. The endgame needs roughly thirty.**
Search work cannot close that gap; only exact knowledge can.

## Tier 1 — endgame recognizers (recommended first)

Return an exact score, or scale the eval toward 0, for material configurations
whose result is a THEOREM:

- **Insufficient material**: KK, KNK, KBK, KNNK -> exact 0.
- **Rook pawn + wrong-coloured bishop**: defending king reaches the corner -> 0.
- **K+P vs K**: rule of the square plus opposition decides win/draw exactly.
- **KRK / KQK**: already handled by the bare-king mate drive.
- **Scale factors** for drawish material (opposite-coloured bishops, R+P vs R).

Implementation: a recognizer consulted in `evaluate()` before the strategy sum,
returning either an exact score or a multiplier applied to the result.

**Why this is different from the heuristics that failed this session.** The
commitment axis, the initiative race and the borrowed-horizon book all measured
neutral — they were *guesses about murky positions*. "KBK is a draw" is not a
guess, it is proven. These recognizers are exact where the failed ideas were
fuzzy, which is precisely why they are worth building despite that track record.

Cost: roughly 150 lines. Directly testable with `lab/ab_match.py`, and testable
in isolation on the four textbook positions above.

## Tier 2 — tablebases (exact and complete, bigger build)

Syzygy 3-4-5 piece tablebases solve **every** endgame up to 5 pieces perfectly,
which covers most of what we are losing. 4-piece is ~30 MB, 5-piece ~1 GB, and
it needs a probe reader.

**Question worth answering before building this:** the project rule is "no
Stockfish for evaluation" — the engine must learn its own judgement. Tablebases
are not another engine's opinion; they are mathematically proven results, closer
to a rule of chess than to borrowed evaluation. Whether that counts as cheating
is the user's call, not an assumption to make quietly.

## Tier 3 — search techniques ("nesting logic to increase depth")

These all work the same way: lower the effective branching factor so the same
node budget reaches deeper.

| technique | what it does | realistic gain |
|---|---|---|
| Internal Iterative Deepening | shallow search to get a TT move when none exists, improving ordering | ~5-10% nodes |
| Singular extensions | extend when one move is clearly forced/best | +strength, costs nodes |
| ProbCut / multi-cut | prove a beta cutoff cheaply at reduced depth | ~10% |
| Better TT replacement | endgames revisit positions constantly; depth-preferred or bucketed replacement | ~10% in endgames |
| Razoring | drop straight to quiescence when hopeless at low depth | ~5% |
| History / countermove tuning | better ordering = lower EBF | ~5-10% |

Worth doing for general strength. But note what the arithmetic says: stacking
*all* of these for a 2x node reduction yields **about one extra ply**. They will
not fix the endgame.

## Recommendation

**Tier 1 first.** It is cheap, it is exact, and it attacks the measured failure
directly — the engine calling a dead draw +371. Tier 3 is worthwhile general
work but is orthogonal to this problem. Tier 2 is the complete solution if
tablebases are acceptable under the project's no-cheating rule.

Every tier must clear `lab/ab_match.py` before being kept, same as everything
else in this directory.
