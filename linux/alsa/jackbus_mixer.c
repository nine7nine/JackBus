/*
 * jackbus-mixer: a sound card's mixer state, the user's own
 * (jackbus_mixer_state.h). JackBus puts a saved state back whenever it opens
 * the card.
 *
 *   jackbus-mixer save <card>       the card's controls as they are now
 *   jackbus-mixer restore <card>    put the saved ones back now
 *   jackbus-mixer path <card>       where they are kept (exit 1: nothing saved)
 *
 * <card>: its id (PCH, U44), its index, or an ALSA device (hw:PCH,0).
 */
#include "jackbus_mixer_state.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 3 || (strcmp(argv[1], "save") && strcmp(argv[1], "restore") && strcmp(argv[1], "path"))) {
		fprintf(stderr, "usage: %s save|restore|path <card>\n", argv[0]);
		return 2;
	}
	char dev[128], id[64], path[4096];
	snprintf(dev, sizeof(dev), "%s%s", strchr(argv[2], ':') ? "" : "hw:", argv[2]);
	if (jackbus_mixer_card_id(dev, id, sizeof(id)) < 0 || jackbus_mixer_state_path(id, path, sizeof(path)) < 0) {
		fprintf(stderr, "%s: no card %s\n", argv[0], argv[2]);
		return 1;
	}
	if (strcmp(argv[1], "path") == 0) {
		printf("%s\n", path);
		return access(path, R_OK) == 0 ? 0 : 1;
	}
	if (strcmp(argv[1], "save") == 0) {
		int r = jackbus_mixer_save(dev);
		if (r < 0) {
			fprintf(stderr, "%s: can't save card %s: %s\n", argv[0], id, strerror(-r));
			return 1;
		}
		printf("saved: %s\n", path);
		return 0;
	}
	int n = jackbus_mixer_restore(dev);
	if (n < 0) {
		fprintf(stderr, "%s: can't restore card %s: %s\n", argv[0], id, strerror(-n));
		return 1;
	}
	if (n == 0) {
		fprintf(stderr, "%s: nothing saved for card %s (%s)\n", argv[0], id, path);
		return 1;
	}
	printf("restored: %d controls of card %s\n", n, id);
	return 0;
}
