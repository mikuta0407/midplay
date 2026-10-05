# midplay -- macOS (CoreAudio, AudioToolbox, CoreMIDI) or Linux (ALSA, FluidSynth)
CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra
PREFIX  ?= $(HOME)/.local
OS      := $(shell uname -s)
COMMON  := $(filter-out src/out_%.c,$(wildcard src/*.c))

ifeq ($(OS),Darwin)
ARCHS   ?= $(shell uname -m)
ARCHF   := $(foreach a,$(ARCHS),-arch $(a))
SRC     := $(COMMON) src/out_au.c src/out_midi.c
LIBS    := -framework AudioToolbox -framework CoreAudio -framework CoreMIDI -framework CoreFoundation -liconv
SINK    := tests/midi_sink.c
SINKLIB := -framework CoreMIDI -framework CoreFoundation
else
PKGS    := alsa fluidsynth
override CFLAGS += -D_DEFAULT_SOURCE -Wno-format-truncation $(shell pkg-config --cflags $(PKGS))
SRC     := $(COMMON) src/out_alsa.c src/out_fluid.c
LIBS    := $(shell pkg-config --libs $(PKGS)) -lpthread
SINK    := tests/midi_sink_alsa.c
SINKLIB := $(shell pkg-config --libs alsa)
endif

OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))

all: build/midplay

build/%.o: src/%.c $(wildcard src/*.h) Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(ARCHF) -c $< -o $@

build/midplay: $(OBJ)
	$(CC) $(ARCHF) $^ -o $@ $(LIBS)

# a virtual MIDI destination that logs what it receives (tests/check_midi.py uses it)
build/midi_sink: $(SINK) Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(ARCHF) $< -o $@ $(SINKLIB)

install: build/midplay
	mkdir -p $(PREFIX)/bin
	cp build/midplay $(PREFIX)/bin/midplay

clean:
	rm -rf build

.PHONY: all install clean
