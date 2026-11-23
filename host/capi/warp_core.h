/* WarpPoint's engine as a plain C interface, so any UI can drive it: the Swift menu
 * bar app today, something else on Windows or Linux later.
 *
 * Strings going in are UTF-8. Strings coming out are malloc'd UTF-8 and must be freed
 * with warp_free. Events and status are small JSON objects (see host/app_core.hpp).
 * All functions are safe to call from the UI's main thread; none of them blocks on
 * the network (resolve may wait up to ~30 s for a sync in progress).
 */
#ifndef WARP_CORE_H
#define WARP_CORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WarpCore WarpCore;

/* dataDir: where state.json, config.json, backups/ and titles/ live. */
WarpCore* warp_core_create(const char* dataDir);
void warp_core_destroy(WarpCore* core); /* stops syncing first */

/* Start syncing automatically. On failure returns false and *error (if given) is set
 * to a message to free with warp_free. */
bool warp_core_start(WarpCore* core, char** error);
void warp_core_sync_now(WarpCore* core);

/* The next event (JSON) or NULL when there is none. Poll it a few times a second. */
char* warp_core_next_event(WarpCore* core);

/* {"conflicts": [...], "error": ""} */
char* warp_core_status(WarpCore* core);

/* Settle a conflict; keepLocal = the emulator's copy wins. Then syncs. */
bool warp_core_resolve(WarpCore* core, const char* device, const char* key, bool keepLocal,
                       char** error);

/* The library: {"console","consoleSeen","emulator","emulatorOpen","consolePresent",
 * "games": [{"key","titleId","name","publisher","icon","onConsole","inEmulator",
 * "status","lastText","lastAt"}]}. status is one of conflict, synced, consoleOnly,
 * emulatorOnly, problem, new, ignored. */
char* warp_core_games(WarpCore* core);

/* A save's backups, newest first: [{"id","side","reason","created","files","bytes"}]. */
char* warp_core_backups(WarpCore* core, const char* key);

/* Put a backup back into the emulator (current copy backed up first), then sync. */
bool warp_core_restore(WarpCore* core, const char* key, const char* backupId, char** error);

/* Setup and Settings: {"emulator":{"name","path","found"},"console","consolePresent",
 * "consoleUsers":[{"id","name","saves"}],"emulatorProfiles":[{"id","name"}],
 * "links":{consoleUser:emulatorUser},"keepBackups","needsSetup"} */
char* warp_core_setup(WarpCore* core);
/* Link a console user to an emulator profile; "" or NULL emulatorUser unlinks. */
bool warp_core_set_link(WarpCore* core, const char* consoleUser, const char* emulatorUser,
                        char** error);
/* Backups to keep per save (0 = all). */
bool warp_core_set_keep_backups(WarpCore* core, int keep, char** error);
/* The emulator's data folder (the one with bis/ and system/ in it). */
bool warp_core_set_emulator_path(WarpCore* core, const char* path, char** error);

/* Stop syncing a save (ignored = true) or start again. Nothing is deleted. */
bool warp_core_set_ignored(WarpCore* core, const char* key, bool ignored, char** error);

/* Copy a console-only save into the emulator ("Bring to Astris"). */
bool warp_core_bring_to_emulator(WarpCore* core, const char* key, char** error);

/* The folder the core was created with (for "Show in Finder"). */
char* warp_core_data_dir(WarpCore* core);

void warp_free(char* s);

#ifdef __cplusplus
}
#endif

#endif
