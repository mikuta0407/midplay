# midplay -- macOS only (CoreAudio, AudioToolbox, CoreMIDI)
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra
ARCHS   ?= $(shell uname -m)
ARCHF   := $(foreach a,$(ARCHS),-arch $(a))
LIBS    := -framework AudioToolbox -framework CoreAudio -framework CoreMIDI -framework CoreFoundation -liconv
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
PREFIX  ?= $(HOME)/.local

all: build/midplay

build/%.o: src/%.c $(wildcard src/*.h) Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(ARCHF) -c $< -o $@

build/midplay: $(OBJ)
	$(CC) $(ARCHF) $^ -o $@ $(LIBS)

# a virtual MIDI destination that logs what it receives (tests/check_midi.py uses it)
build/midi_sink: tests/midi_sink.c Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(ARCHF) $< -o $@ -framework CoreMIDI -framework CoreFoundation

install: build/midplay
	mkdir -p $(PREFIX)/bin
	cp build/midplay $(PREFIX)/bin/midplay

clean:
	rm -rf build

.PHONY: all install clean
