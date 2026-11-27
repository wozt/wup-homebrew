#!/usr/bin/env python3
"""Make the UI fonts in LVGL's format (fonts/*.c, kept in git): Inter, with the symbols LVGL's widgets
and the menu use from Font Awesome (the file LVGL ships). Run after changing sizes or symbols.

    python3 tools/mkfont_lv.py        (needs node: it runs LVGL's own converter, npx lv_font_conv)

Inter: SIL Open Font License 1.1 (fonts/Inter-LICENSE.txt). Font Awesome Free: SIL OFL 1.1."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths, deps

OUT = paths.FONTS
# name, file, pixel size
FACES = [('inter_16', 'Inter-Medium.ttf', 16), ('inter_20', 'Inter-Medium.ttf', 20),
         ('inter_24', 'Inter-SemiBold.ttf', 24), ('inter_32', 'Inter-SemiBold.ttf', 32),
         ('inter_hud', 'Inter-Bold.ttf', 13, 'ABXYLR+-')]            # the letters in the button badges only
# LV_SYMBOL_*: wifi, battery full..empty, charge, refresh, left, right, up, down, ok, close, settings, download, plus,
# backspace, new line, keyboard, eye open/close, warning, file, directory, home, power, trash, sun (WUP_SYMBOL_SUN), globe
SYMBOLS = '0xF1EB,0xF240-0xF244,0xF0E7,0xF021,0xF053,0xF054,0xF077,0xF078,0xF00C,0xF00D,0xF013,0xF019,0xF067,0xF55A,0xF8A2,' \
          '0xF11C,0xF06E,0xF070,0xF071,0xF15B,0xF07B,0xF015,0xF011,0xF2ED,0xF185,0xF028,0xF017,0xF120,0xF2DB,0xF0A0,0xF0AC,' \
          '0xF093,0xF7C2'


def main():
    deps.fetch('lvgl')
    fa = os.path.join(deps.DEPS['lvgl'][0], 'scripts', 'built_in_font', 'FontAwesome5-Solid+Brands+Regular.woff')
    os.makedirs(OUT, exist_ok=True)
    for name, ttf, size, *only in FACES:
        out = os.path.join(OUT, name + '.c')
        subprocess.run(['npx', '--yes', 'lv_font_conv@1.5.3', '--size', str(size), '--bpp', '4', '--format', 'lvgl', '--no-compress',
                        '--lv-include', 'lvgl.h', '--font', os.path.join(paths.FONTS, ttf)] +
                       (['--symbols', only[0]] if only else ['--range', '0x20-0x7E,0xB0,0x2022,0x2026', '--font', fa, '--range', SYMBOLS]) +
                       ['-o', out], check=True)
        s = open(out).read()
        i = s.index(' * Opts:'); j = s.index('\n', i)
        open(out, 'w').write(s[:i] + ' * Made by tools/mkfont_lv.py (Inter + Font Awesome symbols)' + s[j:])
        print(out, os.path.getsize(out) >> 10, 'KB of source')


if __name__ == '__main__':
    main()
