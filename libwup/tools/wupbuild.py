#!/usr/bin/env python3
"""Build a GamePad app with libwup into a .wup.

    python3 tools/wupbuild.py APPDIR                          an app folder with an app.cfg (e.g. examples/hello)
    python3 tools/wupbuild.py SRC... --name "Title" [--author A] [--version V] [--icon icon.png]
                              [--description D] [-I dir] [-D X=Y] [-o out.wup] [--thumb] [--net]

app.cfg: key = value lines: name, author, version, description, icon (64x64 PNG; optional), sources (space separated,
relative to the folder, globs allowed, '!pattern' leaves files out, indented lines continue), include (folders),
defines, cflags (extra compiler flags), prebuild (a Python script in the folder run first, for generated sources;
.s/.S sources are assembled, .cpp ones are C++17 without exceptions or RTTI), thumb (yes/no), thumb_sources (these as Thumb -Os: smaller), deps (tools/deps.py names),
net (yes: sockets; raw: lwIP's callback API, see include/wup.h), lvgl (yes: the GUI library, include/wup_lvgl.h), kit
(yes: the Homebrew Menu's look for apps, include/wup_kit.h; brings lvgl), system (yes: the menu and Software Update:
the filesystem may be formatted, the flash slots written), https (yes: wup_https.h, BearSSL; with net = yes).
large (yes: screen 0, and with the 8-bit screen the rest of screen 1, as heap: src/malloc_large.c), printf (integer,
float or double: picolibc's printf family). Shared libraries (include/dlfcn.h): shared (yes: the folder builds a library,
<name>.so, ARM, for an app built with dl; its imports listed in libwup/build/apps/<name>/imports.txt), dl (yes: the app
loads libraries: dlopen, and a table of what it exports to them), dl_exports (more symbols for its libraries: names, or
@files of them, e.g. a library's imports.txt), libraries (folders of its libraries, built first: each is kept as
libwup/build/apps/<name>/<name>.so). files (for its package, wup-menu/tools/mkrepo.py: data beside the app, or
/path/on/the/pad=file).
SRC: .c files or directories (all their .c files). The app defines WUP_APP("Title") and main().
An app is one image linked at WUP_APP_BASE (0x80000, include/wup_ram.h): code, constants and data, then its .bss;
the rest of the app's block, to WUP_APP_END (0x253000), is its heap. Code is ARM (faster than Thumb on the ARM926;
--thumb for smaller), -O2. Send it to a pad with
    python3 tools/wuplink.py out.wup"""
import os, re, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
BASE, LIMIT = 0x080000, 0x253000                         # the app's block (include/wup_ram.h)
HEAP_MIN = 0x10000                                       # at least this much heap left after .bss
NM = paths.NM
LIB = ([os.path.join(paths.SRC, f) for f in ('wup.c', 'sys.c', 'kernel.c', 'pthread.c', 'time.c', 'atomic.c', 'gfx.c', 'fonts.c', 'wup_hw.c', 'fs.c', 'video.c', 'netfs.c', 'mem.S')]
       + [os.path.join(paths.THIRD, 'littlefs', f) for f in ('lfs.c', 'lfs_util.c')])
OBJCOPY = paths.OBJCOPY
IGNORED = ('screen8', 'text_in_big', 'text_in_app')   # (v1's memory layouts)


def build(srcs, name, out, incs=(), defs=(), thumb=False, net=False, thumb_srcs=(), cflags=(), gui=False, system=False, https=False, large=False, printf='integer', shared=False, dl=False, dl_exports=()):
    """-> the app image (shared: the library's bytes, an ELF shared object)"""
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
    if shared:                               # (app.cfg shared = yes: a library for an app built with dl = yes)
        flags += ['-fPIC', '-fvisibility=hidden']
        if thumb: sys.exit('shared = yes: ARM only')
    import deps; deps.fetch('littlefs')
    lib = [] if shared else list(LIB)
    if dl: lib += [os.path.join(paths.SRC, 'dl.c')]   # (dl = yes: dlopen; the export table: link_dl)
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
    if large:                                # (app.cfg large = yes: screen 0 is the heap too, src/malloc_large.c)
        if gui: sys.exit('large = yes is not for LVGL apps (they draw on both screens)')
        deps.fetch('tlsf')
        lib += [os.path.join(paths.SRC, 'malloc_large.c'), os.path.join(paths.THIRD, 'tlsf', 'tlsf.c')]
        flags += ['-DWUP_LARGE']
    if system: lib += [os.path.join(paths.SRC, 'menu_slot.c')]   # (wup_system.h: the menu and Software Update only)
    if https:                                # HTTPS (wup_https.h): BearSSL, built once (tools/bearssl.py), and picohttpparser
        if net is not True: sys.exit('https = yes needs net = yes')
        import bearssl; bearssl.ensure(); deps.fetch('picohttpparser')
        php = deps.DEPS['picohttpparser'][0]
        lib += [os.path.join(paths.SRC, 'https.c'), os.path.join(php, 'picohttpparser.c')]
        flags += bearssl.flags() + ['-I', php]; extra_libs = extra_libs + [bearssl.LIB]
    cxx = any(x.endswith(('.cpp', '.cc')) for x in srcs)
    if (cxx or dl) and not shared: lib += [os.path.join(paths.SRC, 'cxx.cpp')]   # (a library's C++ runtime: its app's)     # (C++: the runtime's pieces, src/cxx.cpp)
    libcxx = os.path.normpath(os.path.join(res, '..', '..', '..', 'include', 'c++', 'v1'))   # (the compiler's own headers)
    cxxflags = ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-nostdinc++',
                '-isystem', os.path.join(paths.CONFIG, 'c++'), '-isystem', libcxx]
    bdir = paths.out('apps', name.replace(' ', '_').lower())
    objs = []
    for s in lib + srcs:
        o = os.path.join(bdir, os.path.relpath(s, paths.ROOT).replace(os.sep, '_') + '.o')
        is_lib = s in lib
        third = 'littlefs' in s or 'lwip' in s or 'picohttpparser' in s or '/ext/' in s.replace(os.sep, '/')
        f = flags + (['-Wno-everything'] if third else [])
        if s in thumb_srcs: f = [x if x != '-marm' else '-mthumb' for x in f] + ['-Os']   # size over speed
        if s.endswith(('.s', '.S')): f = [x for x in f if x not in ('-nostdlib',)]
        is_cxx = s.endswith(('.cpp', '.cc'))
        if is_cxx: f = cxxflags + f
        r = subprocess.run(['clang++' if is_cxx else 'clang'] + f + ['-c', s, '-o', o], capture_output=True, text=True)
        if r.returncode: sys.exit('compile %s failed:\n%s' % (s, r.stderr[-4000:]))
        if r.stderr.strip() and not is_lib and not third: print(r.stderr.strip()[-2000:])
        objs.append(o)
    if shared:
        return link_shared(objs, bdir, name, flags)
    import deps; deps.fetch('tinf'); tinf = os.path.join(deps.DEPS['tinf'][0], 'src')   # tinf: wup_inflate
    for s in [os.path.join(paths.SRC, 'inflate.c'), os.path.join(tinf, 'tinflate.c')]:
        o = os.path.join(bdir, os.path.relpath(s, paths.ROOT).replace(os.sep, '_') + '.o')
        subprocess.run(['clang'] + flags + ['-I', tinf, '-Wno-everything', '-c', s, '-o', o], check=True); objs.append(o)
    if dl:
        lib_objs = [o for s, o in zip(lib + srcs, objs) if s in lib] + [objs[-1], objs[-2]] + sdk.libs(mode)
        return link_dl(objs + extra_libs + sdk.libs(mode), lib_objs, bdir, name, printf, flags, dl_exports)
    return link(objs + extra_libs + sdk.libs(mode), bdir, name, printf)


def link_shared(objs, bdir, name, flags):
    """a shared library (app.cfg shared = yes): an ARM ELF shared object, bound at once (no lazy binding), with a SysV
    hash table, its symbols but its API hidden; what it does not define (the C library, the runtimes) its app gives it
    (dl = yes: libwup's dlopen binds them)"""
    dso = os.path.join(bdir, 'dso_handle.c')            # (C++ static objects register their destructors with it)
    open(dso, 'w').write('__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;\n')
    subprocess.run(['clang'] + flags + ['-c', dso, '-o', dso + '.o'], check=True)
    out = os.path.join(bdir, name + '.so')
    r = subprocess.run([paths.LD, '-shared', '-z', 'now', '-z', 'max-page-size=4096', '--hash-style=sysv', '--gc-sections',
                        '--strip-all', '-soname', name + '.so', '-o', out] + objs + [dso + '.o'], capture_output=True, text=True)
    if r.returncode: sys.exit('link failed:\n' + r.stderr[-4000:])
    data = open(out, 'rb').read()
    und = sorted({l.split()[-1] for l in subprocess.run([NM, '-D', '-u', out], capture_output=True, text=True).stdout.splitlines() if l.strip()})
    open(os.path.join(bdir, 'imports.txt'), 'w').write('\n'.join(und) + '\n')
    print('%s: shared library %d KB, %d symbols from its app (%s)' % (name, len(data) >> 10, len(und), os.path.join(bdir, 'imports.txt')))
    return data


IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
def link_dl(objs, lib_objs, bdir, name, printf, flags, extra):
    """an app that loads libraries (app.cfg dl = yes): linked twice. The first link shows which of the library code's
    symbols (the C library, maths, the compiler's and C++ runtimes, libwup) it holds; with those its libraries need
    (dl_exports: pulled in too), they make its export table (wup__dl_exports, sorted: src/dl.c), the second link."""
    gen = os.path.join(bdir, 'dl_exports.c')
    def table(names):
        lines = ['typedef struct { const char *name; void *addr; } e_t;']
        lines += ['extern char x%d[] __asm__("%s");' % (i, n) for i, n in enumerate(names)]
        lines += ['const e_t wup__dl_exports[] = {'] + ['  { "%s", x%d },' % (n, i) for i, n in enumerate(names)] + ['  { 0, 0 } };']
        lines += ['const unsigned wup__dl_nexports = %d;' % len(names)]
        open(gen, 'w').write('\n'.join(lines) + '\n')
        subprocess.run(['clang'] + [f for f in flags if not f.startswith('-W')] + ['-w', '-fno-builtin', '-c', gen, '-o', gen + '.o'], check=True)
    force = ['-u' + n for n in extra]
    table([])
    link(objs + [gen + '.o'], bdir, name, printf, force, quiet=True)
    have = {}
    for l in subprocess.run([NM, os.path.join(bdir, 'app.elf')], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in 'TDBRW' and p[1].isupper(): have[p[2]] = True
    libdef = set()
    for o in lib_objs:
        for l in subprocess.run([NM, '--defined-only', '-g', o], capture_output=True, text=True).stdout.splitlines():
            p = l.split()
            if len(p) == 3: libdef.add(p[2])
    names = sorted(n for n in (libdef | set(extra)) if n in have and IDENT.match(n) and not n.startswith(('wup__', '_wup')))
    missing = [n for n in extra if n not in have]
    if missing: sys.exit('%s: dl_exports not found in the libraries: %s' % (name, ' '.join(missing)))
    table(names)
    img = link(objs + [gen + '.o'], bdir, name, printf, force)
    print('%s: %d symbols exported to its libraries' % (name, len(names)))
    return img


PRINTF = {'integer': [], 'float': ['__f_vfprintf', '__f_vfscanf'], 'double': ['__d_vfprintf', '__d_vfscanf']}


def link(objs, bdir, name, printf='integer', extra=(), quiet=False):
    """the one image: the HBRW header first, linked at BASE; its .bss after it, the heap after that. printf: picolibc's
    printf and scanf family, 'integer' (the default: no %f, smallest), 'float' or 'double' (app.cfg printf = ...)"""
    if printf not in PRINTF: sys.exit('%s: app.cfg printf = %s: integer, float or double' % (name, printf))
    defsym = ['--defsym=vfprintf=%s' % PRINTF[printf][0], '--defsym=vfscanf=%s' % PRINTF[printf][1]] if PRINTF[printf] else []
    ld = os.path.join(bdir, 'app.ld')
    open(ld, 'w').write('SECTIONS { . = 0x%x; .header : { KEEP(*(.header)) } .text : { *(.text.entry) *(.text*) } .rodata : { *(.rodata*) }'
                        ' . = ALIGN(4); .init_array : { __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; }'
                        ' .data : { *(.data*) } . = ALIGN(4); __bss_start = .; .bss : { *(.bss*) *(COMMON) } . = ALIGN(4); __bss_end = .;'
                        ' /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) } }\n' % BASE)
    elf, binf = os.path.join(bdir, 'app.elf'), os.path.join(bdir, 'app.bin')
    for out, fmt in ((elf, []), (binf, ['--oformat', 'binary'])):
        r = subprocess.run([paths.LD, '--gc-sections', '--error-limit=0', '-e', '_wup_entry', '-u', '_wup_header', '-T', ld] + list(extra) + defsym + fmt + ['-o', out] + objs,
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
    if not quiet: print('%s: image %d KB at 0x%x, .bss to 0x%x; heap %d KB' % (name, len(img) >> 10, BASE, end, (LIMIT - end) >> 10))
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
    for lib in cfg.get('libraries', '').split():             # its libraries first (shared = yes folders): their
        build_app(os.path.join(d, lib))                      # imports.txt is what its dl_exports names
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
                 cfg.get('https', 'no') == 'yes', cfg.get('large', 'no') == 'yes',
                 cfg.get('printf', 'integer'), cfg.get('shared', 'no') == 'yes', cfg.get('dl', 'no') == 'yes',
                 dl_exports(d, cfg.get('dl_exports', '')))
    icon = os.path.join(d, cfg['icon']) if cfg.get('icon') else None
    author, version = cfg.get('author', ''), cfg.get('version', '1.0')
    description = cfg.get('description', '')
    return code, name, author, version, icon, description


def dl_exports(d, spec):
    """app.cfg dl_exports: symbol names, or @files of them (one a line: a library's imports.txt)"""
    out = []
    for x in spec.split():
        if x.startswith('@'):
            f = os.path.join(d, x[1:])
            if os.path.isfile(f): out += open(f).read().split()
        else: out.append(x)
    return sorted(set(out))


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'): sys.exit(__doc__)
    opt = lambda k, d=None: a[a.index(k) + 1] if k in a else d
    multi = lambda k: [a[i + 1] for i, x in enumerate(a) if x == k]
    if len(a) == 1 and os.path.isfile(os.path.join(a[0], 'app.cfg')):        # an app folder
        code, name, author, version, icon, description = build_app(a[0]); out = None
        if code[:4] == b'\x7fELF':                                           # (shared = yes: a library, as it is)
            out = os.path.join(paths.apps_out(), name + '.so')
            open(out, 'wb').write(code)
            print('%s (%d KB)' % (out, len(code) >> 10))
            return
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
