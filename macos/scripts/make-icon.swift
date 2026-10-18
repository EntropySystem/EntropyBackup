// Renders the hexagon logo into an .iconset: swift scripts/make-icon.swift <dir>.iconset
import AppKit

let out = CommandLine.arguments[1]
try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for (px, name) in [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"), (128, "128x128"), (256, "128x128@2x"),
                   (256, "256x256"), (512, "256x256@2x"), (512, "512x512"), (1024, "512x512@2x")] {
    let s = CGFloat(px)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let hex = NSBezierPath()
    for i in 0..<6 {
        let a = CGFloat(i) * .pi / 3 + .pi / 2, r = s * 0.46
        let p = NSPoint(x: s / 2 + r * cos(a), y: s / 2 + r * sin(a))
        if i == 0 { hex.move(to: p) } else { hex.line(to: p) }
    }
    hex.close()
    NSGradient(starting: NSColor(red: 0.16, green: 0.36, blue: 0.78, alpha: 1), ending: NSColor(red: 0.05, green: 0.12, blue: 0.34, alpha: 1))!.draw(in: hex, angle: -90)
    NSColor(red: 0.92, green: 0.77, blue: 0.42, alpha: 1).setStroke()
    hex.lineWidth = max(1, s * 0.025)
    hex.stroke()
    let config = NSImage.SymbolConfiguration(pointSize: s * 0.36, weight: .regular).applying(.init(paletteColors: [NSColor(red: 0.98, green: 0.89, blue: 0.63, alpha: 1)]))
    if let sym = NSImage(systemSymbolName: "externaldrive.fill.badge.timemachine", accessibilityDescription: nil)?.withSymbolConfiguration(config) {
        let z = sym.size
        sym.draw(in: NSRect(x: (s - z.width) / 2, y: (s - z.height) / 2, width: z.width, height: z.height))
    }
    NSGraphicsContext.restoreGraphicsState()
    try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "\(out)/icon_\(name).png"))
}
