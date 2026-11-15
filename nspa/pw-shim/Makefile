# jackbus-pw spike: a libpipewire-0.3.so.0 on JackBus.
#
#   make              the shim, in $(OUT)/lib
#   make test-apps    test/pw-probe and test/jbpw-check, plus PipeWire's
#                     audio-dsp-filter example if PW_EXAMPLE names its source.
#                     The PipeWire apps link the REAL libpipewire-0.3, as a
#                     distro build would; jackbus-pw swaps it at run time.

JACK_TREE  ?= $(abspath ../..)
JACK_BUILD ?= $(JACK_TREE)/build
OUT        ?= $(JACK_BUILD)/pw-shim
PW_EXAMPLE ?=

PW_CFLAGS     := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS       := $(shell pkg-config --libs libpipewire-0.3)
SPA_PLUGINDIR := $(shell pkg-config --variable=plugindir libspa-0.2)

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CPPFLAGS += $(PW_CFLAGS) -I$(JACK_TREE)/common

SRCS = pw.c loop.c core.c filter.c
SHIM = $(OUT)/lib/libpipewire-0.3.so.0

all: $(SHIM)

$(SHIM): $(SRCS) shim.h Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DSHIM_SPA_PLUGINDIR='"$(SPA_PLUGINDIR)"' $(CFLAGS) \
		-fPIC -fvisibility=hidden -shared \
		-Wl,-soname,libpipewire-0.3.so.0 -Wl,--no-undefined -Wl,-z,defs \
		-o $@ $(SRCS) -L$(JACK_BUILD)/common -ljack -ldl -lpthread -lm

TEST_APPS = $(OUT)/pw-probe $(OUT)/jbpw-check $(if $(PW_EXAMPLE),$(OUT)/audio-dsp-filter)

test-apps: $(TEST_APPS)

# PipeWire clients: built against the system's libpipewire-0.3
$(OUT)/pw-probe: test/pw-probe.c
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

$(OUT)/audio-dsp-filter: $(PW_EXAMPLE)
	@mkdir -p $(@D)
	$(CC) $(PW_CFLAGS) $(CFLAGS) -o $@ $< $(PW_LIBS) -lm

# a plain JACK client that feeds and checks the filter
$(OUT)/jbpw-check: test/jbpw-check.c
	@mkdir -p $(@D)
	$(CC) -I$(JACK_TREE)/common $(CFLAGS) -o $@ $< -L$(JACK_BUILD)/common -ljack -lm

clean:
	rm -rf $(OUT)

.PHONY: all test-apps clean
