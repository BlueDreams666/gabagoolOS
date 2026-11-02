import json, os, subprocess, re, hashlib, concurrent.futures as cf
lib = json.load(open('sloppy-dev/prototype/lib.json'))
root = 'build-sim/simdisk/SLOPPY/Sorted Music Jojo'
def safe(s): return re.sub(r'[\\/:*?"<>|]', '-', s).strip().rstrip('.')
jobs = []
for a in lib:
    d = os.path.join(root, safe(a['ar']), safe(a['al'] + (f" ({a['y']})" if a['y'] else '')))
    os.makedirs(d, exist_ok=True)
    h = int(hashlib.md5((a['al'] + a['ar']).encode()).hexdigest()[:6], 16)
    cov = os.path.join(d, 'cover.jpg')
    if not os.path.exists(cov):
        hue = h % 360
        jobs.append(['convert', '-size', '480x480', f'gradient:hsl({hue},60%,45%)-hsl({(hue+70)%360},70%,15%)',
                     '-gravity', 'southwest', '-fill', 'white', '-font', 'DejaVu-Sans-Bold', '-pointsize', '40',
                     '-annotate', '+28+60', a['al'][:28], '-pointsize', '24', '-annotate', '+30+24', a['ar'][:40], cov])
    for n, t in enumerate(a['t'], 1):
        title, ln, ext = t[0], t[1] or 200, t[2]
        f = os.path.join(d, f"{n:02d} - {safe(title)[:80]}.{'flac' if ext == 'FLAC' else 'mp3'}")
        if os.path.exists(f): continue
        cmd = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', f'anullsrc=r=22050:cl=mono', '-t', str(min(ln, 600)),
               '-metadata', f'title={title}', '-metadata', f'artist={a["ar"]}', '-metadata', f'album_artist={a["ar"]}',
               '-metadata', f'album={a["al"]}', '-metadata', f'track={n}', '-metadata', f'date={a["y"]}']
        cmd += (['-c:a', 'flac'] if ext == 'FLAC' else ['-c:a', 'libmp3lame', '-b:a', '32k'])
        cmd.append(f)
        jobs.append(cmd)
with cf.ThreadPoolExecutor(4) as ex:
    list(ex.map(lambda c: subprocess.run(c), jobs))
print('files', sum(len(a['t']) for a in lib))
