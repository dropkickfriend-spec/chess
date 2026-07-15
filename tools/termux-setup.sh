#!/data/data/com.termux/files/usr/bin/bash
# One-shot setup for running chess-bb on an Android phone under Termux.
# Usage: bash tools/termux-setup.sh   (from a clone of the repo)
set -e

pkg install -y clang make git python stockfish
pip install --only-binary=:all: chess || pip install chess

make
./chess test

echo
echo "Ready. Things you can run:"
echo "  ./chess                                             # UCI engine"
echo "  python tools/match.py --games 10 --movetime 0.1 \\"
echo "      --skill 5 --upload                              # SF match -> Supabase"
echo "  LICHESS_TOKEN=... python tools/lichess_bot.py       # play on lichess"
echo
echo "To keep matches running with the screen off: pkg install termux-services"
echo "and run inside 'termux-wake-lock'."
