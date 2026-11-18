/*
 * JackBus: a sound card's mixer state, the user's own.
 *
 * Saved from the card's simple-mixer controls (the ones alsamixer shows) into
 * $XDG_CONFIG_HOME/jackbus/mixer/<card id>.state (else ~/.config/...), and put
 * back by JackBus whenever it opens the card: the server starting, a bridge
 * added or its card plugged back in. Whatever touched the card meanwhile (a
 * sound server's card profiles while JackBus had let go of it) is undone. Saved
 * only on request (jackbus-mixer save, PatchBox's Mixer tab); nothing saved,
 * nothing restored.
 *
 * One line per control and what it holds, its raw values per channel:
 *     <name>|<index>|pvol|<channel>:<value> ...
 *     <name>|<index>|psw|<channel>:<0|1> ...
 *     <name>|<index>|cvol|... · <name>|<index>|csw|...
 *     <name>|<index>|enum|<item name>
 */
#ifndef JACKBUS_MIXER_STATE_H
#define JACKBUS_MIXER_STATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the card's id ("PCH", "U44") from an ALSA device name ("hw:PCH,0", "hw:1",
 * "plughw:CARD=U44,DEV=0"): its index, or -1 when it names no card */
int jackbus_mixer_card_id(const char *device, char *id, size_t len);
/* the file of card id */
int jackbus_mixer_state_path(const char *id, char *path, size_t len);
/* 0 saved; -errno */
int jackbus_mixer_save(const char *device);
/* the number of controls put back (0: nothing saved for the card); -errno.
 * Controls the card no longer has are skipped. */
int jackbus_mixer_restore(const char *device);

#ifdef __cplusplus
}
#endif

#endif
