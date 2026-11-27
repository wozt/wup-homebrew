/* release: wup-homebrew's updates, from the project's GitHub Releases (Settings > System > Software Update).
 *
 * The newest release (RELEASES_URL) names its version in tag_name ("v2.0.1") and carries one asset, RELEASE_ASSET: a
 * zip of kernel.bin (the kernel's slot as stored: a 4 KB header sector {"GPST", size, crc32}, then the kernel) and
 * homebrew_menu.bin (the menu's slot as stored: a header sector {"WMNU", ...}, then the deflated menu), made by
 * wup-menu/tools/mkrelease.py. Installing writes the kernel's slot, then the menu's, each header last (libwup
 * wup_system.h): an update cut short leaves no whole kernel or no whole menu, and A at power-on then starts stock,
 * from where Software Update runs again. */
#pragma once
#include <stdint.h>

#ifndef RELEASES_URL
#define RELEASES_URL "https://api.github.com/repos/wozt/wup-homebrew/releases/latest"   /* the fork's own: an upstream release has no Network Storage */
#endif
#define RELEASE_ASSET "wup-homebrew-update.zip"

typedef struct {
    char version[24];            /* the tag, without a leading "v" */
    char url[320];               /* the asset's download ("": the release has none: nothing to install) */
    uint32_t size_kb;
} release_t;

int release_check(release_t *r);                                   /* 0, or < 0 (catalog_why) */
int release_install(const release_t *r, void (*progress)(int));    /* 0, or < 0 (catalog_why) */
