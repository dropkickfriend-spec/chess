CC      := cc
# -MMD -MP emits a .d file per object listing the headers it includes, so
# editing a header (e.g. changing STRAT_COUNT in eval_strategy.h) recompiles
# every object that depends on it. Without this, stale objects keep an old
# header's constants and silently disagree across translation units.
CFLAGS  := -std=c11 -O2 -Wall -Wextra -MMD -MP
# Without a -march, COUNT() links libgcc's __popcountdi2 — a software popcount
# loop — even though every CPU we build on has the instruction. A bitboard
# engine calls it constantly (it showed up at 5.9% of search time in gprof).
# -march=native picks up POPCNT on x86 and the ARM equivalent under Termux;
# measured 4% faster with bit-identical node counts. Probed rather than assumed,
# so a compiler that rejects it still builds.
ARCHFLAG := $(shell $(CC) -march=native -E -x c /dev/null >/dev/null 2>&1 \
                    && echo -march=native)
CFLAGS  += $(ARCHFLAG)
SRC_DIR := src
SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(SRCS:.c=.o)
DEPS    := $(OBJS:.o=.d)
BIN     := chess

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJS) $(DEPS) $(BIN)

-include $(DEPS)
