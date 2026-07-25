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
| blockade / pawn_struct | +26 / +22, LOS ~75-80% | probably positive, unresolved |
| coord | -30, LOS 17% | possibly a small liability, unresolved |
| trap-risk / commitment axis | ~0, and 47% correct where decisive | **reverted** |
| enemy-relative "initiative race" | coin flip | **reverted** |
| borrowed-horizon book anchor | 0 Elo over 100 games | **off by default** |
| deeper LMR (depth term) | +4 ± 30 at fixed nodes, LOS 55% | neutral — LMR already near optimum |

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

## Deferred (next)

- Learning-rule benchmarks on synthetic tasks with known optima (the
  `test_weights*` experiments generalise into this): compare update rules for
  sample-efficiency and collapse-resistance where the true optimum is known.
- Learning-mechanism ablation: freeze the online nudge / the batch fit / the
  Texel tune in turn and measure how strength develops.
- Resolve `coord` (-30, LOS 17%) and `blockade`/`pawn_struct` with more games.
- Startup cost: `find_magic` recomputes magics every launch. Irrelevant to search
  depth, but the harness and training loops restart the engine twice per game, so
  hardcoding them would raise games/hour (faster learning and measurement).
