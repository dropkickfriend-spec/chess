CC      := cc
CFLAGS  := -std=c11 -O2 -Wall -Wextra
SRC_DIR := src
SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(SRCS:.c=.o)
BIN     := chess

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJS) $(BIN)
