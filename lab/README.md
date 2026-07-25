# lab/ — a learning-and-eval experiment layer

This side of the project treats the engine as an instance of a general question:
**what is actually doing the lifting, and how do we know?** Instead of trusting
intuition about whether an eval term or a learning rule helps, we *measure* it.

## Framing

Every learner here is four things: **Theta** (parameters — strategy weights,
eval value tables), **E** (experience — outcome-labelled games), **U** (the
update rule), **Q** (quality — playing strength). Tools in this directory
isolate and measure those pieces.

## `ab_match.py` — ablation harness ("what's doing the lifting")

Plays the full engine head-to-head against a copy with one mechanism switched
**off**, over a fixed set of varied openings, both colours, at a fixed depth.
The score of baseline-vs-ablated is that mechanism's contribution, in Elo.

- A mechanism is ablated by appending `name index 0` lines to a copy of
  `data/learned_values.txt`; the loader is last-wins, so the term is zeroed and
  everything else is identical.
- Fixed depth + a deterministic engine => the whole run is **reproducible**: the
  same command gives the same result. The error bars measure how much the
  verdict leans on the particular opening set, not run-to-run randomness.
- The route book is disabled (engines run in an empty cwd) so each move is a
  pure function of (position, weights).

```
python3 lab/ab_match.py                 # every preset ablation
python3 lab/ab_match.py trap_risk press # just these
python3 lab/ab_match.py --depth 7 --openings 30
python3 lab/ab_match.py --set "trap_w 0 0,PRESS 0 0"   # custom
```

Presets live in `PRESETS` (add one line to test a new term). A verdict of
"HELPS" requires LOS > 95%; "no measurable effect" means the opening set at this
depth can't tell the term apart from noise — usually a sign to raise `--depth`
or `--openings`, or that the term rarely triggers.

## `env_ab.py` — A/B a boolean engine env flag

Same head-to-head method, but the two sides differ by an environment flag rather
than a weights file. `--nodes N` budgets nodes per move instead of depth.

**Search-efficiency changes must be judged at fixed NODES, not fixed depth.** At
fixed depth, more aggressive pruning can only show its downside (the lines it
misses); the upside (more depth for the same work) is invisible. At fixed nodes
both sides do equal work and the real question is asked.

## Findings log

Every number below is head-to-head, both colours, deterministic, so re-running
reproduces it. "±" is the Elo standard error; LOS = likelihood of superiority.

| what | result | verdict |
|---|---|---|
| plan machinery (PLAN_*, LOGI_PLAN, SQV_PLAN, LINE_*) | **-137 ± 35** Elo, LOS 0% | **disabled** — confirmed twice (+137 for plan-off, LOS 100%) |
| └ GAME_PLAN piece pricing | -85, LOS 0.2% | worst single offender |
| └ LOGI_PLAN | -51, LOS 5% | harmful |
| └ SQV_PLAN / LINE_* | ~-40 each | lean harmful |
| └ PLAN_ENGAGE | 0, LOS 50% | inert |
| piece-square tables | **+103 ± 34**, LOS 99.9% | the backbone |
| RESTRICTION (prophylaxis) | **+66 ± 33**, LOS 98% | keeper |
| DEFENDER_LOGISTICS | **+44**, LOS 97% (costs ~8% nps) | keeper — pays for its cost |
| pawn_struct | +22 ± 24, LOS 82% (128 games) | probably positive, unproven |
| blockade | +8 ± 24, LOS 63% (128 games) | neutral |
| coord | -19 ± 24, LOS 22% (128 games) | **kept** — did not firm up |
| trap-risk / commitment axis | ~0, and 47% correct where decisive | **reverted** |
| enemy-relative "initiative race" | coin flip | **reverted** |
| borrowed-horizon book anchor | 0 Elo over 100 games | **off by default** |
| deeper LMR (depth term) | +4 ± 30 at fixed nodes, LOS 55% | neutral — LMR already near optimum |
| hardcoded magics | startup 377 ms -> 5.8 ms (65x) | **landed** — bit-identical search, perft clean |
| learned strategy weights vs uniform | **+86 Elo**, LOS 100% (128 games) | the per-game nudge genuinely works |
| weight fit restarting from uniform | erased that +86 Elo every retune | **fixed** — fit now warm-starts from current weights |
| SPSA-tuned weights vs learned start | **+72 Elo**, LOS 99.9% (128 games, depth 6) | **adopt** — `data/strategy_weights_spsa.txt` |
| endgame piece values (eg_material) | +344, LOS 100% | control: harness detects large effects |
| endgame PSTs (eg_pst) | **+52 ± 25**, LOS 98% | keeper |
| passed-pawn bonus (eg_passed) | **-11 ± 25**, LOS 34% | **earns nothing — investigate** |
| weights fitted on blunder positions | within 1-5% of the SPSA vector | dead end: 492 positions cannot beat the prior |
| PAWN_PROMOTION (strategy) | **+44**, LOS 96% | the passed-pawn term that actually works |
| KING_ACTIVITY + OPPOSITION | **0.0**, LOS 50% (exactly even) | inert in real games |
| SPSA tuned on endgame positions | +11 Elo, and 41% of iterations had ZERO gradient | **the endgame is not an eval problem** |

**The sample-size wall.** coord / blockade / pawn_struct are all sub-25-Elo
effects measured against +/-24 error bars, and going from 80 to 128 games moved
coord from -30 to -19 (regression toward zero = noise, not signal). Halving the
bars needs ~4x the games (~500 each, ~1 h per term) to chase at most ~20 Elo.
That is a bad trade against what is already banked, so all three were left
alone. Cutting a term at LOS 22% would be exactly the guesswork this harness
exists to prevent -- "unresolved" is a real answer, not a to-do.

Two lessons that generalise beyond chess:

1. **Every added heuristic measured neutral; the big win came from deletion.**
   Three well-argued new ideas (commitment, initiative-race, borrowed horizon)
   were all ~0. The +137 came from turning OFF the engine's most elaborate
   existing feature. At this strength, cleverness washes out and the leverage is
   in measuring what's already there.
2. **Outcome-based learning is blind to rare-firing features.** The Texel tuner
   left `trap_w`/`PRESS` at their seeds after 20k positions — not because the
   seeds were right, but because those terms fire too rarely to register in a
   result-prediction objective. Rare features can be neither learned nor
   ablation-tested from ordinary games.

## `spsa.py` — tune weights against strength itself

The Texel-style fit optimises a proxy ("predict the result from a static eval")
that disagrees with strength: unregularised it halves SQUARE_VALUE, measured at
+103 Elo. SPSA instead kicks every live weight +/- c at once, plays the two
vectors against each other over the SAME openings both colours (the paired
design cancels opening luck), and steps toward the winner. Noisy per iteration
but unbiased, and it cannot converge on something that predicts well yet plays
badly.

Result: 80 iterations x 12 games from the nudged weights produced **+72 Elo**
(LOS 99.9%, 128 games at depth 6). It generalises -- held-out openings it never
trained on scored 67.5% at depth 4 and 77.5% at depth 5, so it is not overfitting
the training openings. **Always validate on held-out openings AND a different
depth**: the deterministic engine plus a fixed opening set makes in-sample
validation easy to fool yourself with.

## `blunder_mine.py` — where games are actually lost

Result-only learning gives one bit per game and never says which move was the
mistake, so the tuner labels every position in a lost game "loss" including the
ones where the engine was fine. But each game already stores a per-ply eval
trace, so the collapse is already in the data: scan for the largest drop across
one of OUR moves (evals are White-POV, so a Black blunder is an increase; skip
drops on the opponent's move and swings from already-lost positions).

**Headline finding: 74% of large decisive swings happen after ply 60** (95 of
129 at a 150 cp threshold; 54% at 80 cp, so the bigger the blunder the more
endgame-concentrated it is). The engine is not losing in the opening.

### The endgame audit (what it produced)

Chasing the 74% figure through the endgame machinery gave a clean split:

| working | inert |
|---|---|
| eg_pst +52 | passed_eg -11 |
| PAWN_PROMOTION +44 | KING_ACTIVITY + OPPOSITION 0.0 |

So passed-pawn evaluation IS working -- entirely through PAWN_PROMOTION. The
separate `passed_mg`/`passed_eg` tables (12 tuner parameters) are redundant on
top of it. And of the three endgame-phase strategies, only one contributes.

Caveat worth keeping: KING_ACTIVITY contains the bare-king mate drive, which
only fires in K+R/K+Q vs K positions. Measuring 0 here does not prove the mate
drive is useless -- it proves those positions are too rare in these games to
register. That is the rare-firing blindness again, not a verdict on the term.

Original framing kept for the record: we lose in the endgame, and our
**passed-pawn bonus measures as worth nothing**
— the central endgame concept contributing no measurable strength.

Useful as a DIAGNOSTIC, not as training data: fitting weights on the mined
positions moved them 1-5% from the prior, so there was nothing to A/B.

## The endgame is a SEARCH problem, not an eval problem

Four independent measurements converge on this, and it rules out a whole class
of work:

1. Blunder mining: 74% of large decisive swings happen after ply 60.
2. Ablation: the endgame eval terms are mostly inert — KING_ACTIVITY +
   OPPOSITION score exactly 0.0 (LOS 50%), passed_eg -11 (LOS 34%). Only
   eg_pst (+52) and PAWN_PROMOTION (+44) contribute.
3. SPSA tuned FROM endgame positions: **41% of iterations produced zero
   gradient** (both perturbed vectors scored identically) and the run gained
   +11 Elo, i.e. nothing.
4. The same SPSA on book openings: only 10% zero-gradient, +72 Elo confirmed.

A zero gradient means the outcome did not depend on the weights at all. In sharp
endgames at this depth, who wins is decided by whether the tactic is seen — not
by how MATERIAL is priced against PAWN_STRUCTURE. So **no amount of eval tuning
will fix the endgame weakness**; the lever is search (depth, extensions,
endgame-aware pruning), or exact knowledge like tablebases.

This is worth more than a tuned vector would have been: it says where NOT to
spend effort.

## Which opponents are worth playing (the informative band)

The same zero-gradient argument applies to opponent strength, and it changes how
the ladder should be run. Score by Stockfish skill over the 966-game corpus:

| skill | 0–4 | 5 | 6 | 7 | 8 | 9–13 | 14–16 | 20 |
|---|---|---|---|---|---|---|---|---|
| games | 498 | 50 | 112 | 37 | 35 | 150 | 30 | 48 |
| score | 99→74% | **53.0%** | **43.3%** | **36.5%** | **28.6%** | 8–25% | 5–15% | 3.1% |

A game we lose regardless of the weight vector carries the same information as an
SPSA iteration with zero gradient: none, at full cost. At skill 12+ we score
8–15%, so nearly every game is decided by tactics the search never saw, not by
how the eval is priced. Symmetrically, skills 0–2 (87–99%) are won regardless.

**The learning signal lives where the score is near 50% — skills 5–8 here.**
228 of our games sit at skill 9+, which is where the ladder's compute has been
going and where it has been buying the least. Rungs are still worth a small fixed
sample as a strength *measurement*; they are not worth `--learn` games.

Same shape as the endgame result: an outcome-based learner only learns from
outcomes that could have gone either way.

## The online nudge is degenerate: 5 degrees of freedom, not 16

A 40-game `--learn` run started from the SPSA vector and ended here:

| group | strategies | final weight |
|---|---|---|
| always-active | BLOCKADE, COORDINATION, MATERIAL, RESTRICTION, SQUARE_VALUE | 0.8237–0.8241 |
| opening | CENTER_CONTROL, DEVELOPMENT, KING_SAFETY_OPENING | 1.0776–1.0780 |
| middlegame | ATTACK_POTENTIAL, PAWN_STRUCTURE, PIECE_ACTIVITY | 1.1030–1.1044 |
| endgame | KING_ACTIVITY, OPPOSITION, PAWN_PROMOTION | 1.1151–1.1154 |
| pawn moves | DEFENDER_LOGISTICS | 1.1275 |

Sixteen strategies, five distinct values, identical to 3 decimals inside each
group — and the groups are exactly the hardcoded phase lists in
`analyze_game.py`. Supabase's canonical vector had the same signature
independently. Two defects in `adjust_weights`:

1. **Rank-1 update.** Credit is `surprise * (shares[s] - mean_share)`, and
   `shares` comes from a phase-membership table, not from anything the strategy
   scored. The direction is therefore near-constant and the only per-game
   information is one scalar. One scalar cannot separate 16 strategies.
2. **MEANREV erases the prior.** `lw = 0.85*lw + credit` has a **half-life of
   4.27 games**; 0.15% of the starting vector survived 40 games.

The fixed point rewards *firing frequency*, which is anti-correlated with the
ablation ground truth above: SQUARE_VALUE (+103) and RESTRICTION (+66) were
driven to the bottom cluster while KING_ACTIVITY + OPPOSITION (measured
**exactly 0.0**, LOS 50%) were promoted to the top.

Scores over that run were 16/40 = 40% against a ~40.4% corpus baseline for the
same skill mix — no signal either way, because the weights moved throughout.
That is the point: **a moving brain cannot be measured.**

This also resolves a loose end. The "+86 Elo learned vs uniform" result measured
the *batch fit*, never this nudge in isolation — the nudge has been undoing it.

### Measured cost: -49 Elo

`lab/strat_ab.py`, SPSA vector vs the collapsed vector, 64 openings / 128 games,
both colours, depth 6:

```
+55 =36 -37   SPSA scores 57.0%   Elo +49.2   LOS 97.1%
```

So forty games of `--learn` did not fail to help — it **cost about 49 Elo**,
demolishing most of the +72 the SPSA run had bought. The learning mechanism was
the single most destructive thing in the project after the plan machinery.

Third lesson for the log, alongside "deletion beat every addition" and
"outcome-based learning is blind to rare-firing features":
**check the rank of your update rule.** A learning rule that cannot express the
target configuration will look like it is learning — weights move every game —
while being structurally incapable of finding the answer.

## `replay_nudge.py` — test a learning rule without playing games

The update rule is a pure function of (positions, outcome), and both are already
stored for every game. So a candidate rule can be replayed over the existing
corpus in seconds instead of being evaluated by a multi-hour run whose brain
moves while you measure it.

Validated against reality: replaying the real 40 games through the old rule
reproduces the observed collapse cluster for cluster (0.8456-0.8460 against the
run's 0.8237-0.8241, and so on), so the harness is faithful rather than a
strawman. Two diagnostics:

- **RANK** — distinct weight levels at 1% tolerance. Old rule: **3 levels across
  15 strategies.** That is the defect stated as a number.
- **DIRECTION** — Spearman against the ablation Elo above. Deliberately gated at
  the small-n significance bar (|rho| > 0.68 at n=9), because a rank correlation
  over nine points invites reading a story into noise. Neither rule has cleared
  it, so direction remains an open question that only an A/B can settle.

## v3 of the nudge: credit by predictive accuracy

Participation is replaced by **which way each strategy actually called the
game**. Per position `evalfens` gives every strategy's raw (unweighted) opinion;
`mean/rms` over the game's positions gives a verdict in [-1, +1] from our side,
and credit is `surprise * centred(verdict)`:

| we | strategy said | verdict | weight |
|---|---|---|---|
| won | we were better | it was right | up |
| lost | we were better | it misled us | down |
| lost | we were worse | it warned us | up |

Two details that matter. **Raw, not raw x weight** — otherwise a strategy's
credit scales with the weight it already has, and whatever we currently favour
keeps gaining. **Divide by the strategy's own RMS** — MATERIAL speaks in
hundreds of centipawns and OPPOSITION in single digits, so on a raw scale
MATERIAL alone would set the direction and the update would be rank-1 again.

MEANREV 0.15 -> 0.02 (half-life 4.3 -> 34 games), LR 0.3 -> 0.08.

Replay rank: **3 levels -> 12-14 levels** across 15 strategies. The structural
defect is gone.

### Gate: replay 400 logged games from the SPSA vector, then A/B the result

| depth | score | Elo | LOS |
|---|---|---|---|
| 4 | 50.4% | +2.7 | 54.5% |
| **5** | **66.4%** | **+118.4** | **100.0%** |
| **6** | **59.0%** | **+63.1** | **99.5%** |

Clears the 95% gate at two of three depths and never loses. Depth 4 — much
shallower than anything we play — is the outlier; note the old rule by contrast
*cost* 49 Elo, so "neutral at one depth" is a different category of result.

Worth resolving an apparent contradiction: the new vector moves SQUARE_VALUE
(ablation +103) and RESTRICTION (+66) **down**, yet plays stronger. There is no
conflict. Ablation measured *deleting* a term; this measures *re-pricing* one. A
term can be indispensable — zeroing it costs 103 Elo — while still being
overweighted at 1.14. "Essential" and "overweighted" are compatible, and only
the second is what a weight vector can fix.

## Deferred (next)

- Learning-rule benchmarks on synthetic tasks with known optima (the
  `test_weights*` experiments generalise into this): compare update rules for
  sample-efficiency and collapse-resistance where the true optimum is known.
- Learning-mechanism ablation: freeze the online nudge / the batch fit / the
  Texel tune in turn and measure how strength develops.
- coord / blockade / pawn_struct are parked at the sample-size wall (see above),
  not forgotten -- revisit only with a much larger opening set or faster games.
- Re-run SPSA from the SPSA weights (it had not plateaued) and on each machine's
  own learned_values.txt, since value tables and strategy weights interact.
- Feed the ablation ground truth into the fit: we know pst is +103 and the plan
  machinery was -137, yet the proxy fit cannot see either.
