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

/* The folder the core was created with (for "Show in Finder"). */
char* warp_core_data_dir(WarpCore* core);

void warp_free(char* s);

#ifdef __cplusplus
}
#endif

#endif
