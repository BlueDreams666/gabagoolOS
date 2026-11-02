#!/usr/bin/env python3
"""epub_fixtures.py OUTDIR: the e-book reader's test books (batch_epub.inc).

latin.epub    EPUB 3, every file stored: nav contents with a #fragment, a cover, three chapters
hebrew.epub   EPUB 2, NCX contents, Hebrew, deflated
deflate.epub  EPUB 2, deflated, a linear="no" aside, entities, no cover
big.epub      one chapter of about 2 MB of XHTML, deflated
broken.epub   latin.epub cut short: its directory and its OPF gone
zerowidth.epub a paragraph of zero-width letters, one line longer than the reader's buffer
garbage.epub  not a zip at all
drm.epub      a chapter listed in encryption.xml
cjk.epub      Chinese words (drawn from the card's glyph packs)
hindi.epub    Devanagari, which no font here draws
"""
import io, os, struct, sys, zipfile, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

CONTAINER = ('<?xml version="1.0"?>\n<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="%s" media-type="application/oebps-package+xml"/></rootfiles></container>')


def png(w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def xhtml(title, body, lang='en'):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml" '
            'xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="%s"><head><title>%s</title>'
            '<style>p { text-indent: 1em; } /* <p>not words</p> */</style>'
            '<script>var x = "<p>nor these</p>";</script></head><body>%s</body></html>' % (lang, title, body))


def write(name, files, method=zipfile.ZIP_STORED, order=None):
    path = os.path.join(out, name)
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
        for n in (order or files):
            z.writestr(n, files[n], compress_type=method)
    return path


words = ('The harbour was quiet in the hour before the boats came in, and the gulls walked the wall as if they owned it. '
         'Nobody had told them otherwise, and nobody would.')

# ---- latin: EPUB 3
ch1 = '<h1 id="c1">The Quiet Harbour</h1>' + ''.join(
    '<p>%d. %s</p>' % (i + 1, words) for i in range(36))
ch2 = ('<h2>Part Two</h2><p><em>A letter, kept in a drawer for forty years.</em></p>'
       '<p>Caf&eacute; &amp; bar &#8212; &#x201C;open&#x201D;, she said.<br/>Then nothing.</p>'
       '<blockquote><p>Quoted words, set in from the edge.</p></blockquote>'
       '<ul><li>First thing</li><li>Second thing</li></ul><ol><li>One</li><li>Two</li></ol>'
       '<p><img src="../img/map.png" alt="A map of the bay"/></p><hr/>'
       + ''.join('<p>%s</p>' % words for _ in range(30)) +
       '<h3 id="part-b">The Far Side</h3>' + ''.join('<p>B %d. %s</p>' % (i, words) for i in range(10)))
ch3 = '<h1>Afterword</h1><p>The last words of the book.</p>'
latin = {
    'META-INF/container.xml': CONTAINER % 'OEBPS/content.opf',
    'OEBPS/content.opf': ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
                          '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">q1</dc:identifier>'
                          '<dc:title>The Quiet Harbour</dc:title><dc:creator>Mira Sol</dc:creator><dc:language>en</dc:language></metadata>'
                          '<manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
                          '<item id="cov" href="img/cover.png" media-type="image/png" properties="cover-image"/>'
                          '<item id="map" href="img/map.png" media-type="image/png"/>'
                          '<item id="c1" href="text/one.xhtml" media-type="application/xhtml+xml"/>'
                          '<item id="c2" href="text/two%20b.xhtml" media-type="application/xhtml+xml"/>'
                          '<item id="c3" href="text/three.xhtml" media-type="application/xhtml+xml"/></manifest>'
                          '<spine><itemref idref="c1"/><itemref idref="c2"/><itemref idref="c3"/></spine></package>'),
    'OEBPS/nav.xhtml': xhtml('Contents', '<nav epub:type="landmarks"><ol><li><a href="text/three.xhtml">Not this</a></li></ol></nav>'
                                         '<nav epub:type="toc"><h1>Contents</h1><ol><li><a href="text/one.xhtml">One: The Quiet Harbour</a></li>'
                                         '<li><a href="text/two%20b.xhtml"><span>Two</span></a><ol><li><a href="text/two%20b.xhtml#part-b">The Far Side</a></li></ol></li>'
                                         '<li><a href="text/three.xhtml">Afterword</a></li></ol></nav>'),
    'OEBPS/img/cover.png': png(60, 90, (200, 80, 40)),
    'OEBPS/img/map.png': png(8, 8, (20, 120, 200)),
    'OEBPS/text/one.xhtml': xhtml('One', ch1),
    'OEBPS/text/two b.xhtml': xhtml('Two', ch2),
    'OEBPS/text/three.xhtml': xhtml('Three', ch3),
}
latin_order = ['META-INF/container.xml', 'OEBPS/img/cover.png', 'OEBPS/img/map.png', 'OEBPS/text/one.xhtml',
               'OEBPS/text/two b.xhtml', 'OEBPS/text/three.xhtml', 'OEBPS/nav.xhtml', 'OEBPS/content.opf']
p = write('latin.epub', latin, order=latin_order)

# ---- broken: the same, cut where its OPF would start (directory gone too)
data = open(p, 'rb').read()
cut = zipfile.ZipFile(p).getinfo('OEBPS/content.opf').header_offset
open(os.path.join(out, 'broken.epub'), 'wb').write(data[:cut + 10])
open(os.path.join(out, 'garbage.epub'), 'wb').write(b'This is not a book, only words in a file named like one.\n' * 20)


def epub2(title, author, lang, chapters, ncx_titles, extra_items='', extra_spine='', rtl=False):
    items = ''.join('<item id="c%d" href="c%d.xhtml" media-type="application/xhtml+xml"/>' % (i, i) for i in range(len(chapters)))
    spine = ''.join('<itemref idref="c%d"/>' % i for i in range(len(chapters)))
    files = {
        'META-INF/container.xml': CONTAINER % 'content.opf',
        'content.opf': ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0">'
                        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">'
                        '<dc:title>%s</dc:title><dc:creator opf:role="aut">%s</dc:creator><dc:language>%s</dc:language></metadata>'
                        '<manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>%s%s</manifest>'
                        '<spine toc="ncx"%s>%s%s</spine></package>') % (title, author, lang, items, extra_items,
                                                                       ' page-progression-direction="rtl"' if rtl else '', spine, extra_spine),
        'toc.ncx': ('<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap>' +
                    ''.join('<navPoint id="n%d" playOrder="%d"><navLabel><text>%s</text></navLabel><content src="c%d.xhtml"/></navPoint>'
                            % (i, i + 1, t, i) for i, t in enumerate(ncx_titles)) + '</navMap></ncx>'),
    }
    for i, c in enumerate(chapters): files['c%d.xhtml' % i] = xhtml('c%d' % i, c, lang)
    return files


he_words = 'הנמל היה שקט בשעה שלפני שהסירות חזרו, והשחפים הלכו על החומה כאילו היא שלהם. '
hebrew = epub2('הנמל השקט', 'מירה סול', 'he',
               ['<h1>פרק ראשון</h1>' + ''.join('<p>%s%s</p>' % (he_words, he_words) for _ in range(20)),
                '<h1>פרק שני</h1><p>%s</p>' % he_words],
               ['פרק ראשון', 'פרק שני'])
write('hebrew.epub', hebrew, zipfile.ZIP_DEFLATED)

defl = epub2('Deflated &amp; Done', 'A. N. Author', 'en-GB',
             ['<h1>Opening</h1><p>Fish &amp; chips &lt;tonight&gt; &mdash; and &hellip; more.</p>' + ''.join('<p>%s</p>' % words for _ in range(12)),
              '<div><b>CHAPTER II</b></div><p>Words after a bold heading.</p>'],
             ['Opening', 'Chapter II'],
             '<item id="aside" href="aside.xhtml" media-type="application/xhtml+xml"/>', '<itemref idref="aside" linear="no"/>')
defl['aside.xhtml'] = xhtml('aside', '<p>An aside, outside the reading order.</p>')
write('deflate.epub', defl, zipfile.ZIP_DEFLATED)

paras, n = [], 0
while n < 2 * 1024 * 1024:
    q = '<p>%d. %s</p>\n' % (len(paras), ' '.join(words.split()[len(paras) % 7:] + words.split()[:len(paras) % 7]) * 3)
    paras.append(q); n += len(q)
big = epub2('A Very Long Chapter', 'Typist', 'en', ['<h1>The Only Chapter</h1>' + ''.join(paras)], ['The Only Chapter'])
write('big.epub', big, zipfile.ZIP_DEFLATED)

drm = epub2('Locked', 'Shop', 'en', ['<p>scrambled</p>'], ['One'])
drm['META-INF/encryption.xml'] = ('<?xml version="1.0"?><encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container" '
                                  'xmlns:enc="http://www.w3.org/2001/04/xmlenc#"><enc:EncryptedData><enc:CipherData>'
                                  '<enc:CipherReference URI="c0.xhtml"/></enc:CipherData></enc:EncryptedData></encryption>')
write('drm.epub', drm, zipfile.ZIP_DEFLATED)

zh = '港口在渔船归来之前的那一小时里很安静，海鸥沿着墙走着，好像墙是它们的。'
cjk = epub2('安静的港口', '米拉', 'zh', ['<h1>第一章</h1>' + ''.join('<p>%s</p>' % (zh * 3) for _ in range(10))], ['第一章'])
write('cjk.epub', cjk, zipfile.ZIP_DEFLATED)

hi = 'बंदरगाह उस घंटे में शांत था जब नावें लौटती हैं, और समुद्री पक्षी दीवार पर ऐसे चलते थे जैसे वह उनकी हो। '
hindi = epub2('शांत बंदरगाह', 'मीरा', 'hi', ['<h1>पहला अध्याय</h1>' + ''.join('<p>%s</p>' % (hi * 2) for _ in range(10))], ['पहला अध्याय'])
write('hindi.epub', hindi, zipfile.ZIP_DEFLATED)
# a paragraph of 2100 zero-width non-joiners and a word: a line that never wraps, 6 KB+
zw = epub2('Zero Width', 'Nobody', 'en', ['<h1>One</h1><p>' + '&zwnj;' * 2100 + 'end</p>'], ['One'])
write('zerowidth.epub', zw, zipfile.ZIP_DEFLATED)
