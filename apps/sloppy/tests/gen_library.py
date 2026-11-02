#!/usr/bin/env python3
"""A card full of small real MP3s for the scan test (run-scan-test.sh).

  gen_library.py <card dir> <songs> [--unicode]

Each song is a real file: an ID3v2.4 tag (UTF-8: title, artist, album,
album artist, track, disc, year, genre) and three silent MPEG-1 Layer III
frames, so the tag database reads it as it would on the card. Ten songs
an album, four albums an artist, folders Artist/Album/NN Title.mp3. One
album in ten has a guest on some songs ("feat."), one in twenty is two
discs, one in fifty a compilation (Various Artists, each song its own
artist). With --unicode, a set of names that break naive code is added
on top: emoji, accents, Hebrew, Arabic, Chinese and Japanese, quotes, #,
%, &, ;, brackets, leading dots and spaces, and names as long as the card
allows. Prints how many files it wrote."""
import os, sys, struct

FRAME = b'\xff\xfb\x90\x64' + bytes(417 - 4)       # 128 kbit/s, 44.1 kHz, silent

def synchsafe(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])

def text_frame(fid, s):
    body = b'\x03' + s.encode('utf-8')               # 3: UTF-8 (ID3v2.4)
    return fid.encode() + synchsafe(len(body)) + b'\x00\x00' + body

def song(path, title, artist, album, albumartist, track, disc, year, genre):
    frames = b''.join(text_frame(f, v) for f, v in (
        ('TIT2', title), ('TPE1', artist), ('TALB', album), ('TPE2', albumartist),
        ('TRCK', str(track)), ('TPOS', str(disc)), ('TDRC', str(year)), ('TCON', genre)) if v)
    tag = b'ID3\x04\x00\x00' + synchsafe(len(frames)) + frames
    with open(path, 'wb') as f:
        f.write(tag + FRAME * 3)

GENRES = ['Rock', 'Pop', 'Jazz', 'Electronic', 'Folk', 'Classical', 'Hip-Hop/Rap', 'Soul; Funk']

def safe(s):
    # a file name the card (exFAT) can hold; the tags keep every character
    return ''.join('_' if c in '"*/:<>?\\|' else c for c in s)

def main():
    card, n = sys.argv[1], int(sys.argv[2])
    unicode_set = '--unicode' in sys.argv
    music = os.path.join(card, 'Music')
    os.makedirs(os.path.join(card, '.rockbox'), exist_ok=True)
    written = 0
    a = 0
    while written < n:
        artist = 'Artist %05d' % (a // 4)
        album = 'Album %06d' % a
        comp = a % 50 == 49
        aa = 'Various Artists' if comp else artist
        d = os.path.join(music, safe(aa), safe(album))
        os.makedirs(d, exist_ok=True)
        for k in range(10):
            if written >= n:
                break
            disc = 1 + (k >= 5 if a % 20 == 7 else 0)
            title = 'Song %06d' % written
            if comp:
                who = 'Artist %05d' % ((a * 7 + k * 13) % 9000)
            elif a % 10 == 3 and k % 3 == 0:
                who = '%s feat. Guest %04d' % (artist, (a + k) % 997)
            else:
                who = artist
            song(os.path.join(d, '%02d %s.mp3' % (k + 1, title)), title, who, album, aa, k % 5 + 1 if disc == 2 else k + 1,
                 disc, 1960 + a % 66, GENRES[a % len(GENRES)])
            written += 1
        a += 1
    written += cue_images(card)
    if unicode_set:
        written += unicode_names(card)
    print(written)

# Albums in one file: a FLAC with its sheet inside (a CUESHEET comment),
# and one with the sheet beside it. Forty minutes each by their STREAMINFO.
def flac(path, comments):
    rate, ch, bits, total = 44100, 2, 16, 44100 * 2400
    si = struct.pack('>HH', 4096, 4096) + bytes(6) + bytes([
        (rate >> 12) & 0xff, (rate >> 4) & 0xff, ((rate & 0xf) << 4) | ((ch - 1) << 1) | ((bits - 1) >> 4),
        (((bits - 1) & 0xf) << 4) | ((total >> 32) & 0xf)]) + struct.pack('>I', total & 0xffffffff) + bytes(16)
    vc = struct.pack('<I', 3) + b'gen' + struct.pack('<I', len(comments))
    for c in comments:
        b = c.encode('utf-8'); vc += struct.pack('<I', len(b)) + b
    with open(path, 'wb') as f:
        f.write(b'fLaC' + bytes([0]) + struct.pack('>I', len(si))[1:] + si
                + bytes([0x84]) + struct.pack('>I', len(vc))[1:] + vc)

SHEET = ('PERFORMER "Cue Artist"\nTITLE "%s"\nFILE "%s" WAVE\n'
         '  TRACK 01 AUDIO\n    TITLE "One"\n    INDEX 01 00:00:00\n'
         '  TRACK 02 AUDIO\n    TITLE "Two"\n    INDEX 01 12:00:00\n'
         '  TRACK 03 AUDIO\n    TITLE "Three"\n    PERFORMER "Cue Guest"\n    INDEX 01 25:30:00\n')

def cue_images(card):
    d = os.path.join(card, 'Music', 'Cue')
    os.makedirs(d, exist_ok=True)
    flac(os.path.join(d, 'Inside.flac'), ['TITLE=Inside', 'ARTIST=Cue Artist', 'CUESHEET=' + SHEET % ('Sheet inside', 'Inside.flac')])
    flac(os.path.join(d, 'Beside.flac'), ['TITLE=Beside', 'ARTIST=Cue Artist'])
    with open(os.path.join(d, 'Beside.cue'), 'w', encoding='utf-8') as f:
        f.write(SHEET % ('Sheet beside', 'Beside.flac'))
    return 2

# Names that trip up paths, playlists, cue sheets and URLs, each a song
UNICODE = [
    ('Emoji 🎧', 'Sunrise 🌅', 'Kite 🪁', 'Wave 🌊'),
    ('Accents Édith', 'Café à la crème', 'Beyoncé & Ñandú', 'Øresund Ångström'),
    ('Hebrew שלום', 'שיר ערש', 'אריק איינשטיין', 'עוד יום'),
    ('Arabic سلام', 'أغنية الليل', 'فيروز', 'زهرة المدائن'),
    ('CJK 音楽', '夜の歌', '周杰伦', '안녕하세요'),
    ('Quotes "It\'s"', 'Don\'t "Stop" Now', 'Rock \'n\' Roll', '“Curly” ‘Quotes’'),
    ('Symbols #1 100% A&B', 'Track #2 at 50%', 'Rock & Roll; Part 2', 'C:\\ is not a path?'),
    ('Brackets [Live] (Remix) {Edit}', 'Song (feat. Guest) [2024]', '+ Plus = Equals', '~Tilde~ `Backtick`'),
    (' Leading space', '.Leading dot', '..Two dots', 'Trailing dot.'),
]

def unicode_names(card):
    root = os.path.join(card, 'Music', 'Unicode test')
    n = 0
    for i, (album, *titles) in enumerate(UNICODE):
        d = os.path.join(root, safe(album))
        os.makedirs(d, exist_ok=True)
        for k, t in enumerate(titles):
            name = '%02d %s.mp3' % (k + 1, safe(t))
            song(os.path.join(d, name), t, 'Unicode Artist ' + str(i), album, 'Unicode Artist ' + str(i), k + 1, 1, 2020, 'Pop')
            n += 1
    # long names: as long as Rockbox's paths go (259 bytes in all), then past it
    for sub, room in (('Long', 255), ('Too long', 330)):
        d = os.path.join(root, sub)
        os.makedirs(d, exist_ok=True)
        rel = len(('/Music/Unicode test/%s/' % sub).encode())
        for k, base in enumerate(['Long ' + 'x' * 400, 'Lång ' + 'é' * 200, 'ארוך ' + 'ש' * 200, '長い ' + '曲' * 140]):
            cut = min(room - rel - 4, 251)
            name = base.encode('utf-8')[:cut].decode('utf-8', 'ignore') + '.mp3'
            song(os.path.join(d, name), base[:60], 'Unicode Artist L', sub, 'Unicode Artist L', k + 1, 1, 2020, 'Pop')
            n += 1
    # an SACD image: the master TOC at sector 510, a stereo area of ten seconds
    d = os.path.join(card, 'Music', 'SACD')
    os.makedirs(d, exist_ok=True)
    disc = bytearray(2048 * 540)
    disc[510 * 2048:510 * 2048 + 8] = b'SACDMTOC'
    disc[510 * 2048 + 64:510 * 2048 + 68] = struct.pack('>I', 520)
    a = 520 * 2048
    disc[a:a + 8] = b'TWOCHTOC'
    disc[a + 32] = 2
    disc[a + 64:a + 67] = bytes([0, 10, 0])
    disc[a + 72:a + 80] = struct.pack('>II', 530, 539)
    with open(os.path.join(d, 'Test disc.iso'), 'wb') as f:
        f.write(disc)
    n += 1
    return n

if __name__ == '__main__':
    main()
