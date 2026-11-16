# jackbus-pa: a libpulse.so.0 on JackBus.
#
#   make              the shim, in $(OUT)/lib
#
# The library implements libpulse's client API (main loops, context, streams,
# introspection) and links PulseAudio's own libpulsecommon, where the rest of
# libpulse's API (sample specs, channel maps, volumes, property lists,
# formats, ...) comes from.

JACK_TREE  ?= $(abspath ../..)
JACK_BUILD ?= $(JACK_TREE)/build
OUT        ?= $(JACK_BUILD)/pa-shim

PULSE_CFLAGS  := $(shell pkg-config --cflags libpulse)
PULSE_PRIVDIR := $(shell pkg-config --variable=libdir libpulse)/pulseaudio
PULSE_COMMON  := $(firstword $(wildcard $(PULSE_PRIVDIR)/libpulsecommon-*.so))

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++11 -Wall -fno-exceptions -fno-rtti
CPPFLAGS += $(PULSE_CFLAGS) -I$(JACK_TREE)/common

SRCS = mainloop.c node.c context.c stream.c
OBJS = $(OUT)/obj/resample.o $(OUT)/obj/JackBridgeResampler.o
SHIM = $(OUT)/lib/libpulse.so.0

all: $(SHIM)

$(OUT)/obj/resample.o: ../pw-shim/resample.cpp ../pw-shim/resample.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(OUT)/obj/JackBridgeResampler.o: $(JACK_TREE)/common/JackBridgeResampler.cpp $(JACK_TREE)/common/JackBridgeResampler.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(SHIM): $(SRCS) $(OBJS) shim.h pa.map ../compat/compat_conf.h Makefile
	@test -n "$(PULSE_COMMON)" || { echo "libpulsecommon not found in $(PULSE_PRIVDIR)"; exit 1; }
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -shared \
		-Wl,-soname,libpulse.so.0 -Wl,--version-script=pa.map -Wl,--no-undefined \
		-Wl,-rpath,$(PULSE_PRIVDIR) \
		-o $@ $(SRCS) $(OBJS) $(PULSE_COMMON) -L$(JACK_BUILD)/common -ljack -lpthread -lm

clean:
	rm -rf $(OUT)

.PHONY: all clean
