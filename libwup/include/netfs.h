/* netfs: the GamePad's network filesystem — a read-only view of a folder on a computer, mounted at /net.
 *
 * The Homebrew Menu stores the connection settings in the pad's filesystem (/config/netfs, littlefs); every
 * app reads them when it first touches /net. The wire protocol is a small framed TCP protocol (tools/netfs-server,
 * PROTOCOL.md); this header holds what both sides share: the settings record and the client API used by sys.c.
 *
 * The client needs sockets (app.cfg: net = yes). Without them every /net operation fails cleanly. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NETFS_MAGIC   0x3143464eu               /* "NFC1" */
#define NETFS_PATH    "/config/netfs"           /* the menu writes it; apps read it (littlefs, always local) */
#define NETFS_STATUS_PATH "/config/netfs_status"/* the last connection result, written by an app for the menu */
#define NETFS_IP_LEN  16
#define NETFS_TOKEN_LEN 24
#define NETFS_PORT_DEF 8734

typedef struct {
    uint32_t magic;                             /* NETFS_MAGIC */
    uint8_t  on;                                /* network storage enabled */
    uint8_t  pad[3];
    char     ip[NETFS_IP_LEN];                  /* server address, dotted decimal */
    uint16_t port;                              /* server port (0 = default) */
    uint8_t  token_len;                         /* bytes used of token (0 = none) */
    uint8_t  pad2;
    char     token[NETFS_TOKEN_LEN];            /* the shared secret (light authentication, LAN only) */
    uint8_t  cache_kb;                          /* read cache size; 0 = 32, 255 = off */
} netfs_cfg_t;
_Static_assert(sizeof(netfs_cfg_t) <= 64, "the settings record stays small");

/* the last connection result (an app writes it, the menu shows it) */
typedef struct {
    uint32_t magic;                             /* "NFSO" */
    int32_t  result;                            /* 0 = connected, < 0 = the error */
    int32_t  error;                             /* errno when result < 0 */
} netfs_status_t;

/* ---- the client (src/netfs.c). Compiled into every app; without sockets the calls just fail. ---- */
int  netfs_path(const char *p);                 /* 1: p is "/net" or under it (absolute) */
int  netfs_open(const char *path, int flags, void **out);   /* a remote handle; < 0 errno-style (-1) */
int  netfs_read(void *h, void *buf, size_t n);             /* at the handle's position */
int  netfs_seek(void *h, int64_t off, int whence);         /* SEEK_SET/CUR/END; < 0 errno-style */
int  netfs_size(void *h, uint64_t *size);
int64_t netfs_tell(void *h);                        /* the handle's position, or < 0 errno-style */
int  netfs_close(void *h);
int  netfs_fstat(void *h, uint64_t *size, int *is_dir);
int  netfs_stat(const char *path, uint64_t *size, int *is_dir);   /* < 0 errno-style */
void *netfs_opendir(const char *path);          /* or NULL */
int  netfs_readdir(void *d, char *name, size_t n, uint64_t *size, int *is_dir);  /* 1 entry, 0 end, < 0 error */
void netfs_closedir(void *d);
int  netfs_errno(void);                         /* the errno of the last failed call */
int  netfs_connected(void);                     /* the link is up */
void netfs_close_all(void);                     /* drop the connection and every handle (wup__net_exit) */
