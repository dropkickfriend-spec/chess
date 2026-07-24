CC      := cc
# -MMD -MP emits a .d file per object listing the headers it includes, so
# editing a header (e.g. changing STRAT_COUNT in eval_strategy.h) recompiles
# every object that depends on it. Without this, stale objects keep an old
# header's constants and silently disagree across translation units.
CFLAGS  := -std=c11 -O2 -Wall -Wextra -MMD -MP
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
