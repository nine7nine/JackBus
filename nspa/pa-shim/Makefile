# jackbus-pa: a libpulse.so.0 on JackBus.
#
#   make              the libraries, in $(OUT)/lib
#   make install      into /usr/lib/jackbus-pa and /usr/bin (DESTDIR for packaging)
#
# libpulse.so.0, the library applications load, is a dispatcher (dispatch.c):
# each libpulse function a jump to JackBus's implementation, libjackbus-pa.so,
# while JackBus runs, else to PulseAudio's own libpulse (PipeWire plays it).
# libjackbus-pa.so implements libpulse's client API (main loops, context,
# streams, introspection) and links PulseAudio's own libpulsecommon, where the
# rest of libpulse's API (sample specs, channel maps, volumes, property lists,
# formats, ...) comes from.

JACK_TREE  ?= $(abspath ../..)
JACK_BUILD ?= $(JACK_TREE)/build
OUT        ?= $(JACK_BUILD)/pa-shim

PULSE_CFLAGS  := $(shell pkg-config --cflags libpulse)
PULSE_LIBDIR  := $(shell pkg-config --variable=libdir libpulse)
PULSE_REAL    := $(PULSE_LIBDIR)/libpulse.so.0
PULSE_PRIVDIR := $(PULSE_LIBDIR)/pulseaudio
PULSE_COMMON  := $(firstword $(wildcard $(PULSE_PRIVDIR)/libpulsecommon-*.so))

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++11 -Wall -fno-exceptions -fno-rtti
CPPFLAGS += $(PULSE_CFLAGS) -I$(JACK_TREE)/common

SRCS = mainloop.c node.c context.c stream.c server.c
OBJS = $(OUT)/obj/resample.o $(OUT)/obj/JackBridgeResampler.o
IMPL = $(OUT)/lib/libjackbus-pa.so
SHIM = $(OUT)/lib/libpulse.so.0
GEN  = $(OUT)/gen

PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib/jackbus-pa

all: $(SHIM) $(IMPL)

$(GEN)/trampolines.S $(GEN)/names.h &: gen-trampolines.sh $(PULSE_REAL)
	sh gen-trampolines.sh $(PULSE_REAL) $(GEN)

$(SHIM): dispatch.c $(GEN)/trampolines.S $(GEN)/names.h pa.map ../compat/server.h Makefile
	@mkdir -p $(@D)
	$(CC) -I$(GEN) $(CFLAGS) -DPULSE_REAL_LIB='"$(PULSE_REAL)"' -fPIC -fvisibility=hidden -shared $(LDFLAGS) \
		-Wl,-soname,libpulse.so.0 -Wl,--version-script=pa.map -Wl,--no-undefined \
		-o $@ dispatch.c $(GEN)/trampolines.S

$(OUT)/obj/resample.o: ../pw-shim/resample.cpp ../pw-shim/resample.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(OUT)/obj/JackBridgeResampler.o: $(JACK_TREE)/common/JackBridgeResampler.cpp $(JACK_TREE)/common/JackBridgeResampler.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(IMPL): $(SRCS) $(OBJS) shim.h pa.map ../compat/compat_conf.h ../compat/server.h Makefile
	@test -n "$(PULSE_COMMON)" || { echo "libpulsecommon not found in $(PULSE_PRIVDIR)"; exit 1; }
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -shared $(LDFLAGS) \
		-Wl,-soname,libjackbus-pa.so -Wl,--version-script=pa.map -Wl,--no-undefined \
		-Wl,-rpath,$(PULSE_PRIVDIR) \
		-o $@ $(SRCS) $(OBJS) $(PULSE_COMMON) -L$(JACK_BUILD)/common -ljack -lpthread -lm

install: $(SHIM) $(IMPL)
	install -Dm755 $(SHIM) $(DESTDIR)$(LIBDIR)/libpulse.so.0
	install -Dm755 $(IMPL) $(DESTDIR)$(LIBDIR)/libjackbus-pa.so
	install -Dm755 jackbus-pa $(DESTDIR)$(PREFIX)/bin/jackbus-pa

clean:
	rm -rf $(OUT)

.PHONY: all install clean
