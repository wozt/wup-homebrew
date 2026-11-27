#!/usr/bin/env python3
"""The project's App Store repository, in the format of the Homebrew App Store's libget (github.com/fortheusers/libget):
repo.json, zips/<name>.zip (the files to put on the pad, at their paths there), packages/<name>/icon.png. Publish the
folder as it is (a web server over HTTPS, e.g. GitHub Pages); anyone's repository is made the same way.

    python3 tools/mkrepo.py [--out OUT] [APP_FOLDER ...]     -> build/repo (default: the apps below)

Each package's zip holds apps/<id>/<id>.wup (the Homebrew Menu's layout), and the app.cfg's files = (data the app
needs beside it, e.g. a game's data file) in the same folder, or where it says (/path/on/the/pad=file: RetroArch's
cores). wup-homebrew itself is not in it: Software
Update gets it from the project's GitHub Releases (tools/mkrelease.py)."""
import datetime, hashlib, io, json, os, re, shutil, subprocess, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
ROOT = os.path.dirname(HERE)
PROJECTS = os.path.dirname(ROOT)

HOME = 'https://github.com/ravensclawx/wup-homebrew'      # (an app's own: url in its app.cfg)

APPS = [os.path.join(paths.LIBWUP, 'examples', 'hello')]   # (folders; licence from app.cfg, else GPL-3.0-or-later)


def cfg(d):
    c, last = {}, None
    for line in open(os.path.join(d, 'app.cfg')):
        text = re.split(r'\s+#', line)[0].rstrip()
        if not text.strip() or text.lstrip().startswith('#'): continue
        if line[0] in ' \t' and last: c[last] += ' ' + text.strip()      # (an indented line continues the last key)
        elif '=' in text: k, v = text.split('=', 1); last = k.strip(); c[last] = v.strip()
    return c


def pkg(out, name, title, author, version, category, description, licence, files, icon, url=''):
    z = io.BytesIO()
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for path, data, how in files: zf.writestr(zipfile.ZipInfo(path, (2026, 1, 1, 0, 0, 0)), data, compress_type=how)
    data = z.getvalue()
    open(os.path.join(out, 'zips', name + '.zip'), 'wb').write(data)
    if icon:
        os.makedirs(os.path.join(out, 'packages', name), exist_ok=True)
        shutil.copy(icon, os.path.join(out, 'packages', name, 'icon.png'))
    return {'name': name, 'title': title, 'author': author, 'version': version, 'category': category, 'license': licence,
            'description': description, 'details': description, 'changelog': '', 'url': url, 'screens': 0, 'app_dls': 0,
            'md5': hashlib.md5(data).hexdigest(), 'filesize': (len(data) + 1023) // 1024,
            'extracted': (sum(len(f[1]) for f in files) + 1023) // 1024,
            'binary': files[0][0] if files[0][0].startswith('apps/') else '', 'updated': datetime.date.today().strftime('%d/%m/%Y')}


def main():
    a = sys.argv[1:]
    out = os.path.join(paths.local_out(), 'repo')
    if '--out' in a: i = a.index('--out'); out = a[i + 1]; del a[i:i + 2]
    shutil.rmtree(out, ignore_errors=True); os.makedirs(os.path.join(out, 'zips'))
    def built(d):
        r = subprocess.run([sys.executable, os.path.join(paths.LIBWUP, 'tools', 'wupbuild.py'), d], capture_output=True, text=True, cwd=ROOT)
        m = re.search(r'(\S+\.wup) \(', r.stdout)
        if r.returncode or not m: sys.exit('%s did not build:\n%s' % (d, r.stdout[-2000:] + r.stderr[-2000:]))
        return open(m.group(1), 'rb').read()
    def extra(d, wid, f):                                 # files =: a file beside the app, or /path/on/the/pad=file
        dest, src = f.split('=', 1) if '=' in f else ('apps/%s/%s' % (wid, os.path.basename(f)), f)
        return (dest.lstrip('/'), open(os.path.join(d, src), 'rb').read(), zipfile.ZIP_DEFLATED)
    packages = []
    for d in [os.path.abspath(x) for x in a] or APPS:
        c = cfg(d)
        wid = c['name'].replace(' ', '_').lower()
        icon = os.path.join(d, c['icon'].split()[0]) if c.get('icon') else None
        packages.append(pkg(out, wid, c['name'], c.get('author', ''), c.get('version', '1.0'), c.get('category', 'tool'),
                            c.get('description', ''), c.get('licence', 'GPL-3.0-or-later'),
                            [('apps/%s/%s.wup' % (wid, wid), built(d), zipfile.ZIP_STORED)]
                            + [extra(d, wid, f) for f in c.get('files', '').split()], icon, c.get('url', HOME)))
        print('  %-18s %s' % (wid, c.get('version')))
    json.dump({'packages': packages}, open(os.path.join(out, 'repo.json'), 'w'), indent=1)
    print('%s: %d packages' % (out, len(packages)))


if __name__ == '__main__':
    main()
