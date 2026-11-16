# jackbus-pa: a libpulse.so.0 on JackBus.
#
#   make              the libraries, in $(OUT)/lib
#   make install      into /usr/lib/jackbus-pa, /usr/bin, and that directory into
#                     /etc/ld.so.conf.d (DESTDIR for packaging)
#
# libpulse.so.0, the library applications load, is a dispatcher (dispatch.c):
# each libpulse function a jump to JackBus's implementation, libjackbus-pa.so,
# while JackBus runs, else to PulseAudio's own libpulse (PipeWire plays it).
# libjackbus-pa.so implements libpulse's client API (main loops, context,
# streams, introspection) and links PulseAudio's own libpulsecommon, where the
# rest of libpulse's API (sample specs, channel maps, volumes, property lists,
# formats, ...) comes from; and alsa-lib, for the cards' own volume (mixer.c).

JACK_TREE  ?= $(abspath ../..)
JACK_BUILD ?= $(JACK_TREE)/build
OUT        ?= $(JACK_BUILD)/pa-shim

PULSE_CFLAGS  := $(shell pkg-config --cflags libpulse)
ALSA_LIBS     := $(shell pkg-config --libs alsa)
PULSE_LIBDIR  := $(shell pkg-config --variable=libdir libpulse)
PULSE_REAL    := $(PULSE_LIBDIR)/libpulse.so.0
PULSE_PRIVDIR := $(PULSE_LIBDIR)/pulseaudio
PULSE_COMMON  := $(firstword $(wildcard $(PULSE_PRIVDIR)/libpulsecommon-*.so))

CFLAGS   ?= -O2 -g
CFLAGS   += -DJACKBUS_COMPAT_LAYER='"pa"' -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS ?= -O2 -g
CXXFLAGS += -DJACKBUS_COMPAT_LAYER='"pa"' -std=c++11 -Wall -fno-exceptions -fno-rtti
CPPFLAGS += $(PULSE_CFLAGS) -I$(JACK_TREE)/common

SRCS = mainloop.c node.c context.c stream.c server.c mixer.c
OBJS = $(OUT)/obj/resample.o $(OUT)/obj/JackBridgeResampler.o
IMPL = $(OUT)/lib/libjackbus-pa.so
SHIM = $(OUT)/lib/libpulse.so.0
GEN  = $(OUT)/gen

PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib/jackbus-pa

all: $(SHIM) $(IMPL)

$(GEN)/trampolines.S $(GEN)/names.h &: gen-trampolines.sh $(PULSE_REAL)
	sh gen-trampolines.sh $(PULSE_REAL) $(GEN)

$(SHIM): dispatch.c $(GEN)/trampolines.S $(GEN)/names.h pa.map ../compat/server.h ../compat/compat_conf.h Makefile
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
		-o $@ $(SRCS) $(OBJS) $(PULSE_COMMON) -L$(JACK_BUILD)/common -ljack $(ALSA_LIBS) -lpthread -lm

# test helpers: a dlsym interposer (as teams-sdl-cef's audio probe has); a
# second card; a card's mixer (an ALSA control plugin, never a real card)
test-apps: $(OUT)/dlsym-hook.so $(OUT)/pa-dlsym-client $(OUT)/fake-card $(OUT)/libasound_module_ctl_jbtest.so \
	$(OUT)/ca-play

# libcanberra's PulseAudio driver, as the desktop's event sounds use it
$(OUT)/ca-play: test/ca-play.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< -lcanberra -lpthread

$(OUT)/libasound_module_ctl_jbtest.so: test/fake-mixer.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DPIC -fPIC -shared -o $@ $< $(ALSA_LIBS)

$(OUT)/fake-card: test/fake-card.c
	@mkdir -p $(@D)
	$(CC) -I$(JACK_TREE)/common $(CFLAGS) -o $@ $< -L$(JACK_BUILD)/common -ljack

$(OUT)/pa-dlsym-client: test/pa-dlsym-client.c
	@mkdir -p $(@D)
	$(CC) $(PULSE_CFLAGS) $(CFLAGS) -o $@ $< -lpulse

$(OUT)/dlsym-hook.so: test/dlsym-hook.c
	@mkdir -p $(@D)
	$(CC) $(PULSE_CFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

install: $(SHIM) $(IMPL)
	install -Dm755 $(SHIM) $(DESTDIR)$(LIBDIR)/libpulse.so.0
	install -Dm755 $(IMPL) $(DESTDIR)$(LIBDIR)/libjackbus-pa.so
	install -Dm755 jackbus-pa $(DESTDIR)$(PREFIX)/bin/jackbus-pa
	install -d $(DESTDIR)/etc/ld.so.conf.d
	echo "$(LIBDIR)" >$(DESTDIR)/etc/ld.so.conf.d/jackbus-pa.conf

clean:
	rm -rf $(OUT)

.PHONY: all install test-apps clean
