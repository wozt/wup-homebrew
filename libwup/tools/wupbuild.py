#!/usr/bin/env python3
"""Build a GamePad app with libwup into a .wup.

    python3 tools/wupbuild.py APPDIR                          an app folder with an app.cfg (e.g. examples/hello)
    python3 tools/wupbuild.py SRC... --name "Title" [--author A] [--version V] [--icon icon.png]
                              [--description D] [-I dir] [-D X=Y] [-o out.wup] [--thumb] [--net]

app.cfg: key = value lines: name, author, version, description, icon (64x64 PNG; optional), sources (space separated,
relative to the folder, globs allowed, '!pattern' leaves files out, indented lines continue), include (folders),
defines, cflags (extra compiler flags), prebuild (a Python script in the folder run first, for generated sources;
.s/.S sources are assembled), thumb (yes/no), thumb_sources (these as Thumb -Os: smaller), deps (tools/deps.py names),
net (yes: sockets; raw: lwIP's callback API, see include/wup.h), lvgl (yes: the GUI library, include/wup_lvgl.h), kit
(yes: the Homebrew Menu's look for apps, include/wup_kit.h; brings lvgl), system (yes: the menu and Software Update:
the filesystem may be formatted, the flash slots written), https (yes: wup_https.h, BearSSL; with net = yes).
SRC: .c files or directories (all their .c files). The app defines WUP_APP("Title") and main().
An app is one image linked at WUP_APP_BASE (0x80000, include/wup_ram.h): code, constants and data, then its .bss;
the rest of the app's block, to WUP_APP_END (0x253000), is its heap. Code is ARM (faster than Thumb on the ARM926;
--thumb for smaller), -O2. Send it to a pad with
    python3 tools/wuplink.py out.wup"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
BASE, LIMIT = 0x080000, 0x253000                         # the app's block (include/wup_ram.h)
HEAP_MIN = 0x10000                                       # at least this much heap left after .bss
NM = paths.NM
LIB = ([os.path.join(paths.SRC, f) for f in ('wup.c', 'sys.c', 'kernel.c', 'pthread.c', 'time.c', 'atomic.c', 'gfx.c', 'fonts.c', 'wup_hw.c', 'fs.c', 'video.c', 'netfs.c', 'mem.S')]
       + [os.path.join(paths.THIRD, 'littlefs', f) for f in ('lfs.c', 'lfs_util.c')])
OBJCOPY = paths.OBJCOPY
IGNORED = ('large', 'screen8', 'text_in_big', 'text_in_app')   # (v1's memory layouts: one layout now)


def build(srcs, name, out, incs=(), defs=(), thumb=False, net=False, thumb_srcs=(), cflags=(), gui=False, system=False, https=False):
    """-> the app image"""
    subprocess.run([sys.executable, os.path.join(HERE, 'mkfont_aa.py')], check=True, stdout=subprocess.DEVNULL)
    import sdk; sdk.ensure()
    mode = 'thumb' if thumb else 'arm'
    res = subprocess.run(['clang', '-print-resource-dir'], capture_output=True, text=True).stdout.strip()
    flags = sum((['-I', i] for i in incs), []) + ['-I', paths.INCLUDE,
             *sdk.flags(mode), '-O2', '-isystem', res + '/include', '-isystem', paths.CONFIG, '-fno-common', '-ffunction-sections', '-fdata-sections',
             '-Wall', '-Wno-unused-function', '-ffile-prefix-map=%s=.' % paths.ROOT, '-I', paths.SRC, '-I', paths.THIRD, '-I', paths.GEN,
             *([] if system else ['-DFS_NO_FORMAT']), '-DLFS_NO_DEBUG',   # (only the menu may format)
             '-DLFS_NO_WARN', '-DLFS_NO_ERROR', '-DLFS_NO_ASSERT', '-DLFS_NO_INTRINSICS',
             '-D_POSIX_MONOTONIC_CLOCK=200112L']     # (libwup has it: <time.h> then names CLOCK_MONOTONIC)
    flags += ['-D' + d for d in list(defs) + os.environ.get('WUP_DEFINES', '').split()] + list(cflags)   # WUP_DEFINES: extra, for test builds
    import deps; deps.fetch('littlefs')
    lib = list(LIB)
    if net:                                  # libwup networking: lwIP + the Wi-Fi interface + sockets
        import lwip
        raw = net == 'raw'                   # raw: lwIP's callback API, polled (the menu); else lwIP's sockets, in threads
        lib += lwip.fetch(not raw) + [os.path.join(paths.SRC, f) for f in ('wup_netif.c', 'sys_arch.c', 'net.c')]
        flags += ['-DWUP_NET', '-I', os.path.join(lwip.SRC, 'include')] + ([] if raw else ['-DWUP_NET_SOCKETS'])
    extra_libs = []
    if gui:                                  # LVGL (wup_lvgl.h): the library is built once (tools/lvgl.py)
        import lvgl; lvgl.ensure()
        lib += [os.path.join(paths.SRC, 'lvgl.c'), os.path.join(paths.SRC, 'alert.c')]
        flags += lvgl.flags(); extra_libs = [lvgl.LIB]
    if system: lib += [os.path.join(paths.SRC, 'menu_slot.c')]   # (wup_system.h: the menu and Software Update only)
    if https:                                # HTTPS (wup_https.h): BearSSL, built once (tools/bearssl.py), and picohttpparser
        if net is not True: sys.exit('https = yes needs net = yes')
        import bearssl; bearssl.ensure(); deps.fetch('picohttpparser')
        php = deps.DEPS['picohttpparser'][0]
        lib += [os.path.join(paths.SRC, 'https.c'), os.path.join(php, 'picohttpparser.c')]
        flags += bearssl.flags() + ['-I', php]; extra_libs = extra_libs + [bearssl.LIB]
    bdir = paths.out('apps', name.replace(' ', '_').lower())
    objs = []
    for s in lib + srcs:
        o = os.path.join(bdir, os.path.relpath(s, paths.ROOT).replace(os.sep, '_') + '.o')
        is_lib = s in lib
        third = 'littlefs' in s or 'lwip' in s or 'picohttpparser' in s or '/ext/' in s.replace(os.sep, '/')
        f = flags + (['-Wno-everything'] if third else [])
        if s in thumb_srcs: f = [x if x != '-marm' else '-mthumb' for x in f] + ['-Os']   # size over speed
        if s.endswith(('.s', '.S')): f = [x for x in f if x not in ('-nostdlib',)]
        r = subprocess.run(['clang'] + f + ['-c', s, '-o', o], capture_output=True, text=True)
        if r.returncode: sys.exit('compile %s failed:\n%s' % (s, r.stderr[-4000:]))
        if r.stderr.strip() and not is_lib and not third: print(r.stderr.strip()[-2000:])
        objs.append(o)
    import deps; deps.fetch('tinf'); tinf = os.path.join(deps.DEPS['tinf'][0], 'src')   # tinf: wup_inflate
    for s in [os.path.join(paths.SRC, 'inflate.c'), os.path.join(tinf, 'tinflate.c')]:
        o = os.path.join(bdir, os.path.relpath(s, paths.ROOT).replace(os.sep, '_') + '.o')
        subprocess.run(['clang'] + flags + ['-I', tinf, '-Wno-everything', '-c', s, '-o', o], check=True); objs.append(o)
    return link(objs + extra_libs + sdk.libs(mode), bdir, name)


def link(objs, bdir, name):
    """the one image: the HBRW header first, linked at BASE; its .bss after it, the heap after that"""
    ld = os.path.join(bdir, 'app.ld')
    open(ld, 'w').write('SECTIONS { . = 0x%x; .header : { KEEP(*(.header)) } .text : { *(.text.entry) *(.text*) } .rodata : { *(.rodata*) }'
                        ' .data : { *(.data*) } . = ALIGN(4); __bss_start = .; .bss : { *(.bss*) *(COMMON) } . = ALIGN(4); __bss_end = .;'
                        ' /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) } }\n' % BASE)
    elf, binf = os.path.join(bdir, 'app.elf'), os.path.join(bdir, 'app.bin')
    for out, fmt in ((elf, []), (binf, ['--oformat', 'binary'])):
        r = subprocess.run([paths.LD, '--gc-sections', '--error-limit=0', '-e', '_wup_entry', '-u', '_wup_header', '-T', ld] + fmt + ['-o', out] + objs,
                           capture_output=True, text=True)
        if r.returncode: sys.exit('link failed:\n' + r.stderr[-4000:])
    syms = {}
    for l in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    img = bytearray(open(binf, 'rb').read())[:syms['__bss_start'] - BASE]
    end = syms['__bss_end']
    if end > LIMIT - HEAP_MIN:
        sys.exit('%s: too big: .bss reaches 0x%x; the app block ends at 0x%x' % (name, end, LIMIT))
    if img[:4] != b'HBRW': sys.exit('no WUP_APP(...) header in the sources')
    struct.pack_into('<I', img, 8, len(img))
    print('%s: image %d KB at 0x%x, .bss to 0x%x; heap %d KB' % (name, len(img) >> 10, BASE, end, (LIMIT - end) >> 10))
    return bytes(img)


def read_cfg(d):
    cfg, last = {}, None
    for raw in open(os.path.join(d, 'app.cfg')):
        line = raw.split('#', 1)[0]
        if line[:1] in (' ', '\t') and line.strip() and last:     # an indented line continues the previous key
            cfg[last] += ' ' + line.strip(); continue
        line = line.strip()
        if '=' in line:
            k, v = line.split('=', 1); last = k.strip(); cfg[last] = v.strip()
    return cfg


def build_app(d):
    """an app folder (with an app.cfg) -> (image, name, author, version, icon path or None, description)"""
    import deps, glob
    d = os.path.abspath(d); cfg = read_cfg(d)
    for dep in cfg.get('deps', '').split():                  # third-party code fetched by tools/deps.py (before the globs)
        deps.fetch(dep)
    if cfg.get('prebuild'):                                  # generated sources (before the globs too)
        subprocess.run([sys.executable, os.path.join(d, cfg['prebuild'])], check=True, cwd=d)
    srcs = sum((sorted(glob.glob(os.path.join(d, x))) for x in cfg['sources'].split() if not x.startswith('!')), []) \
        if 'sources' in cfg else sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith('.c'))
    drop = set(sum((glob.glob(os.path.join(d, x[1:])) for x in cfg.get('sources', '').split() if x.startswith('!')), []))
    srcs = [os.path.normpath(s) for s in srcs if s not in drop]   # '!pattern' leaves files out
    kit = cfg.get('kit', 'no') == 'yes'
    if kit: srcs.append(os.path.join(paths.SRC, 'kit.c'))        # the menu's look for apps (include/wup_kit.h)
    name = cfg.get('name') or os.path.basename(d)
    for k in IGNORED:
        if k in cfg: print('%s: app.cfg %s: ignored (one memory layout now)' % (name, k))
    pats = cfg.get('thumb_sources', '').split()               # '!pattern' excludes (hot code stays ARM)
    thumb_srcs = set(sum((glob.glob(os.path.join(d, x)) for x in pats if not x.startswith('!')), []))
    thumb_srcs -= set(sum((glob.glob(os.path.join(d, x[1:])) for x in pats if x.startswith('!')), []))
    thumb_srcs = {os.path.normpath(x) for x in thumb_srcs}
    code = build(srcs, name, None, [os.path.join(d, x) for x in cfg.get('include', '').split()], cfg.get('defines', '').split(),
                 cfg.get('thumb', 'no') == 'yes', {'yes': True, 'raw': 'raw'}.get(cfg.get('net', 'no'), False), thumb_srcs,
                 cfg.get('cflags', '').split(), kit or cfg.get('lvgl', 'no') == 'yes', cfg.get('system', 'no') == 'yes',
                 cfg.get('https', 'no') == 'yes')
    icon = os.path.join(d, cfg['icon']) if cfg.get('icon') else None
    author, version = cfg.get('author', ''), cfg.get('version', '1.0')
    description = cfg.get('description', '')
    return code, name, author, version, icon, description


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'): sys.exit(__doc__)
    opt = lambda k, d=None: a[a.index(k) + 1] if k in a else d
    multi = lambda k: [a[i + 1] for i, x in enumerate(a) if x == k]
    if len(a) == 1 and os.path.isfile(os.path.join(a[0], 'app.cfg')):        # an app folder
        code, name, author, version, icon, description = build_app(a[0]); out = None
    else:
        skip = {i + 1 for i, x in enumerate(a) if x in ('--name', '--author', '--version', '--icon', '--description', '-o', '-I', '-D')}
        srcs = []
        for i, x in enumerate(a):
            if i in skip or x.startswith('-'): continue
            if os.path.isdir(x): srcs += sorted(os.path.join(x, f) for f in os.listdir(x) if f.endswith('.c'))
            else: srcs.append(x)
        name = opt('--name') or os.path.splitext(os.path.basename(srcs[0].rstrip('/')))[0]
        code = build([os.path.abspath(s) for s in srcs], name, None, multi('-I'), multi('-D'), '--thumb' in a, '--net' in a)
        icon, author, version, out = opt('--icon'), opt('--author', ''), opt('--version', '1.0'), opt('-o')
        description = opt('--description', '')
    import wupack
    data = wupack.pack(code, name, author, version, icon, description)
    out = out or os.path.join(paths.apps_out(), name.replace(' ', '_').lower() + '.wup')
    open(out, 'wb').write(data)
    print('%s (%d KB)' % (out, len(data) >> 10))


if __name__ == '__main__':
    main()
