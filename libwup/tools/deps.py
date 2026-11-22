#!/usr/bin/env python3
"""Third-party code the builds use, fetched once into the tree (git-ignored, each under its own license).

    python3 tools/deps.py            fetch everything missing
The build scripts call fetch() themselves, so this is only needed to get everything up front."""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths

DEPS = {   # name: (folder, git URL, tag/branch, license[, folders: a sparse checkout of only these])
    'littlefs': (os.path.join(paths.THIRD, 'littlefs'), 'https://github.com/littlefs-project/littlefs.git', 'v2.9.3', 'BSD-3-Clause'),
    'lwip': (os.path.join(paths.THIRD, 'lwip'), 'https://github.com/lwip-tcpip/lwip.git', 'STABLE-2_2_0_RELEASE', 'BSD-3-Clause'),
    # the C library and the compiler's runtime for everything on the pad (tools/sdk.py builds them)
    'picolibc': (os.path.join(paths.THIRD, 'picolibc'), 'https://github.com/picolibc/picolibc.git', '1.8.10', 'BSD-3-Clause and others (COPYING.picolibc)'),
    # inflate for the loader: app images are stored deflated (tools/wupack.py)
    'tinf': (os.path.join(paths.THIRD, 'tinf'), 'https://github.com/jibsen/tinf.git', 'v1.2.1', 'Zlib'),
    # streaming LZ compression for the menu's WUP_BIGMEM blob (src/big.c reads it from flash in chunks)
    'heatshrink': (os.path.join(paths.THIRD, 'heatshrink'), 'https://github.com/atomicobject/heatshrink.git', 'v0.4.1', 'Boost'),
    # the GUI: the menu and apps (tools/lvgl.py builds it)
    'lvgl': (os.path.join(paths.THIRD, 'lvgl'), 'https://github.com/lvgl/lvgl.git', 'v9.2.2', 'MIT'),
    # HTTPS for apps (src/https.c, app.cfg: https = yes): TLS, HTTP responses; JSON and zip for the apps that need them
    'bearssl': (os.path.join(paths.THIRD, 'bearssl'), 'https://www.bearssl.org/git/BearSSL', 'v0.6', 'MIT'),
    'picohttpparser': (os.path.join(paths.THIRD, 'picohttpparser'), 'https://github.com/h2o/picohttpparser.git', 'dd573d8fa9264c646a400e96ce9f07dea6f156e4', 'MIT or Perl'),
    'jsmn': (os.path.join(paths.THIRD, 'jsmn'), 'https://github.com/zserge/jsmn.git', 'v1.1.0', 'MIT'),
    # WUP Boy (its own repository, github.com/ravensclawx/wup-boy): the emulator core and its sound (examples/sdl2/minigb_apu: no SDL in it)
    'peanut-gb': (os.path.join(paths.THIRD, 'peanut-gb'), 'https://github.com/deltabeard/Peanut-GB.git', '33f053b9e70297ff0bab53da527c19126384d911', 'MIT'),
    'compiler-rt': (os.path.join(paths.THIRD, 'llvm'), 'https://github.com/llvm/llvm-project.git', 'llvmorg-19.1.7', 'Apache-2.0 WITH LLVM-exception', ['compiler-rt/lib/builtins']),
}


def fetch(*names):
    for n in names or DEPS:
        d, url, ref, _ = DEPS[n][:4]
        sparse = DEPS[n][4] if len(DEPS[n]) > 4 else None
        if not os.path.isdir(d) or not os.listdir(d):         # (a submodule's folder exists, empty)
            print('fetching %s (%s %s)' % (n, url, ref), flush=True)
            if sparse:                                                       # only some folders of a big repository
                subprocess.run(['git', 'clone', '-q', '--depth', '1', '--filter=blob:none', '--sparse', '--branch', ref, url, d], check=True)
                subprocess.run(['git', '-C', d, 'sparse-checkout', 'set'] + sparse, check=True)
            elif len(ref) == 40 and all(c in '0123456789abcdef' for c in ref):      # a pinned commit
                subprocess.run(['git', 'init', '-q', d], check=True)
                subprocess.run(['git', '-C', d, 'fetch', '-q', '--depth', '1', url, ref], check=True)
                subprocess.run(['git', '-C', d, 'checkout', '-q', 'FETCH_HEAD'], check=True)
            else:
                subprocess.run(['git', 'clone', '-q', '--depth', '1', '--branch', ref, url, d], check=True)


if __name__ == '__main__':
    fetch()
    for n, (d, url, ref, lic, *_) in DEPS.items(): print('%-10s %-8s %-22s %s' % (n, lic, ref, os.path.relpath(d, paths.ROOT)))
