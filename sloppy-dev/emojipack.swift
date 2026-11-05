// Colour emoji packs for GabagoolOS (apps/sloppy/fonts/packs/emoc<px>.slf),
// read a picture at a time from the card by sloppy_xfont.inc.
//
//   swiftc -O sloppy-dev/emojipack.swift -o /tmp/emojipack
//   /tmp/emojipack <noto-emoji>/2D/png/72 <noto-emoji>/third_party/region-flags/png <px> <out.slf>
//
// The pictures are Noto Color Emoji's (github.com/googlefonts/noto-emoji,
// 2D/png/72; SIL Open Font License 1.1, OFL-NotoColorEmoji.txt), the flags
// region-flags' (third_party/region-flags/png; public domain,
// LICENSE-region-flags.txt). Each is fitted into a px square.
//
// File, little endian:
//   0   "SLE1"
//   4   u16 px        the square every picture is drawn in
//   6   u16 count
//   8   u32 index_off count entries of 36 bytes, sorted by key
//   12  u32 data_off
// An entry: u8 key length, u8 w, u8 h, u8 advance, i8 bottom over the
// baseline, 3 bytes nothing, u32 offset from data_off, then the key: up to
// 8 code points of 3 bytes. A key is the sequence without U+200D and U+FE0F,
// compared code point by code point, a shorter one first.
// A picture is w*h RGB565 then w*h alpha, as LVGL's RGB565A8.
import CoreGraphics
import Foundation
import ImageIO

let args = CommandLine.arguments
guard args.count == 5, let px = Int(args[3]) else {
    print("usage: emojipack <png dir> <flags png dir> <px> <out.slf>")
    exit(1)
}
let fm = FileManager.default
var items: [([UInt32], URL)] = []
for name in (try? fm.contentsOfDirectory(atPath: args[1])) ?? [] where name.hasPrefix("emoji_u") && name.hasSuffix(".png") {
    let hex = name.dropFirst(7).dropLast(4).split(separator: "_")
    let cps = hex.compactMap { UInt32($0, radix: 16) }.filter { $0 != 0x200d && $0 != 0xfe0f }
    if cps.count != hex.filter({ $0 != "200d" && $0 != "fe0f" }).count || cps.isEmpty || cps.count > 8 { continue }
    if cps.contains(where: { $0 >= 0xe0000 }) { continue }   // tag flags: drawn as the black flag
    items.append((cps, URL(fileURLWithPath: args[1]).appendingPathComponent(name)))
}
for name in (try? fm.contentsOfDirectory(atPath: args[2])) ?? [] where name.count == 6 && name.hasSuffix(".png") {
    let cc = Array(name.prefix(2).unicodeScalars)
    guard cc.allSatisfy({ $0.value >= 65 && $0.value <= 90 }) else { continue }
    items.append((cc.map { 0x1f1e6 + $0.value - 65 }, URL(fileURLWithPath: args[2]).appendingPathComponent(name)))
}
items.sort { a, b in
    for i in 0..<min(a.0.count, b.0.count) where a.0[i] != b.0[i] { return a.0[i] < b.0[i] }
    return a.0.count < b.0.count
}
var seen = Set<[UInt32]>()
items = items.filter { seen.insert($0.0).inserted }

let adv = px + (px + 5) / 10, below = px / 8
let space = CGColorSpaceCreateDeviceRGB()
var index = Data(), data = Data()
for (cps, url) in items {
    guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil),
          let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: px * 4, space: space,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { continue }
    ctx.interpolationQuality = .high
    let s = Double(px) / Double(max(img.width, img.height))
    let w = Double(img.width) * s, h = Double(img.height) * s
    ctx.draw(img, in: CGRect(x: (Double(px) - w) / 2, y: (Double(px) - h) / 2, width: w, height: h))
    let p = ctx.data!.assumingMemoryBound(to: UInt8.self)
    var rgb = Data(count: px * px * 2), alpha = Data(count: px * px)
    for i in 0..<(px * px) {
        let a = Int(p[i * 4 + 3])
        func un(_ v: UInt8) -> Int { a == 0 ? 0 : min(255, (Int(v) * 255 + a / 2) / a) }
        let r = un(p[i * 4]), g = un(p[i * 4 + 1]), b = un(p[i * 4 + 2])
        let v = UInt16((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
        rgb[i * 2] = UInt8(v & 0xff)
        rgb[i * 2 + 1] = UInt8(v >> 8)
        alpha[i] = UInt8(a)
    }
    var e = Data([UInt8(cps.count), UInt8(px), UInt8(px), UInt8(adv), UInt8(bitPattern: Int8(-below)), 0, 0, 0])
    withUnsafeBytes(of: UInt32(data.count).littleEndian) { e.append(contentsOf: $0) }
    for i in 0..<8 {
        let c = i < cps.count ? cps[i] : 0
        e.append(contentsOf: [UInt8(c & 0xff), UInt8(c >> 8 & 0xff), UInt8(c >> 16 & 0xff)])
    }
    index.append(e)
    data.append(rgb)
    data.append(alpha)
}
var out = Data("SLE1".utf8)
withUnsafeBytes(of: UInt16(px).littleEndian) { out.append(contentsOf: $0) }
withUnsafeBytes(of: UInt16(index.count / 36).littleEndian) { out.append(contentsOf: $0) }
withUnsafeBytes(of: UInt32(16).littleEndian) { out.append(contentsOf: $0) }
withUnsafeBytes(of: UInt32(16 + index.count).littleEndian) { out.append(contentsOf: $0) }
out.append(index)
out.append(data)
try! out.write(to: URL(fileURLWithPath: args[4]))
print("\(args[4]): \(index.count / 36) emoji at \(px) px, \(out.count / 1024) KB")
