#!/usr/bin/env python3
"""Build a GamePad app with libwup into a .wup.

    python3 tools/wupbuild.py APPDIR                          an app folder with an app.cfg (e.g. examples/hello)
    python3 tools/wupbuild.py SRC... --name "Title" [--author A] [--version V] [--icon icon.png]
                              [--description D] [-I dir] [-D X=Y] [-o out.wup] [--thumb] [--net] [--large]

app.cfg: key = value lines: name, author, version, description, icon (64x64 PNG; optional), sources (space separated, relative to the
folder, globs allowed, '!pattern' leaves files out, indented lines continue), include (folders), defines, cflags (extra compiler flags), prebuild (a
Python script in the folder run first, for generated sources; .s/.S sources are assembled), screen8 (yes: a large
app that never uses the RGB565 screen; its .bss may fill WUP_BIGMEM), text_in_big (code that does not fit: sources,
or 'lvgl' / 'bearssl', whose code and constants go in WUP_BIGMEM, loaded from the .wup by libwup before main()), text_in_app (sources whose code goes in the app
area, when WUP_SPARE is full), thumb (yes/no), thumb_sources (these
as Thumb -Os: smaller), deps (tools/deps.py names), net (yes: sockets; raw: lwIP's callback API, see include/wup.h), lvgl (yes: the GUI
library, include/wup_lvgl.h), kit (yes: the Homebrew Menu's look for apps, include/wup_kit.h; brings lvgl), system (yes: the menu: it may format the filesystem), https (yes: wup_https.h, BearSSL; with net = yes), large (yes/no/auto:
code in WUP_SPARE (368 KB), constants and stack at 0x350000, .bss in WUP_BIGMEM below the screen (heap after
it: wup_bigmem_free); auto = only when it does not fit).
SRC: .c files or directories (all their .c files). The app defines WUP_APP("Title") and main().
Code is ARM (faster than Thumb on the ARM926; --thumb for smaller), -O2, linked for 0x350000; code,
data, .bss and libwup's 24 KB stack must fit 0x350000..0x380000. Send it with
    python3 tools/wuplink.py out.wup"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
BASE, LIMIT = 0x350000, 0x380000
NM = paths.NM
LIB = ([os.path.join(paths.SRC, f) for f in ('wup.c', 'sys.c', 'kernel.c', 'pthread.c', 'time.c', 'atomic.c', 'gfx.c', 'fonts.c', 'mmu.c', 'wup_hw.c', 'fs.c', 'netfs.c', 'mem.S')]
       + [os.path.join(paths.THIRD, 'littlefs', f) for f in ('lfs.c', 'lfs_util.c')])


SPARE, SPARE_END = 0x0da000, 0x136000
BIG, BIG_SCREEN, BIG_END = 0x190000, 0x221400, 0x2fa400   # WUP_BIGMEM, the RGB565 screen in it, its end
OBJCOPY = paths.OBJCOPY


def build(srcs, name, out, incs=(), defs=(), thumb=False, net=False, large=None, thumb_srcs=(), cflags=(), screen8=False, app_text=(), gui=False, system=False, big_text=(), https=False):
    """large: True / False / None = only if it does not fit the app area"""
    subprocess.run([sys.executable, os.path.join(HERE, 'mkfont_aa.py')], check=True, stdout=subprocess.DEVNULL)
    import sdk; sdk.ensure()
    mode = 'thumb' if thumb else 'arm'
    res = subprocess.run(['clang', '-print-resource-dir'], capture_output=True, text=True).stdout.strip()
    flags = sum((['-I', i] for i in incs), []) + ['-I', paths.INCLUDE,
             *sdk.flags(mode), '-O2', '-isystem', res + '/include', '-isystem', paths.CONFIG, '-fno-common', '-ffunction-sections', '-fdata-sections',
             '-Wall', '-Wno-unused-function', '-ffile-prefix-map=%s=.' % paths.ROOT, '-I', paths.SRC, '-I', paths.THIRD, '-I', paths.GEN,
             '-DFS_BOUNCE=0x34e000u', *([] if system else ['-DFS_NO_FORMAT']), '-DLFS_NO_DEBUG',   # (only the menu may format)
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
        if r.returncode:
            if os.environ.get('WUP_DEBUG_CMD'): print('FAILED CMD:', ' '.join(['clang'] + f + ['-c', s, '-o', o]))
            sys.exit('compile %s failed:\n%s' % (s, r.stderr[-4000:]))
        if r.stderr.strip() and not is_lib and not third: print(r.stderr.strip()[-2000:])
        objs.append(o)
    in_app = [o for s, o in zip(lib + srcs, objs) if s in app_text]
    in_big = ['*%s' % os.path.basename(o) for s, o in zip(lib + srcs, objs)
              if s in big_text or os.path.splitext(os.path.basename(s))[0] in big_text]   # a module's name works too
    if 'lvgl' in big_text and gui: in_big.append('*liblvgl.a:*')
    if 'bearssl' in big_text and https: in_big.append('*libbearssl.a:*')
    if 'lwip' in big_text: in_big += ['*%s' % os.path.basename(o) for s, o in zip(lib + srcs, objs) if '/lwip/' in s.replace(os.sep, '/')]   # all of lwIP in WUP_BIGMEM
    import deps; deps.fetch('tinf'); tinf = os.path.join(deps.DEPS['tinf'][0], 'src')   # tinf: wup_inflate, and big.c's
    hs = ''
    if in_big: deps.fetch('heatshrink'); hs = deps.DEPS['heatshrink'][0]             # big.c inflates the menu blob with it
    for s in [os.path.join(paths.SRC, 'inflate.c'), os.path.join(tinf, 'tinflate.c')] + \
             ([os.path.join(paths.SRC, 'big.c'), os.path.join(hs, 'heatshrink_decoder.c')] if in_big else []):
        o = os.path.join(bdir, os.path.relpath(s, paths.ROOT).replace(os.sep, '_') + '.o')
        subprocess.run(['clang'] + flags + ['-I', tinf] + (['-I', hs] if hs else []) + ['-Wno-everything', '-c', s, '-o', o], check=True); objs.append(o)
    if in_big and large is None: large = True        # (code in WUP_BIGMEM: the large layout has it)
    tries = [(False, 0)] if large is False else [] if large else [(False, 0)]
    if large is not False: tries += [(True, 0), (True, 1)]       # constants in the app area, else with the code
    for i, (big, net_spare) in enumerate(tries):
        r = link(objs + extra_libs + sdk.libs(mode), bdir, big, name, final=i == len(tries) - 1, net_spare=net_spare, screen8=screen8, in_app=in_app, in_big=in_big)
        if r: return r
    sys.exit('too big')


def link(objs, bdir, large, name, final=True, net_spare=0, screen8=False, in_app=(), in_big=()):
    """one image: the HBRW header + code at 0x350000; large: + a second segment at WUP_SPARE. screen8: the app never
    uses the RGB565 screen (wup_screen), so .bss may fill WUP_BIGMEM; in_app: objects whose code goes in the app area"""
    ld = os.path.join(bdir, 'app.ld')
    discard = ' /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }'
    if not large:
        script = ('SECTIONS { . = 0x%x; .header : { KEEP(*(.header)) } .text : { *(.text.entry) *(.text*) } .rodata : { *(.rodata*) }'
                  ' .data : { *(.data*) } . = ALIGN(4); __bss_start = .; .bss : { *(.bss*) *(COMMON) } . = ALIGN(4);'
                  ' __bss_end = .; __bss2_start = .; __bss2_end = .; .wup_lvglbuf (NOLOAD) : { *(.wup_lvglbuf*) }%s }\n' % (BASE, discard))
    else:
        # large: code in WUP_SPARE; header, data, constants (or, variant 1, only header and data) and the
        # stack at 0x350000; all other .bss in WUP_BIGMEM below the screen, where the heap then starts
        ro_app = '' if net_spare else ' *(.rodata*)'
        ro_spare = ' *(.rodata*)' if net_spare else ''
        app_text = ''.join(' *%s(.text*)' % os.path.basename(o) for o in in_app)
        big_end = BIG_END if screen8 else BIG_SCREEN
        script = ('MEMORY { app (rwx) : ORIGIN = 0x%x, LENGTH = 0x%x  spare (rwx) : ORIGIN = 0x%x, LENGTH = 0x%x'
                  '  big (rwx) : ORIGIN = 0x%x, LENGTH = 0x%x }\n'
                  'SECTIONS { .header : { KEEP(*(.header)) } > app'
                  '  .bigtext : { . = ALIGN(32); __big_start = .;%s . = ALIGN(32); __big_end = .; } > big'
                  '  .entry : { *(.text.entry)%s } > app'
                  '  .data : { *(.data*)%s . = ALIGN(4); __seg1_end = .; } > app'
                  '  .stack (NOLOAD) : { __bss_start = .; *(.bss.stack) . = ALIGN(4); __bss_end = .; } > app'
                  '  .text : { *(.text*)%s . = ALIGN(4); __seg2_end = .; } > spare'
                  '  .bss (NOLOAD) : { . = ALIGN(32); __bss2_start = .; *(.bss*) *(COMMON) . = ALIGN(32); __bss2_end = .; } > big'
                  '  .wup_lvglbuf (NOLOAD) : { *(.wup_lvglbuf*) } > spare%s }\n'
                  % (BASE, LIMIT - BASE, SPARE, SPARE_END - SPARE, BIG, big_end - BIG, ''.join(' %s(.text* .rodata*)' % x for x in in_big), app_text, ro_app, ro_spare, discard))
    open(ld, 'w').write(script)
    elf = os.path.join(bdir, 'app.elf')
    cmd = [paths.LD, '--gc-sections', '--error-limit=0', '-e', '_wup_entry', '-u', '_wup_header', '-T', ld, '-o', elf] + objs
    if os.environ.get('WUP_DEBUG_LINK'): open('/tmp/wup_link_cmd.txt', 'w').write('\n'.join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        if not final and ('will not fit' in r.stderr or 'overflow' in r.stderr): return None
        sys.exit('link failed:\n' + r.stderr[-4000:])
    syms = {}
    for l in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    if not large:
        binf = os.path.join(bdir, 'app.bin')
        r = subprocess.run([paths.LD, '--gc-sections', '-e', '_wup_entry', '-u', '_wup_header', '-T', ld, '--oformat', 'binary', '-o', binf] + objs,
                           capture_output=True, text=True)
        img = bytearray(open(binf, 'rb').read())[:syms['__bss_start'] - BASE]
        end = syms['__bss_end']
        if end > LIMIT:
            if not final: return None
            sys.exit('%s: too big: .bss reaches 0x%x, the app area ends at 0x%x' % (name, end, LIMIT))
        seg2 = b''; big = b''
    else:
        b1, b2 = os.path.join(bdir, 'seg1.bin'), os.path.join(bdir, 'seg2.bin')
        subprocess.run([OBJCOPY, '-O', 'binary', '-j', '.header', '-j', '.entry', '-j', '.data', elf, b1], check=True)   # (.entry: + text_in_app)
        subprocess.run([OBJCOPY, '-O', 'binary', '-j', '.text', elf, b2], check=True)
        img = bytearray(open(b1, 'rb').read())[:syms['__seg1_end'] - BASE]
        seg2 = open(b2, 'rb').read()[:syms['__seg2_end'] - SPARE]
        big = b''
        if syms.get('__big_end', 0) > syms.get('__big_start', 0):
            b3 = os.path.join(bdir, 'big.bin')
            subprocess.run([OBJCOPY, '-O', 'binary', '-j', '.bigtext', elf, b3], check=True)
            big = open(b3, 'rb').read()[:syms['__big_end'] - BIG]
        end = syms['__bss_end']
    if img[:4] != b'HBRW': sys.exit('no WUP_APP(...) header in the sources')
    struct.pack_into('<I', img, 8, len(img))
    if large:
        struct.pack_into('<III', img, 44, 0x32474553, SPARE, len(seg2))     # "SEG2"
        e2 = syms['__bss2_end']
        if big: print('%s: %d KB of code loaded into WUP_BIGMEM at 0x%x before main()' % (name, len(big) >> 10, BIG))
        lim = BIG_END if screen8 else BIG_SCREEN
        print('%s (large app): 0x350000 %d KB (%s), stack to 0x%x, %d KB free; code %d KB at 0x%x, %d KB free; .bss %d KB at 0x%x, %d KB left %s'
              % (name, len(img) >> 10, 'header, data' + ('' if net_spare else ', constants'), end, (LIMIT - end) >> 10, len(seg2) >> 10, SPARE,
                 (SPARE_END - SPARE - len(seg2)) >> 10, (e2 - syms['__bss2_start']) >> 10, BIG, (lim - e2) >> 10,
                 'in WUP_BIGMEM' if screen8 else 'below the screen'))
    else:
        print('%s: code+data %d KB, with .bss up to 0x%x (%d KB free of the app area)' % (name, len(img) >> 10, end, (LIMIT - end) >> 10))
    return bytes(img) + seg2, (big if large else b'')


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
    lg = cfg.get('large', 'auto')
    pats = cfg.get('thumb_sources', '').split()               # '!pattern' excludes (hot code stays ARM)
    thumb_srcs = set(sum((glob.glob(os.path.join(d, x)) for x in pats if not x.startswith('!')), []))
    thumb_srcs -= set(sum((glob.glob(os.path.join(d, x[1:])) for x in pats if x.startswith('!')), []))
    app_text = set(sum((glob.glob(os.path.join(d, x)) for x in cfg.get('text_in_app', '').split()), []))
    libs_big = {'lvgl', 'bearssl'} & set(cfg.get('text_in_big', '').split())       # (whole libraries)
    big_text = set(sum((glob.glob(os.path.join(d, x)) for x in cfg.get('text_in_big', '').split() if x not in libs_big), []))
    big_text = {os.path.normpath(x) for x in big_text} | libs_big | {x for x in cfg.get('text_in_big', '').split()
                if x not in libs_big and not any(c in x for c in '/*?[')}   # a plain name: a libwup module too
    thumb_srcs = {os.path.normpath(x) for x in thumb_srcs}; app_text = {os.path.normpath(x) for x in app_text}
    code, big = build(srcs, name, None, [os.path.join(d, x) for x in cfg.get('include', '').split()], cfg.get('defines', '').split(),
                 cfg.get('thumb', 'no') == 'yes', {'yes': True, 'raw': 'raw'}.get(cfg.get('net', 'no'), False), None if lg == 'auto' else lg == 'yes', thumb_srcs,
                 cfg.get('cflags', '').split(), cfg.get('screen8', 'no') == 'yes', app_text, kit or cfg.get('lvgl', 'no') == 'yes', cfg.get('system', 'no') == 'yes', big_text,
                 cfg.get('https', 'no') == 'yes')
    icon = os.path.join(d, cfg['icon']) if cfg.get('icon') else None
    author, version = cfg.get('author', ''), cfg.get('version', '1.0')
    description = cfg.get('description', '')
    return (code, big), name, author, version, icon, description


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
        code = build([os.path.abspath(s) for s in srcs], name, None, multi('-I'), multi('-D'), '--thumb' in a, '--net' in a,
                     True if '--large' in a else None)
        icon, author, version, out = opt('--icon'), opt('--author', ''), opt('--version', '1.0'), opt('-o')
        description = opt('--description', '')
    import wupack
    code, big = code
    data = wupack.pack(code, name, author, version, icon, description, big)
    out = out or os.path.join(paths.apps_out(), name.replace(' ', '_').lower() + '.wup')
    open(out, 'wb').write(data)
    print('%s (%d KB)' % (out, len(data) >> 10))


if __name__ == '__main__':
    main()
