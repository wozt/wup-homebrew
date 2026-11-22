# wup-emu: the Wii U GamePad emulator (needs SDL2: brew install sdl2)
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

wup-emu: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(SDL_LIBS)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build wup-emu
.PHONY: clean
