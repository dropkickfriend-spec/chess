#!/bin/sh
# Safe update for a machine that plays games: pull, rebuild, verify.
#
# data/learned_values.txt is tracked but is REWRITTEN locally by --retune and
# --sync, so a plain `git pull` aborts whenever an incoming commit also touches
# it. The canonical copy lives in Supabase, so the local edit is disposable —
# but only that file. Anything else modified locally is left alone, and the pull
# will still (correctly) refuse rather than silently discarding your work.
set -e
cd "$(dirname "$0")/.."

if ! git diff --quiet -- data/learned_values.txt 2>/dev/null; then
    echo "discarding local data/learned_values.txt (canonical copy is in Supabase)"
    git checkout -- data/learned_values.txt
fi

git pull
make clean && make

./chess test | tail -1
if [ ! -f strategy_weights.txt ]; then
    echo "no strategy_weights.txt — installing the adopted brain"
    cp data/strategy_weights_nudged.txt strategy_weights.txt
fi
echo "up to date. brain: $(head -c 40 strategy_weights.txt | tr '\n' ' ')..."
