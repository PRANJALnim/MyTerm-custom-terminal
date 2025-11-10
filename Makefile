CC=gcc
CFLAGS=-Wall -Wextra -std=c11 -O2
LDFLAGS=-lX11

SRC=$(wildcard src/*.c)
OBJ=$(SRC:.c=.o)
BIN=myterm

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: all
	./myterm

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all run clean
