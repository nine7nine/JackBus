# jackbus-alsa: JackBus's own ALSA plugin (libasound_module_pcm_jackbus.so).
#
#   make            the plugin, in $(OUT)
#   make install    into ALSA's plugin directory and /etc/alsa/conf.d (DESTDIR for packaging)

JACK_TREE  ?= $(abspath ../..)
JACK_BUILD ?= $(JACK_TREE)/build
OUT        ?= $(JACK_BUILD)/jackbus-alsa

ALSA_CFLAGS := $(shell pkg-config --cflags alsa)
ALSA_LIBS   := $(shell pkg-config --libs alsa)
ALSA_PLUGIN_DIR ?= $(shell pkg-config --variable=libdir alsa)/alsa-lib

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

PLUGIN = $(OUT)/libasound_module_pcm_jackbus.so

all: $(PLUGIN)

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++11 -Wall -fno-exceptions -fno-rtti
# JackBus's bridge resampler, for the resampling fallback (built in, not copied)
OBJS = $(OUT)/obj/resample.o $(OUT)/obj/JackBridgeResampler.o

$(OUT)/obj/resample.o: ../pw-shim/resample.cpp ../pw-shim/resample.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(OUT)/obj/JackBridgeResampler.o: $(JACK_TREE)/common/JackBridgeResampler.cpp $(JACK_TREE)/common/JackBridgeResampler.h
	@mkdir -p $(@D)
	$(CXX) -I$(JACK_TREE)/common $(CXXFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(PLUGIN): pcm_jackbus.c $(OBJS) ../compat/compat_conf.h Makefile
	@mkdir -p $(@D)
	$(CC) -DPIC -I$(JACK_TREE)/common $(ALSA_CFLAGS) $(CFLAGS) -fPIC -shared -Wl,--no-undefined \
		-o $@ $< $(OBJS) -L$(JACK_BUILD)/common -ljack $(ALSA_LIBS) -lm

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(DESTDIR)$(ALSA_PLUGIN_DIR)/libasound_module_pcm_jackbus.so
	install -Dm644 50-jackbus.conf $(DESTDIR)/etc/alsa/conf.d/50-jackbus.conf

clean:
	rm -rf $(OUT)

.PHONY: all install clean

# tests: an ALSA program checking timing against JACK's frame clock
test-apps: $(OUT)/alsa-sync

$(OUT)/alsa-sync: test/alsa-sync.c
	@mkdir -p $(@D)
	$(CC) -I$(JACK_TREE)/common $(ALSA_CFLAGS) $(CFLAGS) -o $@ $< -L$(JACK_BUILD)/common -ljack $(ALSA_LIBS)

.PHONY: test-apps
