// Draws WarpPoint's app icon (the navy square with the UFO) at every size macOS needs,
// into an .iconset folder for `iconutil`. Run: branding/appicon/make-icon.sh
import AppKit

let out = CommandLine.arguments[1]  // e.g. build-release/AppIcon.iconset

func draw(_ px: CGFloat) -> NSBitmapImageRep {
  let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(px), pixelsHigh: Int(px),
                             bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                             colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
  NSGraphicsContext.saveGraphicsState()
  NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
  let ctx = NSGraphicsContext.current!.cgContext
  // Work on Apple's 1024 grid: the icon body is 824 wide with ~100 of margin.
  ctx.scaleBy(x: px / 1024, y: px / 1024)
  ctx.translateBy(x: 0, y: 1024)
  ctx.scaleBy(x: 1, y: -1)  // y grows downwards, like the SVG

  // Body: a navy rounded square, lighter at the top.
  let body = NSBezierPath(roundedRect: NSRect(x: 100, y: 100, width: 824, height: 824),
                          xRadius: 185, yRadius: 185)
  ctx.saveGState()
  ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 28,
                color: NSColor.black.withAlphaComponent(0.35).cgColor)
  NSColor(srgbRed: 0.16, green: 0.24, blue: 0.40, alpha: 1).setFill()
  body.fill()
  ctx.restoreGState()
  NSGradient(colors: [NSColor(srgbRed: 0.20, green: 0.30, blue: 0.49, alpha: 1),
                      NSColor(srgbRed: 0.11, green: 0.17, blue: 0.30, alpha: 1)])!
    .draw(in: body, angle: 90)  // flipped: 90 runs top to bottom

  // A few faint stars.
  NSColor.white.withAlphaComponent(0.35).setFill()
  for (x, y, r) in [(250.0, 250.0, 5.0), (770, 230, 4), (820, 430, 3), (210, 520, 3), (700, 330, 2.5),
                    (330, 180, 2.5)] {
    NSBezierPath(ovalIn: NSRect(x: x - r, y: y - r, width: r * 2, height: r * 2)).fill()
  }

  ctx.saveGState()
  ctx.addPath(body.cgPath)
  ctx.clip()
  // The tractor beam: white, fading out downwards.
  let beam = NSBezierPath()
  beam.move(to: NSPoint(x: 436, y: 600))
  beam.line(to: NSPoint(x: 588, y: 600))
  beam.line(to: NSPoint(x: 700, y: 924))
  beam.line(to: NSPoint(x: 324, y: 924))
  beam.close()
  NSGradient(starting: NSColor.white.withAlphaComponent(0.55),
             ending: NSColor.white.withAlphaComponent(0.0))!.draw(in: beam, angle: 90)
  ctx.restoreGState()

  // The UFO, same shapes as the menu bar icon, scaled up: dome, gap, saucer, windows.
  ctx.saveGState()
  ctx.translateBy(x: 512 - 9 * 34, y: 250)
  ctx.scaleBy(x: 34, y: 34)  // menu bar icon units (18 wide) -> 612 px
  ctx.setShadow(offset: CGSize(width: 0, height: 0.15), blur: 0.6,
                color: NSColor.black.withAlphaComponent(0.25).cgColor)
  ctx.beginTransparencyLayer(auxiliaryInfo: nil)
  NSColor.white.setFill()
  let dome = NSBezierPath()
  dome.move(to: NSPoint(x: 5.4, y: 7.6))
  dome.line(to: NSPoint(x: 5.4, y: 6.4))
  dome.appendArc(withCenter: NSPoint(x: 9, y: 6.4), radius: 3.6, startAngle: 180, endAngle: 0,
                 clockwise: false)
  dome.line(to: NSPoint(x: 12.6, y: 7.6))
  dome.close()
  dome.fill()
  ctx.setBlendMode(.clear)
  NSBezierPath(ovalIn: NSRect(x: 9 - 8.35, y: 8.2 - 2.85, width: 16.7, height: 5.7)).fill()
  ctx.setBlendMode(.normal)
  NSBezierPath(ovalIn: NSRect(x: 9 - 7.7, y: 8.2 - 2.2, width: 15.4, height: 4.4)).fill()
  ctx.setBlendMode(.clear)
  for (x, y) in [(5.3, 8.25), (9.0, 8.8), (12.7, 8.25)] {
    NSBezierPath(ovalIn: NSRect(x: x - 0.8, y: y - 0.8, width: 1.6, height: 1.6)).fill()
  }
  ctx.endTransparencyLayer()
  ctx.restoreGState()

  NSGraphicsContext.restoreGraphicsState()
  return rep
}

try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for (name, px) in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64),
                   ("128x128", 128), ("128x128@2x", 256), ("256x256", 256),
                   ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)] {
  let data = draw(CGFloat(px)).representation(using: .png, properties: [:])!
  try! data.write(to: URL(fileURLWithPath: "\(out)/icon_\(name).png"))
}
