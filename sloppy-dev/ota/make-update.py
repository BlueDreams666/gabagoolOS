#!/usr/bin/env python3
"""make-update.py: a GabagoolOS update file (.gbupdate) from a release zip.

The player checks an Ed25519 signature over the manifest with the public
key compiled into it (apps/sloppy/sloppy_ota_key.h), then the SHA-256 of
the zip and of every file. Only files under .rockbox/ are put in, never
.rockbox/sloppy/ (the owner's saves; the Gabagool clip is already there).

  make-update.py pubkey KEY.pem
      the lines for sloppy_ota_key.h (needs OpenSSL 3 for -outform DER of
      an Ed25519 key; 1.1.1 works too)
  make-update.py build --zip NEW.zip --version 1.1 [--min-from 1.0]
                 [--from OLD.zip] [--notes notes.json] [--build-date D]
                 [--boot-menu] -o OUT
      writes OUT.manifest.json and OUT.payload.zip. With --from only what
      changed goes in, and what is gone is listed to remove (then give
      --min-from the old version). notes.json: {"en": "...", "he": "..."}
  make-update.py sign --key KEY.pem OUT        (OpenSSL 3: pkeyutl -rawin)
      writes OUT.manifest.sig
  make-update.py pack OUT
      writes OUT.gbupdate from the three
  make-update.py all --key KEY.pem ...build arguments... -o OUT
      build, sign and pack
  make-update.py feed OUT.gbupdate
      writes gabagoolOS-update.txt beside it: the version, the file's name
      and its size, which Settings > Software update > Check for updates
      reads from the newest release on GitHub. Attach both to the release.
      The file names stay plain (letters, digits, . _ -) or the player
      refuses them.

OPENSSL=/path/to/openssl picks the openssl to run (default: openssl).
The file: b"GBUPDATE\\x01\\0\\0\\0", u32 LE manifest length, u32 LE 64,
the manifest, the signature, the zip."""
import argparse, datetime, hashlib, json, os, struct, subprocess, sys, zipfile

MAGIC = b'GBUPDATE\x01\x00\x00\x00'
SKIP = ('.rockbox/sloppy/', '.rockbox/update/')
KEEP_NAMES = ('config.cfg', 'fixed.cfg', '.playlist_control', 'nvram.bin')
OK_CHARS = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-+/ ')


def openssl():
    return os.environ.get('OPENSSL', 'openssl')


def files_of(zpath):
    """{path: bytes} of the program files a release zip carries"""
    out = {}
    with zipfile.ZipFile(zpath) as z:
        for i in z.infolist():
            n = i.filename
            if n.endswith('/') or not n.startswith('.rockbox/') or n.startswith(SKIP):
                continue
            base = n.rsplit('/', 1)[-1]
            if base in KEEP_NAMES or n.endswith(('.tcd', '.bmark')):
                continue
            if not set(n) <= OK_CHARS or len(n) > 200 or '/../' in n or '/./' in n:
                raise SystemExit(f'not a name the player takes: {n!r}')
            out[n] = z.read(i)
    if '.rockbox/rockbox.r1' not in out:
        print('note: no .rockbox/rockbox.r1 in', zpath, file=sys.stderr)
    return out


def build(a):
    new = files_of(a.zip)
    old = files_of(getattr(a, 'from_zip')) if getattr(a, 'from_zip', None) else None
    put = {p: b for p, b in new.items() if old is None or old.get(p) != b}
    remove = sorted(p for p in old if p not in new) if old else []
    if not put:
        raise SystemExit('nothing changed')
    payload = a.out + '.payload.zip'
    with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(put):
            i = zipfile.ZipInfo(p, (2026, 1, 1, 0, 0, 0))
            i.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(i, put[p])
    zb = open(payload, 'rb').read()
    notes = json.load(open(a.notes, encoding='utf-8')) if a.notes else {}
    man = {
        'product': 'GabagoolOS',
        'version': a.version,
        'min_from': a.min_from or '',
        'build_date': a.build_date or datetime.date.today().isoformat(),
        'channel': 'stable',
        'zip': {'url': '', 'size': len(zb), 'sha256': hashlib.sha256(zb).hexdigest()},
        'files': [{'path': p, 'size': len(put[p]), 'sha256': hashlib.sha256(put[p]).hexdigest()} for p in sorted(put)],
        'remove': remove,
        'notes': notes,
        'needs_boot_menu': bool(a.boot_menu),
        'next_key': '',
    }
    with open(a.out + '.manifest.json', 'wb') as f:
        f.write(json.dumps(man, ensure_ascii=False, separators=(',', ':')).encode('utf-8'))
    print(f'{a.out}.manifest.json: {len(put)} files, {len(remove)} to remove; payload {len(zb)} bytes')


def sign(key, out):
    subprocess.run([openssl(), 'pkeyutl', '-sign', '-inkey', key, '-rawin', '-in', out + '.manifest.json',
                    '-out', out + '.manifest.sig'], check=True)


def pack(out):
    m = open(out + '.manifest.json', 'rb').read()
    s = open(out + '.manifest.sig', 'rb').read()
    z = open(out + '.payload.zip', 'rb').read()
    if len(s) != 64:
        raise SystemExit('the signature is not 64 bytes: is the key Ed25519?')
    with open(out + '.gbupdate', 'wb') as f:
        f.write(MAGIC + struct.pack('<II', len(m), len(s)) + m + s + z)
    print(out + '.gbupdate', len(MAGIC) + 8 + len(m) + len(s) + len(z), 'bytes')


def feed(gbupdate):
    b = open(gbupdate, 'rb').read()
    if b[:12] != MAGIC:
        raise SystemExit('not a .gbupdate')
    mlen, = struct.unpack('<I', b[12:16])
    man = json.loads(b[20:20 + mlen].decode('utf-8'))
    name = os.path.basename(gbupdate)
    if not set(name) <= set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-') or len(name) > 80:
        raise SystemExit(f'the player takes only plain names (letters, digits, . _ -): {name!r}')
    out = os.path.join(os.path.dirname(os.path.abspath(gbupdate)), 'gabagoolOS-update.txt')
    with open(out, 'w', encoding='ascii', newline='\n') as f:
        f.write(f"GBFEED1\nversion {man['version']}\nfile {name}\nsize {len(b)}\n")
    print(out, man['version'], name, len(b), 'bytes')


def pubkey(key):
    der = subprocess.run([openssl(), 'pkey', '-in', key, '-pubout', '-outform', 'DER'], check=True,
                         capture_output=True).stdout
    raw = der[-32:]
    if len(der) != 44:
        raise SystemExit('not an Ed25519 key')
    print('#define SL_OTA_KEY_SET 1')
    print('#define SL_OTA_KEY { ' + ', '.join('0x%02x' % b for b in raw) + ' }')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('pubkey'); p.add_argument('key')
    for name in ('build', 'all'):
        p = sp.add_parser(name)
        p.add_argument('--zip', required=True)
        p.add_argument('--version', required=True)
        p.add_argument('--min-from')
        p.add_argument('--from', dest='from_zip')
        p.add_argument('--notes')
        p.add_argument('--build-date')
        p.add_argument('--boot-menu', action='store_true')
        p.add_argument('-o', dest='out', required=True)
        if name == 'all':
            p.add_argument('--key', required=True)
    p = sp.add_parser('sign'); p.add_argument('--key', required=True); p.add_argument('out')
    p = sp.add_parser('pack'); p.add_argument('out')
    p = sp.add_parser('feed'); p.add_argument('gbupdate')
    a = ap.parse_args()
    if a.cmd == 'pubkey': pubkey(a.key)
    elif a.cmd == 'build': build(a)
    elif a.cmd == 'sign': sign(a.key, a.out)
    elif a.cmd == 'pack': pack(a.out)
    elif a.cmd == 'feed': feed(a.gbupdate)
    else: build(a); sign(a.key, a.out); pack(a.out)


if __name__ == '__main__':
    main()
