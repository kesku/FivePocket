// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(2)
}

guard CommandLine.arguments.count == 3 else {
    fail("usage: generate_ui_labels.swift FONT.ttf OUTPUT_DIRECTORY")
}
let fontURL = URL(fileURLWithPath: CommandLine.arguments[1])
let output = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true)
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
guard let descriptors = CTFontManagerCreateFontDescriptorsFromURL(fontURL as CFURL) as? [CTFontDescriptor],
      let original = descriptors.first else { fail("cannot read the supplied font") }
let descriptor = CTFontDescriptorCreateCopyWithVariation(original, NSNumber(value: 0x77676874), 700)
let probe = CTFontCreateWithFontDescriptor(descriptor, 100, nil)
guard (CTFontCopyPostScriptName(probe) as String).lowercased().contains("nunito") else {
    fail("the supplied font was not loaded")
}
let font = CTFontCreateCopyWithAttributes(probe, 5800 / CTFontGetCapHeight(probe), nil, nil)

for (file, prefix, label, minimumWidth, height) in [
    ("loading_label", "Loading", "Loading", 308, 83),
    ("map_tap_label", "Label", "Resume Game", 556, 68)
] {
    let text = NSAttributedString(string: label, attributes: [
        NSAttributedString.Key(kCTFontAttributeName as String): font,
        NSAttributedString.Key(kCTKernAttributeName as String): 0.8,
        NSAttributedString.Key(kCTForegroundColorFromContextAttributeName as String): true
    ])
    let line = CTLineCreateWithAttributedString(text)
    let bounds = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    let width = max(minimumWidth, Int(ceil(bounds.width)) + 8)
    var pixels = [UInt8](repeating: 0, count: width * height)
    let image = pixels.withUnsafeMutableBytes { bytes -> CGImage in
        guard let context = CGContext(data: bytes.baseAddress, width: width, height: height,
                                      bitsPerComponent: 8, bytesPerRow: width,
                                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue) else {
            fail("cannot create label bitmap")
        }
        context.setFillColor(gray: 1, alpha: 1)
        context.setShouldAntialias(true)
        context.textPosition = CGPoint(x: 4 - bounds.minX, y: CGFloat(height - 5 - 58))
        CTLineDraw(line, context)
        guard let image = context.makeImage() else { fail("cannot finish label bitmap") }
        return image
    }
    var header = "// Generated from the project's Nunito Sans font.\n#pragma once\n#include <cstdint>\n"
    header += "namespace recomp::gpu::maptap {\n"
    header += "constexpr unsigned \(prefix)W=\(width),\(prefix)H=\(height),\(prefix)Cap=58,\(prefix)CapTop=5,\(prefix)Left=4;\n"
    header += "inline constexpr std::uint8_t \(prefix)Coverage[\(pixels.count)]={\n"
    for row in stride(from: 0, to: pixels.count, by: 40) {
        header += pixels[row..<min(row + 40, pixels.count)].map(String.init).joined(separator: ",") + ",\n"
    }
    header += "};\n}\n"
    try header.write(to: output.appendingPathComponent(file + ".h"), atomically: true, encoding: .utf8)
    guard let destination = CGImageDestinationCreateWithURL(output.appendingPathComponent(file + ".png") as CFURL,
                                                           UTType.png.identifier as CFString, 1, nil) else {
        fail("cannot write label preview")
    }
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { fail("cannot finish label preview") }
}
