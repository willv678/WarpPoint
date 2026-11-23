import AppKit

/// The menu bar UFO (branding/menubar-icon.svg), drawn in code so the tractor beam's
/// brightness can pulse while a sync runs. A template image: macOS tints it for light
/// and dark menu bars and only its transparency matters.
enum UFOIcon {
  /// beam: 0 = the resting beam, 1 = the brightest pulse.
  static func image(beam: CGFloat = 0, size: CGFloat = 18) -> NSImage {
    let image = NSImage(size: NSSize(width: size, height: size), flipped: true) { _ in
      guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
      ctx.scaleBy(x: size / 18, y: size / 18)
      let black = NSColor.black

      // Dome, with a thin gap carved out above the saucer.
      ctx.beginTransparencyLayer(auxiliaryInfo: nil)
      let dome = NSBezierPath()
      dome.move(to: NSPoint(x: 5.4, y: 7.6))
      dome.line(to: NSPoint(x: 5.4, y: 6.4))
      dome.appendArc(withCenter: NSPoint(x: 9, y: 6.4), radius: 3.6, startAngle: 180, endAngle: 0,
                     clockwise: false)
      dome.line(to: NSPoint(x: 12.6, y: 7.6))
      dome.close()
      black.setFill()
      dome.fill()
      ctx.setBlendMode(.clear)
      NSBezierPath(ovalIn: oval(cx: 9, cy: 8.2, rx: 8.35, ry: 2.85)).fill()
      ctx.setBlendMode(.normal)

      // Saucer, with three windows punched through.
      black.setFill()
      NSBezierPath(ovalIn: oval(cx: 9, cy: 8.2, rx: 7.7, ry: 2.2)).fill()
      ctx.setBlendMode(.clear)
      for (x, y) in [(5.3, 8.25), (9.0, 8.8), (12.7, 8.25)] {
        NSBezierPath(ovalIn: oval(cx: x, cy: y, rx: 0.8, ry: 0.8)).fill()
      }
      ctx.setBlendMode(.normal)
      ctx.endTransparencyLayer()

      // Tractor beam, fading out downwards; brighter while syncing.
      let beamPath = NSBezierPath()
      beamPath.move(to: NSPoint(x: 6.4, y: 11.1))
      beamPath.line(to: NSPoint(x: 11.6, y: 11.1))
      beamPath.line(to: NSPoint(x: 14.6, y: 16.6))
      beamPath.line(to: NSPoint(x: 3.4, y: 16.6))
      beamPath.close()
      let top = 0.55 + 0.40 * beam
      let bottom = 0.06 + 0.24 * beam
      NSGradient(starting: black.withAlphaComponent(top), ending: black.withAlphaComponent(bottom))?
        .draw(in: beamPath, angle: 90)
      return true
    }
    image.isTemplate = true
    return image
  }

  private static func oval(cx: CGFloat, cy: CGFloat, rx: CGFloat, ry: CGFloat) -> NSRect {
    NSRect(x: cx - rx, y: cy - ry, width: rx * 2, height: ry * 2)
  }
}
