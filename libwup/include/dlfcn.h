/* dlfcn: shared libraries at run time (libwup src/dl.c), the POSIX interface. A library is an ARM ELF shared object
 * built by wupbuild (app.cfg shared = yes); an app that loads them is built with dl = yes, which gives it a table of
 * what it exports to them (its C library, maths, compiler and C++ runtimes, libwup: wupbuild). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   1                    /* (every library is bound at once: these two are the same) */
#define RTLD_NOW    2
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void *)0)         /* dlsym: the app's own exports */

void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
/* (libwup's own) the loaded library an address is in, and the offset in it: for a crash report; NULL: none */
const char *wup_dl_where(const void *addr, unsigned int *offset);

#ifdef __cplusplus
}
#endif
