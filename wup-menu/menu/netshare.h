/* netshare: the Homebrew Menu's client for the /net network storage (tools/netfs-server, protocol v1).
 *
 * The menu runs lwIP's raw API (app.cfg: net = raw), so this is a small state machine over tcp_pcb —
 * the same style as ftpd.c and wuplink.c — stepped from menu_poll(). Two jobs: test the configured
 * server (connect + AUTH + PING + STAT of the root) and browse a directory (OPENDIR + READDIR pages).
 * Everything is bounded: a deadline aborts a stuck connection, and the menu never waits on the network. */
#pragma once
#include <stdint.h>
#include "netfs.h"                        /* netfs_cfg_t: the settings apps read from /config/netfs */

typedef struct { char name[256]; uint64_t size; uint8_t is_dir; } ns_ent_t;

/* the test of a server: ns_begin_test() starts it; ns_step() (from menu_poll) drives it; ns_result()
   answers once ns_state() is NS_DONE or NS_FAIL (0 ok, else the errno-ish error) */
enum { NS_IDLE, NS_WORK, NS_DONE, NS_FAIL };
int  ns_state(void);
int  ns_result(void);
int  ns_op(void);                            /* which job the client runs: 0 test, 1 browse, 2 upload, 3 fetch */
int  ns_begin_test(const netfs_cfg_t *cfg);

/* browsing: ns_begin_browse() lists path (server-side, "/"-rooted); entries land in ns_entries() when
   ns_state() is NS_DONE (result 0) — up to NS_MAX_ENTRIES, directories and files together */
#define NS_MAX_ENTRIES 64
int  ns_begin_browse(const netfs_cfg_t *cfg, const char *path);
int  ns_truncated(void);                   /* the listing had more than NS_MAX_ENTRIES (first ones kept) */
const ns_ent_t *ns_entries(int *count);
const char *ns_browse_path(void);          /* the path that was listed (for ".." and child paths) */

/* upload (a --read-write server): ns_begin_upload() sends the littlefs file local to remote
   (a "/"-rooted server path); ns_upload_progress() answers 0..100 meanwhile, -1 when idle */
int  ns_begin_upload(const netfs_cfg_t *cfg, const char *local, const char *remote);
int  ns_upload_progress(void);

/* fetch a .wup from the share into WUP_HANDOFF_STAGE for the loader (its code, crc-checked):
   ns_begin_fetch() starts it; on NS_DONE the stage holds the image and ns_stage_size()/ns_stage_raw()
   are what wup_exec_staged() wants. Apps with a WUP_BIGMEM part are refused (ENOTSUP). */
int  ns_begin_fetch(const netfs_cfg_t *cfg, const char *remote);
uint32_t ns_stage_size(void);
uint32_t ns_stage_raw(void);

void ns_abort(void);                       /* drop any connection in progress */
void ns_step(void);                        /* once per menu_poll */
