/* libwup's system layer under the C library (picolibc): the heap, standard output, files on the pad's
 * filesystem (littlefs) and leaving the app. With these, the whole of <stdio.h>, <stdlib.h>, <string.h>... works
 * as anywhere: malloc, printf (to the log), fopen("save.dat", "wb") (relative paths are in the app's folder). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wup.h"
#include "netfs.h"

/* ---- the heap: WUP_BIGMEM below the screen, after a large app's .bss. An app without the RGB565 screen (app.cfg:
   screen8 = yes) may have .bss past where that screen would be: its heap is what is left of WUP_BIGMEM after it */
extern uint8_t __bss2_end[];
static uintptr_t brk_;
void *sbrk(ptrdiff_t n) {
    uintptr_t lo = (uintptr_t)WUP_BIGMEM, hi = lo + WUP_BIGMEM_SCREEN, e = (uintptr_t)__bss2_end;
    if (e > hi && e <= lo + WUP_BIGMEM_SIZE) hi = lo + WUP_BIGMEM_SIZE;
    if (!brk_) brk_ = e > lo && e <= hi ? (e + 31) & ~(uintptr_t)31 : lo;
    if ((n > 0 && (uintptr_t)n > hi - brk_) || (n < 0 && (uintptr_t)-n > brk_ - lo)) { errno = ENOMEM; return (void *)-1; }
    void *p = (void *)brk_; brk_ += (uintptr_t)n;
    return p;
}

/* ---- stdout and stderr: the log (the UART, and UDP port 28803 with networking up), a line at a time */
static int log_put(char c, FILE *f) {
    static char line[160]; static int n;
    (void)f;
    line[n++] = c;
    if (c == '\n' || n == (int)sizeof line - 1) { line[n] = 0; wup_log(line); n = 0; }
    return (unsigned char)c;
}
static FILE log_ = FDEV_SETUP_STREAM(log_put, NULL, NULL, _FDEV_SETUP_WRITE);
FILE *const stdin = &log_;
FILE *const stdout = &log_;
FILE *const stderr = &log_;

/* ---- files. Descriptors from FD_FILES up; below are sockets (net.c), which close() hands back to it.
   Each file slot knows its backend: littlefs (the pad's flash) or netfs (/net, the network share). */
#define FD_FILES 32
#define NFILES   8
static struct { int used, net; union { lfs_file_t lf; void *nf; } u; } files[NFILES];
int (*wup__sock_close)(int fd);                    /* descriptors below FD_FILES are sockets (net.c) */
ssize_t (*wup__sock_read)(int, void *, size_t), (*wup__sock_write)(int, const void *, size_t);
static int lfs_fail(int e) {
    errno = e == LFS_ERR_NOENT ? ENOENT : e == LFS_ERR_EXIST ? EEXIST : e == LFS_ERR_NOSPC ? ENOSPC : e == LFS_ERR_ISDIR ? EISDIR
          : e == LFS_ERR_NOTDIR ? ENOTDIR : e == LFS_ERR_NOMEM ? ENOMEM : e == LFS_ERR_NOTEMPTY ? ENOTEMPTY : e == LFS_ERR_INVAL ? EINVAL : EIO;
    return -1;
}
static const char *full(const char *p, char *out, size_t n) {       /* "x" -> "<the app's folder>/x" */
    if (p[0] == '/') return p;
    snprintf(out, n, "%s/%s", wup_app_dir(), p);
    return out;
}
static int slot(int fd) {
    if (fd < FD_FILES || fd >= FD_FILES + NFILES || !files[fd - FD_FILES].used) { errno = EBADF; return -1; }
    return fd - FD_FILES;
}
int open(const char *path, int flags, ...) {
    char b[192]; int i = 0, lf, acc = flags & O_ACCMODE;
    if (!wup_fs_ok()) { errno = EIO; return -1; }
    const char *rp = full(path, b, sizeof b);
    while (i < NFILES && files[i].used) i++;
    if (i == NFILES) { errno = EMFILE; return -1; }
    if (netfs_path(rp)) {                             /* the network share: read-only for now */
        if (acc != O_RDONLY) { errno = EROFS; return -1; }
        void *h;
        if (netfs_open(rp, 0, &h)) { errno = netfs_errno(); return -1; }
        files[i].used = 1; files[i].net = 1; files[i].u.nf = h;
        return FD_FILES + i;
    }
    lf = (acc == O_RDONLY ? LFS_O_RDONLY : acc == O_WRONLY ? LFS_O_WRONLY : LFS_O_RDWR) | (flags & O_CREAT ? LFS_O_CREAT : 0)
       | (flags & O_TRUNC ? LFS_O_TRUNC : 0) | (flags & O_APPEND ? LFS_O_APPEND : 0) | (flags & O_EXCL ? LFS_O_EXCL : 0);
    int r = lfs_file_open(&fs_lfs, &files[i].u.lf, rp, lf);
    if (r) return lfs_fail(r);
    files[i].used = 1; files[i].net = 0;
    return FD_FILES + i;
}
int close(int fd) {
    if (fd < FD_FILES) { if (wup__sock_close) return wup__sock_close(fd); errno = EBADF; return -1; }
    int i = slot(fd); if (i < 0) return -1;
    if (files[i].net) { netfs_close(files[i].u.nf); files[i].used = 0; return 0; }
    int r = lfs_file_close(&fs_lfs, &files[i].u.lf);
    files[i].used = 0;
    return r ? lfs_fail(r) : 0;
}
ssize_t read(int fd, void *buf, size_t n) {
    if (fd >= 3 && fd < FD_FILES && wup__sock_read) return wup__sock_read(fd, buf, n);
    int i = slot(fd); if (i < 0) return -1;
    if (files[i].net) { int r = netfs_read(files[i].u.nf, buf, n); return r < 0 ? (errno = netfs_errno(), -1) : r; }
    lfs_ssize_t r = lfs_file_read(&fs_lfs, &files[i].u.lf, buf, n);
    return r < 0 ? lfs_fail(r) : r;
}
ssize_t write(int fd, const void *buf, size_t n) {
    if (fd >= 3 && fd < FD_FILES && wup__sock_write) return wup__sock_write(fd, buf, n);
    int i = slot(fd); if (i < 0) return -1;
    if (files[i].net) { errno = EROFS; return -1; }
    lfs_ssize_t r = lfs_file_write(&fs_lfs, &files[i].u.lf, buf, n);
    return r < 0 ? lfs_fail(r) : r;
}
off_t lseek(int fd, off_t off, int whence) {
    int i = slot(fd); if (i < 0) return -1;
    if (files[i].net) {
        int r = netfs_seek(files[i].u.nf, (int64_t)off, whence);
        if (r < 0) { errno = netfs_errno(); return -1; }
        return (off_t)netfs_tell(files[i].u.nf);
    }
    lfs_soff_t r = lfs_file_seek(&fs_lfs, &files[i].u.lf, (lfs_soff_t)off, whence == SEEK_SET ? LFS_SEEK_SET : whence == SEEK_CUR ? LFS_SEEK_CUR : LFS_SEEK_END);
    return r < 0 ? lfs_fail(r) : r;
}
static void fill(struct stat *st, int dir, uint32_t size) {
    memset(st, 0, sizeof *st);
    st->st_mode = dir ? S_IFDIR | 0755 : S_IFREG | 0644; st->st_size = (off_t)size; st->st_nlink = 1;
}
static void fill64(struct stat *st, int dir, uint64_t size) {
    memset(st, 0, sizeof *st);
    st->st_mode = dir ? S_IFDIR | 0755 : S_IFREG | 0444; st->st_size = (off_t)size; st->st_nlink = 1;
}
int stat(const char *path, struct stat *st) {
    char b[192]; struct lfs_info i;
    const char *rp = full(path, b, sizeof b);
    if (netfs_path(rp)) {
        uint64_t sz; int dir;
        if (netfs_stat(rp, &sz, &dir)) { errno = netfs_errno(); return -1; }
        fill64(st, dir, sz);
        return 0;
    }
    int r = lfs_stat(&fs_lfs, rp, &i);
    if (r) return lfs_fail(r);
    fill(st, i.type == LFS_TYPE_DIR, i.size);
    return 0;
}
int fstat(int fd, struct stat *st) {
    if (fd < FD_FILES) { memset(st, 0, sizeof *st); st->st_mode = S_IFCHR; return 0; }
    int i = slot(fd); if (i < 0) return -1;
    if (files[i].net) {
        uint64_t sz; int dir;
        if (netfs_fstat(files[i].u.nf, &sz, &dir)) { errno = netfs_errno(); return -1; }
        fill64(st, dir, sz);
        return 0;
    }
    fill(st, 0, (uint32_t)lfs_file_size(&fs_lfs, &files[i].u.lf));
    return 0;
}
int isatty(int fd) { return fd < 3; }
static int net_ro(const char *path) { if (netfs_path(path)) { errno = EROFS; return 1; } return 0; }
int unlink(const char *path) { char b[192]; const char *rp = full(path, b, sizeof b); if (net_ro(rp)) return -1; int r = lfs_remove(&fs_lfs, rp); return r ? lfs_fail(r) : 0; }
int rmdir(const char *path) { return unlink(path); }
int mkdir(const char *path, mode_t mode) { char b[192]; const char *rp = full(path, b, sizeof b); (void)mode; if (net_ro(rp)) return -1; int r = lfs_mkdir(&fs_lfs, rp); return r ? lfs_fail(r) : 0; }
int rename(const char *from, const char *to) {
    char a[192], b[192];
    const char *ra = full(from, a, sizeof a), *rb = full(to, b, sizeof b);
    if (net_ro(ra) || net_ro(rb)) return -1;
    int r = lfs_rename(&fs_lfs, ra, rb);
    return r ? lfs_fail(r) : 0;
}
int getpid(void) { return 1; }
#include <signal.h>                                /* no signals on the pad: asking for one is fine, none ever comes */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) { (void)sig; (void)act; if (old) memset(old, 0, sizeof *old); return 0; }
int kill(int pid, int sig) { (void)pid; _exit(128 + sig); }

/* ---- folders: opendir, readdir, closedir */
#include <dirent.h>
#include <stdlib.h>
struct wup__dir { int net; union { lfs_dir_t d; void *nd; } u; struct dirent e; };
DIR *opendir(const char *path) {
    char b[192];
    const char *rp = full(path, b, sizeof b);
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return 0; }
    if (netfs_path(rp)) {
        void *nd = netfs_opendir(rp);
        if (!nd) { free(d); errno = netfs_errno(); return 0; }
        d->net = 1; d->u.nd = nd;
        return d;
    }
    d->net = 0;
    int r = lfs_dir_open(&fs_lfs, &d->u.d, rp);
    if (r) { free(d); lfs_fail(r); return 0; }
    return d;
}
struct dirent *readdir(DIR *d) {
    if (d->net) {
        uint64_t sz; int dir;
        int r = netfs_readdir(d->u.nd, d->e.d_name, sizeof d->e.d_name, &sz, &dir);
        if (r <= 0) { if (r < 0) errno = netfs_errno(); return 0; }
        d->e.d_ino = 1; d->e.d_type = dir ? DT_DIR : DT_REG;
        return &d->e;
    }
    struct lfs_info i;
    for (;;) {
        if (lfs_dir_read(&fs_lfs, &d->u.d, &i) <= 0) return 0;
        if (strcmp(i.name, ".") && strcmp(i.name, "..")) break;
    }
    d->e.d_ino = 1; d->e.d_type = i.type == LFS_TYPE_DIR ? DT_DIR : DT_REG;
    strncpy(d->e.d_name, i.name, sizeof d->e.d_name - 1); d->e.d_name[sizeof d->e.d_name - 1] = 0;
    return &d->e;
}
int closedir(DIR *d) {
    if (d->net) netfs_closedir(d->u.nd); else lfs_dir_close(&fs_lfs, &d->u.d);
    free(d); return 0;
}
