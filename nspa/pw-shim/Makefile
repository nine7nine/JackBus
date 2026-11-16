# jackbus-pw spike: a libpipewire-0.3.so.0 on JackBus.
#
#   make              the shim, in $(OUT)/lib
#   make test-apps    test/pw-probe, test/pw-twofilter and test/jbpw-check,
#                     plus PipeWire's audio-dsp-filter, audio-src and
#                     audio-capture examples if PW_EXAMPLES names their
#                     directory (src/examples of a PipeWire tree).
#                     The PipeWire apps link the REAL libpipewire-0.3, as a
#                     distro build would; jackbus-pw swaps it at run time.

JACK_TREE  ?= $(abspath ../..)
JACK_BUILD ?= $(JACK_TREE)/build
OUT        ?= $(JACK_BUILD)/pw-shim
PW_EXAMPLES ?=

PW_CFLAGS     := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS       := $(shell pkg-config --libs libpipewire-0.3)
SPA_PLUGINDIR := $(shell pkg-config --variable=plugindir libspa-0.2)

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++11 -Wall -fno-exceptions -fno-rtti
CPPFLAGS += $(PW_CFLAGS) -I$(JACK_TREE)/common

SRCS = pw.c loop.c core.c filter.c app.c stream.c midi.c
# JackBus's bridge resampler, for streams at another rate (built in, not copied)
RESAMPLER = $(JACK_TREE)/common/JackBridgeResampler.cpp
OBJS = $(OUT)/obj/resample.o $(OUT)/obj/JackBridgeResampler.o
SHIM = $(OUT)/lib/libpipewire-0.3.so.0

all: $(SHIM)

$(OUT)/obj/resample.o: resample.cpp resample.h $(JACK_TREE)/common/JackBridgeResampler.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(OUT)/obj/JackBridgeResampler.o: $(RESAMPLER) $(JACK_TREE)/common/JackBridgeResampler.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(SHIM): $(SRCS) $(OBJS) shim.h resample.h ../compat/compat_conf.h Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DSHIM_SPA_PLUGINDIR='"$(SPA_PLUGINDIR)"' $(CFLAGS) \
		-fPIC -fvisibility=hidden -shared $(LDFLAGS) \
		-Wl,-soname,libpipewire-0.3.so.0 -Wl,--no-undefined -Wl,-z,defs \
		-o $@ $(SRCS) $(OBJS) -L$(JACK_BUILD)/common -ljack -ldl -lpthread -lm

PREFIX ?= /usr
install: $(SHIM)
	install -Dm755 $(SHIM) $(DESTDIR)$(PREFIX)/lib/jackbus-pw/libpipewire-0.3.so.0
	install -Dm755 jackbus-pw $(DESTDIR)$(PREFIX)/bin/jackbus-pw

TEST_APPS = $(OUT)/pw-probe $(OUT)/pw-twofilter $(OUT)/pw-midifilter $(OUT)/pw-linktest $(OUT)/jbpw-check \
	$(if $(shell pkg-config --exists sdl3 && echo y),$(OUT)/sdl3-tone) \
	$(if $(shell pkg-config --exists openal && echo y),$(OUT)/openal-tone) \
	$(if $(PW_EXAMPLES),$(OUT)/audio-dsp-filter $(OUT)/audio-src $(OUT)/audio-capture)

test-apps: $(TEST_APPS)

# PipeWire clients: built against the system's libpipewire-0.3
$(OUT)/pw-probe: test/pw-probe.c
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

$(OUT)/pw-twofilter: test/pw-twofilter.c
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

$(OUT)/pw-midifilter: test/pw-midifilter.c
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

$(OUT)/pw-linktest: test/pw-linktest.c
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

$(OUT)/audio-dsp-filter $(OUT)/audio-src $(OUT)/audio-capture: $(OUT)/%: $(PW_EXAMPLES)/%.c
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

# libraries that load libpipewire themselves (SDL3, OpenAL Soft)
$(OUT)/sdl3-tone: test/sdl3-tone.c
	@mkdir -p $(@D)
	$(CC) $(shell pkg-config --cflags sdl3) $(CFLAGS) -o $@ $< $(shell pkg-config --libs sdl3) -lm

$(OUT)/openal-tone: test/openal-tone.c
	@mkdir -p $(@D)
	$(CC) $(shell pkg-config --cflags openal) $(CFLAGS) -o $@ $< $(shell pkg-config --libs openal) -lm

# a plain JACK client that feeds and checks the filter
$(OUT)/jbpw-check: test/jbpw-check.c
	@mkdir -p $(@D)
	$(CC) -I$(JACK_TREE)/common $(CFLAGS) -o $@ $< -L$(JACK_BUILD)/common -ljack -lm

clean:
	rm -rf $(OUT)

.PHONY: install all test-apps clean
