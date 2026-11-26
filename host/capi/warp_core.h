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

/* Warp Mods (see AppCore::modsJson). titleId: 16 hex. Changes apply at the next sync. */
char* warp_core_mods(WarpCore* core, const char* titleId);
bool warp_core_add_mod(WarpCore* core, const char* titleId, const char* folder, const char* name,
                       char** error);
bool warp_core_remove_mod(WarpCore* core, const char* titleId, const char* name, char** error);
bool warp_core_set_mod_enabled(WarpCore* core, const char* titleId, const char* name, bool on,
                               char** error);

/* Warp Tap: one look at the game running on the console:
 * {"game","titleId","buildId","supported","status","fields":[{"id","label","group","value","note"}]}.
 * Connects for this look only; poll it every second or two while a window shows it. */
char* warp_core_tap(WarpCore* core);

/* Warp Lens: crash reports from the console, newest first (see AppCore::reportsJson).
 * New crashes also arrive as events: {"kind":"crash","console","name","titleId","icon",
 * "image","message"}. */
char* warp_core_reports(WarpCore* core);

/* A save's backups, newest first: [{"id","side","reason","created","files","bytes"}]. */
char* warp_core_backups(WarpCore* core, const char* key);

/* Warp Chronicle: label a backup ("note") and pin it (never pruned). Backups list "note","pinned". */
bool warp_core_label_backup(WarpCore* core, const char* key, const char* backupId, const char* note,
                            bool pinned, char** error);

/* Put a backup back into the emulator (current copy backed up first), then sync. */
bool warp_core_restore(WarpCore* core, const char* key, const char* backupId, char** error);

/* Setup and Settings: {"emulator":{"name","path","found"},"console","consolePresent",
 * "consoleUsers":[{"id","name","saves"}],"emulatorProfiles":[{"id","name"}],
 * "links":{consoleUser:emulatorUser},"keepBackups","needsSetup"} */
char* warp_core_setup(WarpCore* core);
/* Link a console user to an emulator profile; "" or NULL emulatorUser unlinks. */
bool warp_core_set_link(WarpCore* core, const char* consoleUser, const char* emulatorUser,
                        char** error);
/* Warp Presence: show the Switch's game in Discord (setup's "presence": {"enabled",
 * "discordAppId","status"}); events {"kind":"playing","console","titleId","name","icon",
 * "presence"} say what's running there. */
/* Warp Split: LiveSplit auto-splitting (setup's "split": {"enabled","host","port","status"}). */
bool warp_core_set_split(WarpCore* core, bool enabled, const char* host, int port, char** error);
/* Warp Album: setup's "album": {"enabled","folder"}; events {"kind":"album","console",
 * "name","titleId","icon","image","message"} say what was copied. */
bool warp_core_set_album(WarpCore* core, bool enabled, const char* folder, char** error);
bool warp_core_set_presence(WarpCore* core, bool enabled, const char* discordAppId, char** error);
/* Show games running on this Mac too (setup's "presence": {"showMac"}), after the Switch's.
 * Events {"kind":"presence","presence"} carry the status when only it changed. */
bool warp_core_set_presence_mac(WarpCore* core, bool showMac, char** error);
/* Backups to keep per save (0 = all). */
bool warp_core_set_keep_backups(WarpCore* core, int keep, char** error);
/* The emulator's data folder (the one with bis/ and system/ in it). */
bool warp_core_set_emulator_path(WarpCore* core, const char* path, char** error);

/* nxnative ports (1.2). setup's "nxnative": {"enabled","user","userChosen","apps":[{"name",
 * "path","appTitleId","saveTitleId","saveIdSource","savePath","cloudFolder","running",
 * "hasSave"}]}. The library's games add "macSide" ("emulator" | "nxnative"), "macName",
 * "nxnative" (the app, when one has the game), "canUseEmulator", "emulatorHasSave". */
/* Whose saves the nxnative apps hold; "" or NULL = the first linked console user. */
bool warp_core_set_nxnative_user(WarpCore* core, const char* consoleUser, char** error);
bool warp_core_set_nxnative_enabled(WarpCore* core, bool enabled, char** error);
/* Sync this game (console save title id) with the emulator instead of its nxnative app. */
bool warp_core_set_keep_in_emulator(WarpCore* core, const char* titleId, bool keep, char** error);
/* Send a Switch 2 Edition app's saves to the console (only for a console that can read
 * them: the Switch 1 version of a game crashes on them). titleId: the console save's. */
bool warp_core_set_send_upgraded(WarpCore* core, const char* titleId, bool send, char** error);
/* Override which console save an app uses; "" or NULL saveTitleId = from its control data. */
bool warp_core_set_nxnative_save_id(WarpCore* core, const char* appTitleId,
                                    const char* saveTitleId, char** error);

/* Stop syncing a save (ignored = true) or start again. Nothing is deleted. */
bool warp_core_set_ignored(WarpCore* core, const char* key, bool ignored, char** error);

/* Copy a console-only save into the emulator ("Bring to Astris"). */
bool warp_core_bring_to_emulator(WarpCore* core, const char* key, char** error);

/* Save previews: what a game's save slots hold, read from its save folder on the Mac
 * (the library's "savePath"). titleId: 16 hex, the console save's. See savePreviewJson in
 * host/save_preview.hpp: {"supported": false} for a game without a parser. */
char* warp_core_save_preview(WarpCore* core, const char* titleId, const char* folder);

/* The folder the core was created with (for "Show in Finder"). */
char* warp_core_data_dir(WarpCore* core);

void warp_free(char* s);

#ifdef __cplusplus
}
#endif

#endif
