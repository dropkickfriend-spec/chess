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

## Deferred (next)

- Learning-rule benchmarks on synthetic tasks with known optima (the
  `test_weights*` experiments generalise into this): compare update rules for
  sample-efficiency and collapse-resistance where the true optimum is known.
- Learning-mechanism ablation: freeze the online nudge / the batch fit / the
  Texel tune in turn and measure how strength develops.
