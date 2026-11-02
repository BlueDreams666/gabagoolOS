// Rasterise code points of a font into a GabagoolOS letter pack (.slf):
// letters for another script, loaded from the card the first time a word
// needs one (sloppy_xfont.inc). 4 bpp, as the compiled fonts.
//   swiftc -O slfpack.swift -o slfpack
//   ./slfpack NotoSansSC-Medium.otf 20 codepoints.txt out.slf
// codepoints.txt: one hex code point a line (or ranges a-b). A line
// key=a,b,c@i is a glyph drawn in context: the string a b c shaped by Core
// Text (its substitutions and mark positions), the glyphs of the letter at
// index i stored under key, placed from where our pen stands at that letter
// (the advances of the letters before it). Thai marks are kept this way.
//
// Format, little endian:
//   "SLF1"  u16 version 1  u16 px  i16 line_height  i16 base_line  u32 count  u32 bitmap bytes
//   u16 code point x count (sorted)
//   x count: u32 bitmap offset, u16 advance x16, u8 box w, u8 box h, i8 x offset, i8 y offset (bottom above baseline), u16 0
//   bitmaps: 4 bpp, two pixels a byte, a glyph's rows run on without padding
import CoreGraphics
import CoreText
import Foundation

let a = CommandLine.arguments
guard a.count >= 5 else { print("usage: slfpack font size codepoints.txt out.slf"); exit(1) }
let (path, size, listPath, out) = (a[1], Int(a[2])!, a[3], a[4])
var codes: [UInt32] = []
var contexts: [UInt32: ([UInt32], Int)] = [:]
for line in try! String(contentsOfFile: listPath, encoding: .utf8).split(separator: "\n") {
    if line.contains("=") {
        let kv = line.split(separator: "="), si = kv[1].split(separator: "@")
        let key = UInt32(kv[0], radix: 16)!
        contexts[key] = (si[0].split(separator: ",").map { UInt32($0, radix: 16)! }, Int(si[1])!)
        codes.append(key)
        continue
    }
    let p = line.split(separator: "-").map { UInt32($0.trimmingCharacters(in: .whitespaces), radix: 16)! }
    codes += Array(p[0]...(p.count > 1 ? p[1] : p[0]))
}
codes = Array(Set(codes)).sorted()
let provider = CGDataProvider(url: URL(fileURLWithPath: path) as CFURL)!
let font = CTFontCreateWithGraphicsFont(CGFont(provider)!, CGFloat(size), nil, nil)
let ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font)
struct G { var code: UInt32; var adv: Int; var w: Int; var h: Int; var ox: Int; var oy: Int; var px: [UInt8] }
var glyphs: [G] = []
let pad = size + 4             /* a mark reaches well left of its pen */
/* the glyphs of letter `at` in `str` as shaped, each with its place from our pen there */
func shaped(_ str: [UInt32], _ at: Int) -> ([CGGlyph], [CGPoint], CGFloat)? {
    let s = String(String.UnicodeScalarView(str.map { UnicodeScalar($0)! }))
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: [kCTFontAttributeName as NSAttributedString.Key: font]))
    var gs: [(CGGlyph, CGPoint, Int)] = []
    for run in CTLineGetGlyphRuns(line) as! [CTRun] {
        let n = CTRunGetGlyphCount(run)
        var g = [CGGlyph](repeating: 0, count: n), p = [CGPoint](repeating: .zero, count: n), ix = [CFIndex](repeating: 0, count: n)
        CTRunGetGlyphs(run, CFRange(), &g); CTRunGetPositions(run, CFRange(), &p); CTRunGetStringIndices(run, CFRange(), &ix)
        for i in 0..<n { gs.append((g[i], p[i], ix[i])) }   /* one UTF-16 unit a letter: the BMP */
    }
    /* sara am's nikhahit, split off and shaped ahead of a tone mark, stays sara am's */
    if let am = str.firstIndex(of: 0xe33) {
        let nik = shapedNikhahit()
        gs = gs.map { $0.0 == nik && $0.2 != am ? ($0.0, $0.1, am) : $0 }
    }
    var pen: CGFloat = 0, adv = CGSize.zero
    for (g, _, i) in gs where i < at { var gg = g; CTFontGetAdvancesForGlyphs(font, .horizontal, &gg, &adv, 1); pen += adv.width }
    let mine = gs.filter { $0.2 == at }
    if mine.isEmpty { return nil }
    var w: CGFloat = 0
    for (g, _, _) in mine { var gg = g; CTFontGetAdvancesForGlyphs(font, .horizontal, &gg, &adv, 1); w += adv.width }
    return (mine.map { $0.0 }, mine.map { CGPoint(x: $0.1.x - pen, y: $0.1.y) }, w)
}
var nikCache: CGGlyph? = nil
func shapedNikhahit() -> CGGlyph {
    if let n = nikCache { return n }
    nikCache = 0
    let s = "\u{0e01}\u{0e33}"
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: [kCTFontAttributeName as NSAttributedString.Key: font]))
    for run in CTLineGetGlyphRuns(line) as! [CTRun] {
        let n = CTRunGetGlyphCount(run)
        var g = [CGGlyph](repeating: 0, count: n), ix = [CFIndex](repeating: 0, count: n)
        CTRunGetGlyphs(run, CFRange(), &g); CTRunGetStringIndices(run, CFRange(), &ix)
        for i in 0..<n where ix[i] == 1 {
            var gg = g[i], adv = CGSize.zero
            CTFontGetAdvancesForGlyphs(font, .horizontal, &gg, &adv, 1)
            if adv.width == 0 { nikCache = g[i] }
        }
    }
    return nikCache!
}
for c in codes where c <= 0xffff {
    var g: [CGGlyph], at: [CGPoint]
    var adv = CGSize.zero
    if let (str, i) = contexts[c] {
        guard let r = shaped(str, i) else { continue }
        (g, at, adv.width) = (r.0, r.1, r.2)
    } else {
        var u = [UniChar](String(UnicodeScalar(c)!).utf16)
        g = [CGGlyph](repeating: 0, count: u.count)
        guard CTFontGetGlyphsForCharacters(font, &u, &g, u.count), g[0] != 0 else { continue }
        CTFontGetAdvancesForGlyphs(font, .horizontal, &g, &adv, 1)
        at = [.zero]
    }
    let cw = size * 2 + pad * 2, ch = size * 2 + pad * 2
    var buf = [UInt8](repeating: 0, count: cw * ch)
    let ctx = CGContext(data: &buf, width: cw, height: ch, bitsPerComponent: 8, bytesPerRow: cw,
                        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    ctx.setShouldAntialias(true); ctx.setAllowsFontSmoothing(false); ctx.setShouldSubpixelPositionFonts(false)
    ctx.setFillColor(gray: 1, alpha: 1)
    var pos = at.map { CGPoint(x: CGFloat(pad) + $0.x, y: CGFloat(pad + size / 2) + $0.y) }
    CTFontDrawGlyphs(font, &g, &pos, g.count, ctx)
    var x0 = cw, x1 = -1, y0 = ch, y1 = -1
    for y in 0..<ch { for x in 0..<cw where buf[y * cw + x] > 8 { x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y) } }
    var gl = G(code: c, adv: Int((adv.width * 16).rounded()), w: 0, h: 0, ox: 0, oy: 0, px: [])
    if x1 >= 0 {
        gl.w = x1 - x0 + 1; gl.h = y1 - y0 + 1
        gl.ox = x0 - pad
        let baselineRow = ch - 1 - (pad + size / 2)
        gl.oy = baselineRow - y1
        for y in y0...y1 { for x in x0...x1 { gl.px.append(buf[y * cw + x] >> 4) } }
    }
    glyphs.append(gl)
}
var data = Data()
func u16(_ v: Int) { var x = UInt16(truncatingIfNeeded: v).littleEndian; data.append(Data(bytes: &x, count: 2)) }
func u32(_ v: Int) { var x = UInt32(truncatingIfNeeded: v).littleEndian; data.append(Data(bytes: &x, count: 4)) }
var bitmaps = Data(), offs: [Int] = []
for g in glyphs {
    offs.append(bitmaps.count)
    var i = 0
    while i < g.px.count { bitmaps.append(g.px[i] << 4 | (i + 1 < g.px.count ? g.px[i + 1] : 0)); i += 2 }
}
data.append("SLF1".data(using: .ascii)!)
u16(1); u16(size); u16(Int((ascent + descent).rounded(.up))); u16(Int(descent.rounded(.up)))
u32(glyphs.count); u32(bitmaps.count)
for g in glyphs { u16(Int(g.code)) }
for (i, g) in glyphs.enumerated() {
    u32(offs[i]); u16(g.adv); data.append(UInt8(g.w)); data.append(UInt8(g.h))
    data.append(UInt8(bitPattern: Int8(g.ox))); data.append(UInt8(bitPattern: Int8(g.oy))); u16(0)
}
data.append(bitmaps)
try! data.write(to: URL(fileURLWithPath: out))
print("\((out as NSString).lastPathComponent): \(glyphs.count) glyphs, \(data.count) bytes")
