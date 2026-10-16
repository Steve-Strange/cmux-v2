import Foundation
import CoreGraphics
import ImageIO
import XCTest

private struct PaletteRGB: Hashable, CustomStringConvertible {
  let red: UInt8
  let green: UInt8
  let blue: UInt8

  var description: String {
    String(
      format: "#%02x%02x%02x",
      Int(red),
      Int(green),
      Int(blue)
    )
  }
}

private struct PalettePixelRect: Hashable {
  let x: Int
  let y: Int
  let width: Int
  let height: Int
}

private struct PaletteGrid {
  let colors: [PaletteRGB]
  let originX: Int
  let originY: Int
  let swatchWidth: Int
  let swatchHeight: Int
}

private struct ColorRoleSpec {
  let name: String
  let row: Int
  let column: Int
  let width: Int
}

private struct TerminalCellGeometry {
  let originX: Int
  let originY: Int
  let width: Int
  let height: Int
}

private struct CursorRoleSample {
  let color: PaletteRGB
  let changedPixelCount: Int
  let bounds: PalettePixelRect
}

private struct GlyphRoleSample {
  let background: PaletteRGB
  let inkPixelCount: Int
  let totalPixelCount: Int
  let inkBounds: PalettePixelRect
  let meanInkDistanceFromBackground: Double
}

private struct ColorRoleCapture {
  let screenshot: XCUIScreenshot
  let colors: [String: PaletteRGB]
  let cursor: CursorRoleSample
  let glyphs: [String: GlyphRoleSample]
  let cellWidth: Int
  let cellHeight: Int
}

// Keep these fixtures synchronized with scripts/terminal-parity-probe.py.
// The palette page registers the terminal's exact pixel cell geometry first,
// so no OCR or window-chrome assumptions participate in role sampling.
private let colorRoleNames = [
  "registration",
  "default-bg", "default-fg", "truecolor-bg", "truecolor-fg",
] + (0..<8).map { "ansi-normal-\($0)" }
  + (0..<8).map { "ansi-bright-\($0)" }
  + (0..<8).map { "ansi-bold-\($0)" }
  + (0..<8).map { "ansi-dim-\($0)" }
  + ["ansi-inverse-fg", "ansi-inverse-bg"]

private let colorRoleSpecs: [ColorRoleSpec] = colorRoleNames.enumerated().map {
  let slotColumns = [1, 27, 53]
  return ColorRoleSpec(
    name: $0.element,
    row: 4 + $0.offset / slotColumns.count,
    column: slotColumns[$0.offset % slotColumns.count] + 16,
    width: 6
  )
}

private let glyphRoleSpecs = [
  ColorRoleSpec(name: "glyph-default", row: 20, column: 17, width: 2),
  ColorRoleSpec(name: "glyph-bold", row: 20, column: 43, width: 2),
  ColorRoleSpec(name: "glyph-dim", row: 20, column: 69, width: 2),
  ColorRoleSpec(name: "glyph-inverse", row: 21, column: 17, width: 2),
  ColorRoleSpec(name: "glyph-truecolor", row: 21, column: 43, width: 2),
]

private struct PaletteCaptureFailure: Error, CustomStringConvertible {
  let description: String
}

private struct TerminalGridSize: Equatable, CustomStringConvertible {
  let rows: Int
  let columns: Int

  var description: String { "\(columns)x\(rows)" }
}

private struct TerminalProbeState: Decodable, Equatable {
  let generation: Int
  let fenceEpoch: UInt64
  let columns: Int
  let rows: Int
  let page: String
  let overrides: Bool
  let wheelEvents: Int
  let wheelBalance: Int
  let wheelMarkerIndex: Int
  let monotonicNs: UInt64

  var grid: TerminalGridSize {
    TerminalGridSize(rows: rows, columns: columns)
  }

  private enum CodingKeys: String, CodingKey {
    case generation
    case fenceEpoch = "fence_epoch"
    case columns
    case rows
    case page
    case overrides
    case wheelEvents = "wheel_events"
    case wheelBalance = "wheel_balance"
    case wheelMarkerIndex = "wheel_marker_index"
    case monotonicNs = "monotonic_ns"
  }
}

private struct TerminalResizeTraceRecord {
  let view: String
  let sequence: UInt64
  let geometryEpoch: UInt64
  let event: String
  let live: Bool
  let boundsWidth: Int
  let boundsHeight: Int
  let displayedWidth: Int
  let displayedHeight: Int
  let layerWidth: Double
  let layerHeight: Double
  let contentsScale: Double
  let displayedScale: Double
  let transformM11: Double
  let transformM12: Double
  let transformM21: Double
  let transformM22: Double
  let transformM41: Double
  let transformM42: Double
  let transformIdentity: Bool
  let columns: Int
  let rows: Int
  let contentsPresent: Bool
  let anchored: Bool
  let scaleStable: Bool
}

/// Canonical, color-managed pixels decoded directly from an XCUIScreenshot.
///
/// Both screenshots are converted to Display P3 before comparison. This keeps
/// the assertion independent of the PNG's component layout while still
/// catching a missing or incorrect color-space tag on either render path.
private struct PaletteBitmap {
  let width: Int
  let height: Int
  private let rgba: [UInt8]

  init(screenshot: XCUIScreenshot) throws {
    let data = screenshot.pngRepresentation
    guard let source = CGImageSourceCreateWithData(data as CFData, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
      throw PaletteCaptureFailure(description: "could not decode screenshot PNG")
    }
    guard let displayP3 = CGColorSpace(name: CGColorSpace.displayP3) else {
      throw PaletteCaptureFailure(description: "Display P3 color space is unavailable")
    }

    let decodedWidth = image.width
    let decodedHeight = image.height
    var decoded = [UInt8](
      repeating: 0,
      count: decodedWidth * decodedHeight * 4
    )
    var didCreateContext = false
    decoded.withUnsafeMutableBytes { bytes in
      guard let context = CGContext(
        data: bytes.baseAddress,
        width: decodedWidth,
        height: decodedHeight,
        bitsPerComponent: 8,
        bytesPerRow: decodedWidth * 4,
        space: displayP3,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
          | CGBitmapInfo.byteOrder32Big.rawValue
      ) else {
        return
      }
      didCreateContext = true
      context.setBlendMode(.copy)
      // CGBitmapContext's backing rows match the top-down PNG rows when the
      // CGImage is drawn directly. Applying the usual AppKit flip here would
      // invert the probe and make its row-zero registration ambiguous.
      context.draw(
        image,
        in: CGRect(x: 0, y: 0, width: decodedWidth, height: decodedHeight)
      )
    }
    guard didCreateContext else {
      throw PaletteCaptureFailure(description: "could not create screenshot bitmap")
    }

    width = decodedWidth
    height = decodedHeight
    rgba = decoded
  }

  private func color(x: Int, y: Int) -> PaletteRGB {
    let offset = (y * width + x) * 4
    return PaletteRGB(
      red: rgba[offset],
      green: rgba[offset + 1],
      blue: rgba[offset + 2]
    )
  }

  func dominantColor() -> PaletteRGB {
    var counts: [PaletteRGB: Int] = [:]
    for y in 0..<height {
      for x in 0..<width {
        counts[color(x: x, y: y), default: 0] += 1
      }
    }
    return counts.max(by: { $0.value < $1.value })!.key
  }

  func nonMatchingPixelCount(
    color expected: PaletteRGB,
    in rect: PalettePixelRect
  ) throws -> Int {
    guard rect.x >= 0,
          rect.y >= 0,
          rect.width > 0,
          rect.height > 0,
          rect.x + rect.width <= width,
          rect.y + rect.height <= height else {
      throw PaletteCaptureFailure(
        description: "invalid \(rect.width)x\(rect.height) sample at "
          + "\(rect.x),\(rect.y) in \(width)x\(height) bitmap"
      )
    }
    var mismatches = 0
    for y in rect.y..<(rect.y + rect.height) {
      for x in rect.x..<(rect.x + rect.width) where color(x: x, y: y) != expected {
        mismatches += 1
      }
    }
    return mismatches
  }

  func differentPixelCount(from other: PaletteBitmap) throws -> Int {
    try differentPixelCount(
      from: other,
      in: PalettePixelRect(x: 0, y: 0, width: width, height: height)
    )
  }

  func differentPixelCount(
    from other: PaletteBitmap,
    in rect: PalettePixelRect
  ) throws -> Int {
    guard width == other.width, height == other.height else {
      throw PaletteCaptureFailure(
        description: "cannot compare \(width)x\(height) and "
          + "\(other.width)x\(other.height) screenshots"
      )
    }
    guard rect.x >= 0,
          rect.y >= 0,
          rect.width > 0,
          rect.height > 0,
          rect.x + rect.width <= width,
          rect.y + rect.height <= height else {
      throw PaletteCaptureFailure(
        description: "invalid \(rect.width)x\(rect.height) comparison at "
          + "\(rect.x),\(rect.y) in \(width)x\(height) bitmap"
      )
    }
    var changed = 0
    for y in rect.y..<(rect.y + rect.height) {
      for x in rect.x..<(rect.x + rect.width) {
        let offset = (y * width + x) * 4
        if rgba[offset] != other.rgba[offset]
          || rgba[offset + 1] != other.rgba[offset + 1]
          || rgba[offset + 2] != other.rgba[offset + 2]
          || rgba[offset + 3] != other.rgba[offset + 3] {
          changed += 1
        }
      }
    }
    return changed
  }

  /// The probe deliberately assigns this unusual hot-pink hue to palette
  /// index 1. Use a deliberately loose mask only to find its rectangle; the
  /// final comparison remains exact for all 256 sampled colors.
  private func isPaletteOneMarker(x: Int, y: Int) -> Bool {
    let value = color(x: x, y: y)
    let red = Int(value.red)
    let green = Int(value.green)
    let blue = Int(value.blue)
    return red >= 220
      && (15...115).contains(green)
      && (50...175).contains(blue)
      && red - green >= 105
      && red - blue >= 60
  }

  private func markerCandidates() -> [PalettePixelRect] {
    var candidates = Set<PalettePixelRect>()

    // A one-point backing-scale window still gives the three-cell marker a
    // run comfortably wider than 12 pixels. Sampling every other scanline
    // makes registration cheap even for a full-screen Browser screenshot.
    for y in stride(from: 0, to: height, by: 2) {
      var runStart: Int?
      for x in 0...width {
        let isMarker = x < width && isPaletteOneMarker(x: x, y: y)
        if isMarker {
          if runStart == nil { runStart = x }
          continue
        }
        guard let start = runStart else { continue }
        let runWidth = x - start
        runStart = nil
        guard runWidth >= 12 else { continue }

        let centerX = start + runWidth / 2
        var top = y
        while top > 0 && isPaletteOneMarker(x: centerX, y: top - 1) {
          top -= 1
        }
        var bottom = y
        while bottom + 1 < height
          && isPaletteOneMarker(x: centerX, y: bottom + 1) {
          bottom += 1
        }

        let candidate = PalettePixelRect(
          x: start,
          y: top,
          width: runWidth,
          height: bottom - top + 1
        )
        let aspect = Double(candidate.width) / Double(candidate.height)
        guard candidate.height >= 8 && (1.2...2.0).contains(aspect) else {
          continue
        }
        candidates.insert(candidate)
      }
    }

    return candidates.sorted {
      if $0.y != $1.y { return $0.y < $1.y }
      return $0.x < $1.x
    }
  }

  private func dominantColor(
    in rect: PalettePixelRect
  ) -> (color: PaletteRGB, count: Int, total: Int)? {
    let insetX = max(1, rect.width / 10)
    let insetY = max(1, rect.height / 10)
    let startX = rect.x + insetX
    let endX = rect.x + rect.width - insetX
    let startY = rect.y + insetY
    let endY = rect.y + rect.height - insetY
    guard startX < endX && startY < endY else { return nil }

    var counts: [PaletteRGB: Int] = [:]
    for y in startY..<endY {
      for x in startX..<endX {
        counts[color(x: x, y: y), default: 0] += 1
      }
    }
    guard let dominant = counts.max(by: { $0.value < $1.value }) else {
      return nil
    }
    return (
      color: dominant.key,
      count: dominant.value,
      total: (endX - startX) * (endY - startY)
    )
  }

  private func validate(_ rect: PalettePixelRect) throws {
    guard rect.x >= 0,
          rect.y >= 0,
          rect.width > 0,
          rect.height > 0,
          rect.x + rect.width <= width,
          rect.y + rect.height <= height else {
      throw PaletteCaptureFailure(
        description: "invalid \(rect.width)x\(rect.height) sample at "
          + "\(rect.x),\(rect.y) in \(width)x\(height) bitmap"
      )
    }
  }

  func exactFlatColor(in rect: PalettePixelRect) throws -> PaletteRGB {
    try validate(rect)
    guard let dominant = dominantColor(in: rect),
          dominant.count == dominant.total else {
      throw PaletteCaptureFailure(
        description: "role sample at \(rect.x),\(rect.y) was not exactly flat"
      )
    }
    return dominant.color
  }

  func cursorRoleSample(
    reference: PalettePixelRect,
    cursor: PalettePixelRect
  ) throws -> CursorRoleSample {
    try validate(reference)
    try validate(cursor)
    guard reference.width == cursor.width,
          reference.height == cursor.height else {
      throw PaletteCaptureFailure(description: "cursor/reference cells differ in size")
    }

    var referenceCounts: [PaletteRGB: Int] = [:]
    var cursorCounts: [PaletteRGB: Int] = [:]
    for y in 0..<cursor.height {
      for x in 0..<cursor.width {
        referenceCounts[
          color(x: reference.x + x, y: reference.y + y),
          default: 0
        ] += 1
        cursorCounts[
          color(x: cursor.x + x, y: cursor.y + y),
          default: 0
        ] += 1
      }
    }

    let positiveDeltas = cursorCounts.compactMap { entry -> (PaletteRGB, Int)? in
      let delta = entry.value - referenceCounts[entry.key, default: 0]
      return delta > 0 ? (entry.key, delta) : nil
    }
    guard let cursorCandidate = positiveDeltas.max(by: { $0.1 < $1.1 }) else {
      throw PaletteCaptureFailure(description: "steady cursor did not change its known cell")
    }

    var points: [(x: Int, y: Int)] = []
    for y in 0..<cursor.height {
      for x in 0..<cursor.width {
        let candidate = color(x: cursor.x + x, y: cursor.y + y)
        let baseline = color(x: reference.x + x, y: reference.y + y)
        if candidate == cursorCandidate.0 && baseline != candidate {
          points.append((x: x, y: y))
        }
      }
    }
    guard !points.isEmpty else {
      throw PaletteCaptureFailure(description: "steady cursor mask was empty")
    }
    let minX = points.map(\.x).min()!
    let maxX = points.map(\.x).max()!
    let minY = points.map(\.y).min()!
    let maxY = points.map(\.y).max()!
    return CursorRoleSample(
      color: cursorCandidate.0,
      changedPixelCount: points.count,
      bounds: PalettePixelRect(
        x: minX,
        y: minY,
        width: maxX - minX + 1,
        height: maxY - minY + 1
      )
    )
  }

  func glyphRoleSample(in rect: PalettePixelRect) throws -> GlyphRoleSample {
    try validate(rect)
    var counts: [PaletteRGB: Int] = [:]
    for y in rect.y..<(rect.y + rect.height) {
      for x in rect.x..<(rect.x + rect.width) {
        counts[color(x: x, y: y), default: 0] += 1
      }
    }
    guard let background = counts.max(by: { $0.value < $1.value }) else {
      throw PaletteCaptureFailure(description: "glyph sample was empty")
    }
    var inkPoints: [(x: Int, y: Int)] = []
    for y in 0..<rect.height {
      for x in 0..<rect.width
        where color(x: rect.x + x, y: rect.y + y) != background.key {
        inkPoints.append((x: x, y: y))
      }
    }
    guard !inkPoints.isEmpty else {
      throw PaletteCaptureFailure(description: "glyph sample contained no visible ink")
    }
    let minX = inkPoints.map(\.x).min()!
    let maxX = inkPoints.map(\.x).max()!
    let minY = inkPoints.map(\.y).min()!
    let maxY = inkPoints.map(\.y).max()!
    let inkDistance = inkPoints.reduce(0) { total, point in
      let ink = color(x: rect.x + point.x, y: rect.y + point.y)
      return total
        + abs(Int(ink.red) - Int(background.key.red))
        + abs(Int(ink.green) - Int(background.key.green))
        + abs(Int(ink.blue) - Int(background.key.blue))
    }
    return GlyphRoleSample(
      background: background.key,
      inkPixelCount: inkPoints.count,
      totalPixelCount: rect.width * rect.height,
      inkBounds: PalettePixelRect(
        x: minX,
        y: minY,
        width: maxX - minX + 1,
        height: maxY - minY + 1
      ),
      meanInkDistanceFromBackground: Double(inkDistance) / Double(inkPoints.count)
    )
  }

  private func grid(from marker: PalettePixelRect) -> PaletteGrid? {
    // The marker is index 1: the second swatch in row zero.
    let originX = marker.x - marker.width
    let originY = marker.y
    guard originX >= 0,
          originY >= 0,
          originX + marker.width * 16 <= width,
          originY + marker.height * 16 <= height else {
      return nil
    }

    var colors: [PaletteRGB] = []
    colors.reserveCapacity(256)
    for index in 0..<256 {
      let swatch = PalettePixelRect(
        x: originX + (index % 16) * marker.width,
        y: originY + (index / 16) * marker.height,
        width: marker.width,
        height: marker.height
      )
      guard let dominant = dominantColor(in: swatch),
            dominant.count == dominant.total else {
        // Text or a modal over any swatch invalidates the whole capture instead
        // of silently turning a covered center pixel into a palette sample.
        return nil
      }
      colors.append(dominant.color)
    }

    guard colors.count == 256,
          Set(colors).count >= 200,
          isPaletteOneMarker(
            x: marker.x + marker.width / 2,
            y: marker.y + marker.height / 2
          ) else {
      return nil
    }
    return PaletteGrid(
      colors: colors,
      originX: originX,
      originY: originY,
      swatchWidth: marker.width,
      swatchHeight: marker.height
    )
  }

  func paletteGrid() throws -> PaletteGrid {
    let candidates = markerCandidates()
    for candidate in candidates {
      if let grid = grid(from: candidate) { return grid }
    }
    throw PaletteCaptureFailure(
      description: "could not find an unobscured 16x16 palette grid in "
        + "\(width)x\(height) screenshot (\(candidates.count) marker candidates)"
    )
  }
}

private struct PaletteCapture {
  let screenshot: XCUIScreenshot
  let bitmap: PaletteBitmap
  let grid: PaletteGrid
  let pixelWidth: Int
  let pixelHeight: Int
}

private struct XCUICmuxSession {
  let root: URL
  let manifest: URL
  let socket: URL
  let stateDirectory: URL
  let session: String
  let binary: URL
}

private struct ProcessResult {
  let status: Int32
  let output: String
}

/// End-to-end coverage for the process-separated terminal renderer.
///
/// Configure a signed dogfood bundle and source checkout with:
///
///   CMUX_XCUI_BUNDLE_ID=com.cmux.app.dogfood.build-my-build
///   CMUX_XCUI_REPO_ROOT=/path/to/cmux-browser
///
/// `CMUX_XCUI_GHOSTTY_APP_PATH` additionally drives the exact pinned Ghostty
/// bundle, both standalone and as a cmux-tui frontend. The harness exports kept
/// XCTest attachments as lossless PNGs, so parity checks never depend on JPEG
/// samples.
final class CmuxTerminalRenderUITests: XCTestCase {
  private let environment = ProcessInfo.processInfo.environment

  private func configuredValue(environment key: String, file: String) -> String? {
    if let value = environment[key], !value.isEmpty { return value }
    return try? String(contentsOfFile: file, encoding: .utf8)
      .trimmingCharacters(in: .whitespacesAndNewlines)
  }

  private lazy var bundleIdentifier = configuredValue(
    environment: "CMUX_XCUI_BUNDLE_ID",
    file: "/tmp/cmux-xcui-bundle-id"
  ) ?? "com.cmux.app"
  private lazy var repositoryRoot = configuredValue(
    environment: "CMUX_XCUI_REPO_ROOT",
    file: "/tmp/cmux-xcui-repo-root"
  ) ?? FileManager.default.currentDirectoryPath
  private lazy var runToken = configuredValue(
    environment: "CMUX_XCUI_RUN_TOKEN",
    file: "/tmp/cmux-xcui-run-token"
  ) ?? UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
  private lazy var pythonExecutable = configuredValue(
    environment: "CMUX_XCUI_PYTHON3",
    file: "/tmp/cmux-xcui-python3"
  ) ?? "/usr/bin/python3"
  private lazy var brokerConfiguration =
    "/tmp/cmux-xcui-broker-\(runToken).json"
  private lazy var app: XCUIApplication = {
    if let path = configuredValue(
      environment: "CMUX_XCUI_APP_PATH",
      file: "/tmp/cmux-xcui-app-path"
    ) {
      return XCUIApplication(
        url: URL(fileURLWithPath: path).resolvingSymlinksInPath()
      )
    }
    return XCUIApplication(bundleIdentifier: bundleIdentifier)
  }()
  private var activeSession: XCUICmuxSession?
  private var lastSessionQueryError = ""
  private var lastResizeFenceError = ""

  override func setUp() {
    continueAfterFailure = false
  }

  override func tearDown() {
    if app.state != .notRunning {
      app.terminate()
      _ = app.wait(for: .notRunning, timeout: 8)
    }
    if let cleanupError = cleanupActiveSession() {
      XCTFail(cleanupError)
    }
    super.tearDown()
  }

  private func runProcess(_ arguments: [String]) -> ProcessResult {
    guard let executable = arguments.first, executable.hasPrefix("/") else {
      return ProcessResult(status: -1, output: "process executable must be absolute")
    }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = Array(arguments.dropFirst())
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    do {
      try process.run()
    } catch {
      return ProcessResult(status: -1, output: String(describing: error))
    }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    return ProcessResult(
      status: process.terminationStatus,
      output: String(data: data, encoding: .utf8)?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    )
  }

  private func waitUntil(
    timeout: TimeInterval,
    pollEvery: TimeInterval = 0.05,
    _ predicate: () -> Bool
  ) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    repeat {
      if predicate() { return true }
      Thread.sleep(forTimeInterval: pollEvery)
    } while Date() < deadline
    return predicate()
  }

  private func typePhysicalKeys(
    _ text: String,
    in application: XCUIApplication? = nil
  ) {
    let target = application ?? app
    for character in text {
      let key = character == " "
        ? XCUIKeyboardKey.space.rawValue
        : String(character)
      target.typeKey(key, modifierFlags: [])
    }
  }

  private func shellQuote(_ value: String) -> String {
    "'" + value.replacingOccurrences(of: "'", with: "'\"'\"'") + "'"
  }

  private func ghosttyApplication(
    requireExactPath: Bool
  ) throws -> XCUIApplication? {
    let bundleIdentifier = configuredValue(
      environment: "CMUX_XCUI_GHOSTTY_BUNDLE_ID",
      file: "/tmp/cmux-xcui-ghostty-bundle-id"
    )
    if let path = configuredValue(
      environment: "CMUX_XCUI_GHOSTTY_APP_PATH",
      file: "/tmp/cmux-xcui-ghostty-app-path"
    ) {
      let url = URL(fileURLWithPath: path, isDirectory: true)
        .resolvingSymlinksInPath()
      guard url.pathExtension == "app",
            let bundle = Bundle(url: url),
            let actualIdentifier = bundle.bundleIdentifier,
            bundleIdentifier == nil || bundleIdentifier == actualIdentifier,
            let executable = bundle.executableURL,
            FileManager.default.isExecutableFile(atPath: executable.path) else {
        throw NSError(
          domain: "CmuxXCUITest",
          code: 5,
          userInfo: [
            NSLocalizedDescriptionKey:
              "CMUX_XCUI_GHOSTTY_APP_PATH is not the configured executable bundle",
          ]
        )
      }
      return XCUIApplication(url: url)
    }
    if requireExactPath {
      throw XCTSkip(
        "set CMUX_XCUI_GHOSTTY_APP_PATH to the exact pinned Ghostty bundle"
      )
    }
    return bundleIdentifier.map(XCUIApplication.init(bundleIdentifier:))
  }

  private func createOwnedSession(testName: String) throws -> XCUICmuxSession {
    let token = runToken.lowercased()
    guard token.range(of: "^[a-f0-9]{24,64}$", options: .regularExpression) != nil else {
      throw NSError(
        domain: "CmuxXCUITest",
        code: 1,
        userInfo: [NSLocalizedDescriptionKey: "CMUX_XCUI_RUN_TOKEN is not 24-64 lowercase hex digits"]
      )
    }
    guard let manifestPath = configuredValue(
      environment: "CMUX_XCUI_SESSION_MANIFEST_\(testName.uppercased().replacingOccurrences(of: "-", with: "_"))",
      file: "/tmp/cmux-xcui-manifest-\(testName)"
    ) else {
      throw NSError(
        domain: "CmuxXCUITest",
        code: 2,
        userInfo: [NSLocalizedDescriptionKey: "no harness-owned session manifest for \(testName)"]
      )
    }
    let manifest = URL(fileURLWithPath: manifestPath).resolvingSymlinksInPath()
    let data = try Data(contentsOf: manifest)
    guard let ownership = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          let manifestToken = ownership["run_token"] as? String,
          let session = ownership["session"] as? String,
          let rootPath = ownership["root"] as? String,
          let socketPath = ownership["socket"] as? String,
          let statePath = ownership["state_dir"] as? String,
          let binaryPath = ownership["binary"] as? String,
          manifestToken == token else {
      throw NSError(
        domain: "CmuxXCUITest",
        code: 3,
        userInfo: [NSLocalizedDescriptionKey: "invalid or cross-run session manifest"]
      )
    }
    let root = URL(fileURLWithPath: rootPath, isDirectory: true).resolvingSymlinksInPath()
    let socket = URL(fileURLWithPath: socketPath).resolvingSymlinksInPath()
    let stateDirectory = URL(
      fileURLWithPath: statePath,
      isDirectory: true
    ).resolvingSymlinksInPath()
    let binary = URL(fileURLWithPath: binaryPath).resolvingSymlinksInPath()
    let temporaryRoot = URL(fileURLWithPath: "/tmp", isDirectory: true)
      .resolvingSymlinksInPath()
    let configuredAppPath = configuredValue(
      environment: "CMUX_XCUI_APP_PATH",
      file: "/tmp/cmux-xcui-app-path"
    )
    let expectedBinary = configuredAppPath.map {
      URL(fileURLWithPath: $0, isDirectory: true)
        .appendingPathComponent("Contents/Helpers/cmux-tui")
        .resolvingSymlinksInPath()
    }
    guard root.deletingLastPathComponent().path == temporaryRoot.path,
          root.lastPathComponent.hasPrefix("cmux-xcui-session-\(token.prefix(12))-"),
          manifest.path == root.appendingPathComponent("ownership.json").path,
          socket.path == root.appendingPathComponent("control.sock").path,
          stateDirectory.path == root.appendingPathComponent("state").path,
          binary.path == expectedBinary?.path,
          FileManager.default.isExecutableFile(atPath: binary.path),
          session.range(
            of: "^cmux-xcui-\(token.prefix(8))-[a-f0-9]{12}$",
            options: .regularExpression
          ) != nil else {
      throw NSError(
        domain: "CmuxXCUITest",
        code: 4,
        userInfo: [NSLocalizedDescriptionKey: "session manifest paths escaped their owned root"]
      )
    }
    return XCUICmuxSession(
      root: root,
      manifest: manifest,
      socket: socket,
      stateDirectory: stateDirectory,
      session: session,
      binary: binary
    )
  }

  private func cleanupActiveSession() -> String? {
    guard let session = activeSession else { return nil }
    activeSession = nil
    let helper = "\(repositoryRoot)/scripts/xcuitest-cmux-session.py"
    let result = runProcess([
      pythonExecutable, helper, "broker-request",
      "--broker-config", brokerConfiguration,
      "--operation", "cleanup",
      "--manifest", session.manifest.path,
      "--timeout", "10",
    ])
    guard result.status == 0 else {
      return "owned cmux-tui session cleanup failed (\(result.status)): \(result.output)"
    }
    guard !FileManager.default.fileExists(atPath: session.root.path) else {
      return "owned cmux-tui session root survived cleanup: \(session.root.path)"
    }
    return nil
  }

  private func sessionOwnedProcessIdentifiers(command: String) -> Set<Int> {
    guard let session = activeSession else {
      lastSessionQueryError = "no active owned cmux-tui session"
      return []
    }
    let helper = "\(repositoryRoot)/scripts/xcuitest-cmux-session.py"
    let result = runProcess([
      pythonExecutable, helper, "broker-request",
      "--broker-config", brokerConfiguration,
      "--operation", "owned-processes",
      "--manifest", session.manifest.path,
      "--process-command", command,
      "--timeout", "2",
    ])
    guard result.status == 0,
          let data = result.output.data(using: .utf8),
          let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let identifiers = value["pids"] as? [NSNumber] else {
      lastSessionQueryError = result.output
      return []
    }
    lastSessionQueryError = ""
    return Set(identifiers.map(\.intValue).filter { $0 > 1 })
  }

  private func sessionOwnsProcess(_ identifier: Int) -> Bool {
    guard identifier > 1, let session = activeSession else { return false }
    let helper = "\(repositoryRoot)/scripts/xcuitest-cmux-session.py"
    let result = runProcess([
      pythonExecutable, helper, "broker-request",
      "--broker-config", brokerConfiguration,
      "--operation", "owns-pid",
      "--manifest", session.manifest.path,
      "--pid", String(identifier),
      "--timeout", "2",
    ])
    guard result.status == 0,
          let data = result.output.data(using: .utf8),
          let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let owned = value["owned"] as? Bool else {
      lastSessionQueryError = result.output
      return false
    }
    lastSessionQueryError = ""
    return owned
  }

  private func sizeParticipatingTUIClients(
    in session: XCUICmuxSession
  ) -> [[String: Any]]? {
    let helper = "\(repositoryRoot)/scripts/xcuitest-cmux-session.py"
    let result = runProcess([
      pythonExecutable, helper, "broker-request",
      "--broker-config", brokerConfiguration,
      "--operation", "list-clients",
      "--manifest", session.manifest.path,
      "--timeout", "2",
    ])
    guard result.status == 0,
          let data = result.output.data(using: .utf8),
          let response = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let clients = response["clients"] as? [[String: Any]] else {
      lastSessionQueryError = result.output
      return nil
    }
    lastSessionQueryError = ""
    return clients.filter {
      $0["kind"] as? String == "tui"
        && $0["size_participating"] as? Bool == true
    }
  }

  private func validatedPid(at path: URL) -> Int? {
    guard let contents = try? String(contentsOf: path, encoding: .ascii),
          let identifier = Int(contents.trimmingCharacters(in: .whitespacesAndNewlines)),
          identifier > 1,
          sessionOwnsProcess(identifier) else {
      return nil
    }
    return identifier
  }

  private func hasOrderedLeftClickPair(
    _ text: String,
    expectedX: Int? = nil,
    expectedY: Int? = nil
  ) -> Bool {
    let expression = try? NSRegularExpression(
      pattern: #"^\d+ (press|release) button=(\d+) x=(\d+) y=(\d+)$"#
    )
    var presses = Set<String>()
    for line in text.split(whereSeparator: \.isNewline).map(String.init) {
      let range = NSRange(line.startIndex..<line.endIndex, in: line)
      guard let match = expression?.firstMatch(in: line, range: range),
            match.numberOfRanges == 5,
            let kindRange = Range(match.range(at: 1), in: line),
            let buttonRange = Range(match.range(at: 2), in: line),
            let xRange = Range(match.range(at: 3), in: line),
            let yRange = Range(match.range(at: 4), in: line),
            line[buttonRange] == "0" else {
        continue
      }
      let coordinate = "\(line[xRange]),\(line[yRange])"
      if let expectedX, let expectedY,
         coordinate != "\(expectedX),\(expectedY)" {
        continue
      }
      if line[kindRange] == "press" {
        presses.insert(coordinate)
      } else if presses.contains(coordinate) {
        return true
      }
    }
    return false
  }

  private func terminal(in application: XCUIApplication) -> XCUIElement {
    let named = application.textFields["Terminal"].firstMatch
    if named.waitForExistence(timeout: 2) { return named }
    let focused = application.textFields
      .matching(NSPredicate(format: "hasKeyboardFocus == true"))
      .firstMatch
    if focused.exists { return focused }
    // The native NSView is intentionally pixel-only today and is flattened
    // into its containing Chromium group in the macOS AX tree. The main
    // window's center is inside terminal content (to the right of the fixed
    // sidebar), so it remains a stable XCUITest pointer/scroll target.
    return application.windows.firstMatch
  }

  private func launchBrowser(testName: String) -> XCUIElement {
    if app.state != .notRunning {
      app.terminate()
      _ = app.wait(for: .notRunning, timeout: 8)
    }
    if let cleanupError = cleanupActiveSession() {
      XCTFail(cleanupError)
    }
    let session: XCUICmuxSession
    do {
      session = try createOwnedSession(testName: testName)
    } catch {
      XCTFail("could not create owned cmux-tui test session: \(error)")
      return app.windows.firstMatch
    }
    activeSession = session
    app.launchEnvironment["CMUX_TUI_SESSION"] = session.session
    app.launchEnvironment["CMUX_TUI_SOCKET"] = session.socket.path
    // cmux-tui reads this inherited variable before opening its durable
    // registry and terminal-host record root. Keeping it under the same owned
    // directory lets both XCTest and the shell harness recover after failure.
    app.launchEnvironment["CMUX_TUI_STATE_DIR"] = session.stateDirectory.path
    if testName == "window-resize" {
      app.launchEnvironment["CMUX_XCUI_TERMINAL_RESIZE_TRACE"] = session.root
        .appendingPathComponent("terminal-resize-trace.log").path
    } else {
      app.launchEnvironment.removeValue(forKey: "CMUX_XCUI_TERMINAL_RESIZE_TRACE")
    }
    app.launchArguments = [
      "--user-data-dir=\(session.root.appendingPathComponent("profile").path)",
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-breakpad",
      "--disable-crash-reporter",
      "--enable-logging=stderr",
      "--log-level=0",
    ]
    app.launch()
    XCTAssertTrue(app.wait(for: .runningForeground, timeout: 15), "cmux did not launch")
    // The initial focused surface can be the browser. Route focus through the
    // same product shortcut a user uses before resolving the terminal AX node.
    app.typeKey(
      XCUIKeyboardKey.rightArrow.rawValue,
      modifierFlags: [.command, .option]
    )
    let element = terminal(in: app)
    XCTAssertTrue(element.exists, "cmux did not expose a terminal interaction surface")
    element.click()
    return element
  }

  private func runTerminalCommand(_ command: String, in terminal: XCUIElement) {
    terminal.click()
    app.typeText(command)
    app.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: [])
  }

  private func terminalGridSize(at path: URL) -> TerminalGridSize? {
    guard let value = try? String(contentsOf: path, encoding: .ascii) else {
      return nil
    }
    let fields = value.split(whereSeparator: \.isWhitespace)
    guard fields.count == 2,
          let rows = Int(fields[0]),
          let columns = Int(fields[1]),
          rows > 0,
          columns > 0 else {
      return nil
    }
    return TerminalGridSize(rows: rows, columns: columns)
  }

  private func terminalProbeState(at path: URL) -> TerminalProbeState? {
    guard let data = try? Data(contentsOf: path) else { return nil }
    return try? JSONDecoder().decode(TerminalProbeState.self, from: data)
  }

  private func terminalResizeTraceRecords(
    at path: URL
  ) -> [TerminalResizeTraceRecord] {
    guard let contents = try? String(contentsOf: path, encoding: .utf8) else {
      return []
    }
    return contents.split(whereSeparator: \.isNewline).compactMap { line in
      var fields: [Substring: Substring] = [:]
      for field in line.split(separator: " ") {
        let pair = field.split(separator: "=", maxSplits: 1)
        if pair.count == 2 { fields[pair[0]] = pair[1] }
      }
      guard let view = fields["view"].map(String.init),
            let sequence = fields["sequence"].flatMap({ UInt64($0) }),
            let geometryEpoch = fields["geometry_epoch"].flatMap({ UInt64($0) }),
            let event = fields["event"].map(String.init),
            let live = fields["live"].flatMap({ Int($0) }),
            let boundsWidth = fields["bounds_width"].flatMap({ Int($0) }),
            let boundsHeight = fields["bounds_height"].flatMap({ Int($0) }),
            let displayedWidth = fields["displayed_width"].flatMap({ Int($0) }),
            let displayedHeight = fields["displayed_height"].flatMap({ Int($0) }),
            let layerWidth = fields["layer_width"].flatMap({ Double($0) }),
            let layerHeight = fields["layer_height"].flatMap({ Double($0) }),
            let contentsScale = fields["contents_scale"].flatMap({ Double($0) }),
            let displayedScale = fields["displayed_scale"].flatMap({ Double($0) }),
            let transformM11 = fields["transform_m11"].flatMap({ Double($0) }),
            let transformM12 = fields["transform_m12"].flatMap({ Double($0) }),
            let transformM21 = fields["transform_m21"].flatMap({ Double($0) }),
            let transformM22 = fields["transform_m22"].flatMap({ Double($0) }),
            let transformM41 = fields["transform_m41"].flatMap({ Double($0) }),
            let transformM42 = fields["transform_m42"].flatMap({ Double($0) }),
            let transformIdentity = fields["transform_identity"].flatMap({ Int($0) }),
            let columns = fields["columns"].flatMap({ Int($0) }),
            let rows = fields["rows"].flatMap({ Int($0) }),
            let contents = fields["contents"].flatMap({ Int($0) }),
            let anchored = fields["anchored"].flatMap({ Int($0) }),
            let scaleStable = fields["scale_stable"].flatMap({ Int($0) }) else {
        return nil
      }
      return TerminalResizeTraceRecord(
        view: view,
        sequence: sequence,
        geometryEpoch: geometryEpoch,
        event: event,
        live: live == 1,
        boundsWidth: boundsWidth,
        boundsHeight: boundsHeight,
        displayedWidth: displayedWidth,
        displayedHeight: displayedHeight,
        layerWidth: layerWidth,
        layerHeight: layerHeight,
        contentsScale: contentsScale,
        displayedScale: displayedScale,
        transformM11: transformM11,
        transformM12: transformM12,
        transformM21: transformM21,
        transformM22: transformM22,
        transformM41: transformM41,
        transformM42: transformM42,
        transformIdentity: transformIdentity == 1,
        columns: columns,
        rows: rows,
        contentsPresent: contents == 1,
        anchored: anchored == 1,
        scaleStable: scaleStable == 1
      )
    }
  }

  private func waitForTerminalProbeState(
    at path: URL,
    after generation: Int,
    timeout: TimeInterval = 3,
    matching predicate: (TerminalProbeState) -> Bool = { _ in true }
  ) -> TerminalProbeState? {
    var observed: TerminalProbeState?
    _ = waitUntil(timeout: timeout) {
      guard let state = self.terminalProbeState(at: path),
            state.generation > generation,
            predicate(state) else {
        return false
      }
      observed = state
      return true
    }
    return observed
  }

  private func settledTerminalProbeState(
    at path: URL,
    startingFrom prior: TerminalProbeState,
    quietFor quietPeriod: TimeInterval = 0.25,
    timeout: TimeInterval = 1.5
  ) -> TerminalProbeState? {
    var latest = prior
    let deadline = Date().addingTimeInterval(timeout)
    var unchangedSince = Date()
    while Date() < deadline {
      Thread.sleep(forTimeInterval: 0.03)
      guard let current = terminalProbeState(at: path) else { continue }
      if current.generation != latest.generation {
        latest = current
        unchangedSince = Date()
      } else if Date().timeIntervalSince(unchangedSince) >= quietPeriod {
        return latest
      }
    }
    return nil
  }

  private func stableChangedTerminalFrame(
    in terminal: XCUIElement,
    differentFrom reference: PaletteBitmap,
    comparisonRect: PalettePixelRect,
    minimumChangedPixels: Int,
    timeout: TimeInterval = 3
  ) throws -> (screenshot: XCUIScreenshot, bitmap: PaletteBitmap, changed: Int)? {
    let deadline = Date().addingTimeInterval(timeout)
    var priorCandidate: PaletteBitmap?
    repeat {
      let screenshot = terminal.screenshot()
      let bitmap = try PaletteBitmap(screenshot: screenshot)
      let changed = try bitmap.differentPixelCount(
        from: reference,
        in: comparisonRect
      )
      if changed >= minimumChangedPixels {
        if let priorCandidate,
           try bitmap.differentPixelCount(
             from: priorCandidate,
             in: comparisonRect
           ) <= 16 {
          return (screenshot, bitmap, changed)
        }
        priorCandidate = bitmap
      } else {
        priorCandidate = nil
      }
      Thread.sleep(forTimeInterval: 0.08)
    } while Date() < deadline
    return nil
  }

  private func terminalResizeTraceFence(at path: URL) -> [String: UInt64] {
    var sequenceByView: [String: UInt64] = [:]
    for record in terminalResizeTraceRecords(at: path) {
      sequenceByView[record.view] = max(
        sequenceByView[record.view] ?? 0,
        record.sequence
      )
    }
    return sequenceByView
  }

  private func waitForTerminalResizeSettled(
    at path: URL,
    after sequenceByView: [String: UInt64],
    timeout: TimeInterval
  ) -> TerminalResizeTraceRecord? {
    var observed: TerminalResizeTraceRecord?
    _ = waitUntil(timeout: timeout) {
      let records = self.terminalResizeTraceRecords(at: path)
      var latestFrameByView: [String: TerminalResizeTraceRecord] = [:]
      for record in records
        where record.event == "frame_size"
          && record.sequence > (sequenceByView[record.view] ?? 0) {
        if record.sequence > (latestFrameByView[record.view]?.sequence ?? 0) {
          latestFrameByView[record.view] = record
        }
      }
      observed = records.last { record in
        guard record.event == "resize_settled",
              let latestFrame = latestFrameByView[record.view] else {
          return false
        }
        return record.sequence > latestFrame.sequence
          && record.geometryEpoch >= latestFrame.geometryEpoch
      }
      return observed != nil
    }
    return observed
  }

  private func terminalProbeStateAfterResize(
    at path: URL,
    startingFrom prior: TerminalProbeState,
    tracePath: URL,
    after traceFence: [String: UInt64],
    timeout: TimeInterval = 3
  ) -> TerminalProbeState? {
    lastResizeFenceError = ""
    guard let settled = waitForTerminalResizeSettled(
      at: tracePath,
      after: traceFence,
      timeout: timeout
    ) else {
      lastResizeFenceError =
        "product did not publish resize_settled after the new frame_size epoch"
      return nil
    }
    guard let stateAtSettle = terminalProbeState(at: path) else {
      lastResizeFenceError =
        "probe state was unavailable after product resize epoch "
          + "\(settled.geometryEpoch)"
      return nil
    }
    let priorGeneration = max(prior.generation, stateAtSettle.generation)
    let priorFenceEpoch = max(prior.fenceEpoch, stateAtSettle.fenceEpoch)

    // The terminal view defers semantic input while an authoritative geometry
    // transition is in flight. Sending this marker only after the product's
    // resize_settled event therefore samples the PTY after that exact product
    // epoch. The probe publishes this state-only marker without repainting, so
    // the held-resize retained-frame oracle remains undisturbed. Its explicit
    // acknowledgment also proves the resize gesture released the mouse and
    // left the terminal as first responder; no focus-repair click is hidden.
    app.typeKey("f", modifierFlags: [])
    var fenced: TerminalProbeState?
    _ = waitUntil(timeout: timeout) {
      guard let current = self.terminalProbeState(at: path),
            current.generation >= priorGeneration,
            current.fenceEpoch > priorFenceEpoch else {
        return false
      }
      fenced = current
      return true
    }
    guard let fenced else {
      lastResizeFenceError =
        "focused probe did not acknowledge its state-only post-settle fence "
          + "after product resize epoch \(settled.geometryEpoch)"
      return nil
    }
    guard let acknowledgedSettle = terminalResizeTraceRecords(at: tracePath)
      .last(where: {
        $0.view == settled.view
          && $0.event == "resize_settled"
          && $0.sequence >= settled.sequence
      }),
      fenced.columns == acknowledgedSettle.columns,
      fenced.rows == acknowledgedSettle.rows else {
      lastResizeFenceError =
        "post-settle probe grid \(fenced.grid) disagreed with product resize "
          + "epoch \(settled.geometryEpoch)"
      return nil
    }
    return fenced
  }

  private func dragWindowBottomRight(
    _ window: XCUIElement,
    to targetSize: CGSize,
    name: String,
    allowConstrainedWidth: Bool = false
  ) -> CGRect {
    let before = window.frame
    let target = CGSize(
      width: max(targetSize.width, 400),
      height: max(targetSize.height, 320)
    )
    var observed = window.frame
    if abs(observed.width - target.width) > 0.25 {
      let rightEdge = window.coordinate(
        withNormalizedOffset: CGVector(dx: 1, dy: 0.5)
      ).withOffset(CGVector(dx: -1, dy: 0))
      rightEdge.press(
        forDuration: 0.08,
        thenDragTo: rightEdge.withOffset(
          CGVector(dx: target.width - observed.width, dy: 0)
        ),
        withVelocity: .slow,
        thenHoldForDuration: 0
      )
      observed = window.frame
    }
    if abs(observed.height - target.height) > 0.25 {
      // Keep the resize handle away from the Dock/screen boundary. A maximized
      // Browser can put its bottom border outside XCUITest's hittable region,
      // while the top edge remains visible. Moving the top edge preserves the
      // opposite edge and drives the same AppKit live-resize callbacks.
      let topEdge = window.coordinate(
        withNormalizedOffset: CGVector(dx: 0.5, dy: 0)
      ).withOffset(CGVector(dx: 0, dy: 1))
      topEdge.press(
        forDuration: 0.08,
        thenDragTo: topEdge.withOffset(
          CGVector(dx: 0, dy: observed.height - target.height)
        ),
        withVelocity: .slow,
        thenHoldForDuration: 0
      )
    }
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        let frame = window.frame
        return (allowConstrainedWidth || abs(frame.width - target.width) <= 3)
          && abs(frame.height - target.height) <= 3
      },
      "\(name) did not resize Browser from \(before.size) to \(target); "
        + "observed \(window.frame.size)"
        + (allowConstrainedWidth ? " (horizontal screen/layout constraint allowed)" : "")
    )
    return window.frame
  }

  private func dragWindowBottomRightHoldingMouse(
    _ window: XCUIElement,
    to targetSize: CGSize,
    name: String
  ) -> CGRect {
    let before = window.frame
    let target = CGSize(
      width: max(targetSize.width, 400),
      height: max(targetSize.height, 320)
    )
    if abs(before.width - target.width) > 0.25 {
      let rightEdge = window.coordinate(
        withNormalizedOffset: CGVector(dx: 1, dy: 0.5)
      ).withOffset(CGVector(dx: -1, dy: 0))
      rightEdge.press(
        forDuration: 0.08,
        thenDragTo: rightEdge.withOffset(
          CGVector(dx: target.width - before.width, dy: 0)
        ),
        withVelocity: .slow,
        thenHoldForDuration: 0
      )
    }
    let observed = window.frame
    let topEdge = window.coordinate(
      withNormalizedOffset: CGVector(dx: 0.5, dy: 0)
    ).withOffset(CGVector(dx: 0, dy: 1))
    // Keep AppKit's live-resize transaction open at the destination long
    // enough for the in-product geometry trace to sample the held state.
    topEdge.press(
      forDuration: 0.08,
      thenDragTo: topEdge.withOffset(
        CGVector(dx: 0, dy: observed.height - target.height)
      ),
      withVelocity: .slow,
      thenHoldForDuration: 2
    )
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        let frame = window.frame
        return abs(frame.width - target.width) <= 3
          && abs(frame.height - target.height) <= 3
      },
      "\(name) did not resize Browser from \(before.size) to \(target); "
        + "observed \(window.frame.size)"
    )
    return window.frame
  }

  private func captureImmediatePalette(
    in terminal: XCUIElement,
    name: String
  ) throws -> PaletteCapture {
    let screenshot = terminal.screenshot()
    do {
      let bitmap = try PaletteBitmap(screenshot: screenshot)
      return PaletteCapture(
        screenshot: screenshot,
        bitmap: bitmap,
        grid: try bitmap.paletteGrid(),
        pixelWidth: bitmap.width,
        pixelHeight: bitmap.height
      )
    } catch {
      saveScreenshot(
        screenshot,
        name: "invalid immediate resize capture \(name)",
        path: "/tmp/cmux-xcui-invalid-resize-\(name).png"
      )
      throw error
    }
  }

  private func saveScreenshot(
    _ screenshot: XCUIScreenshot,
    name: String,
    path: String
  ) {
    let attachment = XCTAttachment(screenshot: screenshot)
    attachment.name = name
    attachment.lifetime = .keepAlways
    add(attachment)
    // The macOS test runner is sandboxed from /tmp writes. The shell harness
    // exports this lossless attachment from the xcresult after the run.
    _ = path
  }

  private func saveFrame(_ frame: CGRect, path: String) {
    let value = [frame.origin.x, frame.origin.y, frame.size.width, frame.size.height]
      .map { String(format: "%.3f", Double($0)) }
      .joined(separator: " ")
    let attachment = XCTAttachment(string: value)
    attachment.name = URL(fileURLWithPath: path).lastPathComponent
    attachment.lifetime = .keepAlways
    add(attachment)
  }

  private func capturePalette(
    in element: XCUIElement,
    name: String,
    timeout: TimeInterval = 5
  ) throws -> PaletteCapture {
    let deadline = Date().addingTimeInterval(timeout)
    var lastScreenshot: XCUIScreenshot?
    var lastFailure = "palette grid did not render"
    repeat {
      let screenshot = element.screenshot()
      lastScreenshot = screenshot
      do {
        let bitmap = try PaletteBitmap(screenshot: screenshot)
        return PaletteCapture(
          screenshot: screenshot,
          bitmap: bitmap,
          grid: try bitmap.paletteGrid(),
          pixelWidth: bitmap.width,
          pixelHeight: bitmap.height
        )
      } catch {
        lastFailure = String(describing: error)
      }
      Thread.sleep(forTimeInterval: 0.1)
    } while Date() < deadline

    if let screenshot = lastScreenshot {
      saveScreenshot(
        screenshot,
        name: "invalid \(name) palette capture",
        path: "/tmp/cmux-xcui-invalid-\(name)-palette.png"
      )
    }
    throw PaletteCaptureFailure(
      description: "\(name) palette remained invalid for \(timeout)s: \(lastFailure)"
    )
  }

  private func terminalGeometry(
    registeredBy capture: PaletteCapture
  ) throws -> TerminalCellGeometry {
    guard capture.grid.swatchWidth.isMultiple(of: 3) else {
      throw PaletteCaptureFailure(description: "palette swatch is not three cells wide")
    }
    let cellWidth = capture.grid.swatchWidth / 3
    let cellHeight = capture.grid.swatchHeight
    let originX = capture.grid.originX - 4 * cellWidth
    let originY = capture.grid.originY - 3 * cellHeight
    guard cellWidth > 0, cellHeight > 0, originX >= 0, originY >= 0 else {
      throw PaletteCaptureFailure(description: "invalid terminal cell registration")
    }
    return TerminalCellGeometry(
      originX: originX,
      originY: originY,
      width: cellWidth,
      height: cellHeight
    )
  }

  private func cellRect(
    row: Int,
    column: Int,
    width: Int,
    geometry: TerminalCellGeometry
  ) -> PalettePixelRect {
    PalettePixelRect(
      x: geometry.originX + (column - 1) * geometry.width,
      y: geometry.originY + (row - 1) * geometry.height,
      width: width * geometry.width,
      height: geometry.height
    )
  }

  private func decodeColorRoles(
    screenshot: XCUIScreenshot,
    registeredBy palette: PaletteCapture
  ) throws -> ColorRoleCapture {
    let bitmap = try PaletteBitmap(screenshot: screenshot)
    guard bitmap.width == palette.pixelWidth,
          bitmap.height == palette.pixelHeight else {
      throw PaletteCaptureFailure(description: "role page changed registered pixel geometry")
    }
    let geometry = try terminalGeometry(registeredBy: palette)
    var colors: [String: PaletteRGB] = [:]
    for role in colorRoleSpecs {
      let whole = cellRect(
        row: role.row,
        column: role.column,
        width: role.width,
        geometry: geometry
      )
      let insetX = geometry.width
      let insetY = max(1, geometry.height / 6)
      let sample = PalettePixelRect(
        x: whole.x + insetX,
        y: whole.y + insetY,
        width: whole.width - 2 * insetX,
        height: whole.height - 2 * insetY
      )
      colors[role.name] = try bitmap.exactFlatColor(in: sample)
    }
    guard let marker = colors["registration"],
          marker.green >= 150,
          Int(marker.green) - Int(marker.red) >= 60,
          Int(marker.green) - Int(marker.blue) >= 15 else {
      throw PaletteCaptureFailure(description: "role registration marker is missing")
    }

    let referenceCell = cellRect(
      row: 18,
      column: 17,
      width: 2,
      geometry: geometry
    )
    let cursorCell = cellRect(
      row: 18,
      column: 20,
      width: 2,
      geometry: geometry
    )
    let cursor = try bitmap.cursorRoleSample(
      reference: referenceCell,
      cursor: cursorCell
    )
    let edgeTolerance = max(2, geometry.width / 3)
    let cursorTouchesTargetEdge = cursor.bounds.x >= geometry.width - edgeTolerance
      && (
        cursor.bounds.x <= geometry.width + edgeTolerance
          || cursor.bounds.x + cursor.bounds.width >= 2 * geometry.width - edgeTolerance
      )
    guard cursor.changedPixelCount >= max(3, geometry.height / 3),
          cursorTouchesTargetEdge,
          cursor.bounds.width <= max(4, geometry.width / 2),
          cursor.bounds.height >= geometry.height / 2 else {
      throw PaletteCaptureFailure(
        description: "known-content cursor did not form a steady edge-aligned bar: "
          + "count=\(cursor.changedPixelCount) bounds=\(cursor.bounds)"
      )
    }

    var glyphs: [String: GlyphRoleSample] = [:]
    for glyph in glyphRoleSpecs {
      glyphs[glyph.name] = try bitmap.glyphRoleSample(
        in: cellRect(
          row: glyph.row,
          column: glyph.column,
          width: glyph.width,
          geometry: geometry
        )
      )
    }
    return ColorRoleCapture(
      screenshot: screenshot,
      colors: colors,
      cursor: cursor,
      glyphs: glyphs,
      cellWidth: geometry.width,
      cellHeight: geometry.height
    )
  }

  private func captureColorRoles(
    in element: XCUIElement,
    name: String,
    registeredBy palette: PaletteCapture,
    resetFrom overrides: ColorRoleCapture? = nil,
    timeout: TimeInterval = 5
  ) throws -> ColorRoleCapture {
    let deadline = Date().addingTimeInterval(timeout)
    var lastScreenshot: XCUIScreenshot?
    var lastFailure = "color-role page did not render"
    repeat {
      let screenshot = element.screenshot()
      lastScreenshot = screenshot
      do {
        let capture = try decodeColorRoles(
          screenshot: screenshot,
          registeredBy: palette
        )
        if let overrides,
           !hasOverrideResetTransition(overrides: overrides, reset: capture) {
          lastFailure = "captured a valid pre-reset color-role generation"
        } else {
          return capture
        }
      } catch {
        lastFailure = String(describing: error)
      }
      Thread.sleep(forTimeInterval: 0.1)
    } while Date() < deadline

    if let screenshot = lastScreenshot {
      saveScreenshot(
        screenshot,
        name: "invalid \(name) color-role capture",
        path: "/tmp/cmux-xcui-invalid-\(name)-roles.png"
      )
    }
    throw PaletteCaptureFailure(
      description: "\(name) roles remained invalid for \(timeout)s: \(lastFailure)"
    )
  }

  private func hasOverrideResetTransition(
    overrides: ColorRoleCapture,
    reset: ColorRoleCapture
  ) -> Bool {
    let resetRoles = ["default-fg", "default-bg", "ansi-normal-1", "ansi-dim-1"]
    let stableRoles = ["registration", "truecolor-bg", "truecolor-fg"]
    return resetRoles.allSatisfy { overrides.colors[$0] != reset.colors[$0] }
      && stableRoles.allSatisfy { overrides.colors[$0] == reset.colors[$0] }
      && overrides.cursor.color != reset.cursor.color
  }

  private func paletteCellCoordinate(
    index: Int,
    capture: PaletteCapture,
    in element: XCUIElement
  ) throws -> XCUICoordinate {
    guard (0..<256).contains(index),
          capture.pixelWidth > 0,
          capture.pixelHeight > 0 else {
      throw PaletteCaptureFailure(description: "invalid palette click geometry")
    }
    let column = index % 16
    let row = index / 16
    let pixelX = capture.grid.originX
      + column * capture.grid.swatchWidth
      + capture.grid.swatchWidth / 2
    let pixelY = capture.grid.originY
      + row * capture.grid.swatchHeight
      + capture.grid.swatchHeight / 2
    let offset = CGVector(
      dx: (CGFloat(pixelX) + 0.5) / CGFloat(capture.pixelWidth),
      dy: (CGFloat(pixelY) + 0.5) / CGFloat(capture.pixelHeight)
    )
    return element.coordinate(withNormalizedOffset: offset)
  }

  private func assertExactPaletteParity(
    browser: PaletteGrid,
    ghostty: PaletteGrid
  ) {
    var mismatches: [(index: Int, browser: PaletteRGB, ghostty: PaletteRGB)] = []
    for index in 0..<256 {
      let browserColor = browser.colors[index]
      let ghosttyColor = ghostty.colors[index]
      if browserColor != ghosttyColor {
        mismatches.append(
          (index: index, browser: browserColor, ghostty: ghosttyColor)
        )
      }
    }

    let details = mismatches.prefix(16).map {
      String(
        format: "0x%02x %@ != %@",
        $0.index,
        $0.browser.description,
        $0.ghostty.description
      )
    }.joined(separator: ", ")
    XCTAssertTrue(
      mismatches.isEmpty,
      "Browser and pinned Ghostty differ at \(mismatches.count)/256 palette "
        + "entries: \(details)"
    )
  }

  private func assertColorRoleSemantics(
    _ capture: ColorRoleCapture,
    palette: PaletteGrid,
    name: String
  ) {
    for index in 0..<8 {
      XCTAssertEqual(
        capture.colors["ansi-normal-\(index)"],
        palette.colors[index],
        "\(name) ANSI normal \(index) did not resolve through palette index \(index)"
      )
      XCTAssertEqual(
        capture.colors["ansi-bright-\(index)"],
        palette.colors[index + 8],
        "\(name) ANSI bright \(index) did not resolve through palette index \(index + 8)"
      )
      let bold = capture.colors["ansi-bold-\(index)"]
      XCTAssertTrue(
        bold == palette.colors[index] || bold == palette.colors[index + 8],
        "\(name) bold ANSI \(index) resolved outside its normal/bright roles: "
          + "\(String(describing: bold))"
      )
    }
    XCTAssertEqual(
      capture.colors["ansi-inverse-fg"],
      capture.colors["ansi-normal-1"],
      "\(name) inverse foreground did not become the flat cell background"
    )
    XCTAssertEqual(
      capture.colors["ansi-inverse-bg"],
      capture.colors["ansi-normal-4"],
      "\(name) non-inverted background did not resolve through ANSI blue"
    )
    guard let defaultGlyph = capture.glyphs["glyph-default"],
          let boldGlyph = capture.glyphs["glyph-bold"],
          let dimGlyph = capture.glyphs["glyph-dim"],
          let inverseGlyph = capture.glyphs["glyph-inverse"] else {
      XCTFail("\(name) is missing semantic glyph fixtures")
      return
    }
    XCTAssertGreaterThanOrEqual(
      Double(boldGlyph.inkPixelCount) / Double(boldGlyph.totalPixelCount),
      0.9 * Double(defaultGlyph.inkPixelCount) / Double(defaultGlyph.totalPixelCount),
      "\(name) bold glyph unexpectedly lost ink coverage"
    )
    XCTAssertLessThan(
      dimGlyph.meanInkDistanceFromBackground,
      0.85 * defaultGlyph.meanInkDistanceFromBackground,
      "\(name) did not visibly dim foreground glyph colors"
    )
    XCTAssertEqual(
      inverseGlyph.background,
      capture.colors["ansi-normal-1"],
      "\(name) inverse glyph did not swap ANSI red into its background"
    )
  }

  private func assertExactColorRoleParity(
    browser: ColorRoleCapture,
    ghostty: ColorRoleCapture
  ) {
    let mismatches = colorRoleNames.compactMap { name -> String? in
      let browserColor = browser.colors[name]
      let ghosttyColor = ghostty.colors[name]
      guard browserColor != ghosttyColor else { return nil }
      return "\(name) \(String(describing: browserColor)) != "
        + String(describing: ghosttyColor)
    }
    XCTAssertTrue(
      mismatches.isEmpty,
      "Browser and attached pinned Ghostty differ at \(mismatches.count)/"
        + "\(colorRoleNames.count) flat color roles: "
        + mismatches.prefix(12).joined(separator: ", ")
    )

    XCTAssertEqual(
      browser.cursor.color,
      ghostty.cursor.color,
      "Browser and attached pinned Ghostty resolved different cursor colors"
    )
    let browserCoverage = Double(browser.cursor.changedPixelCount)
      / Double(browser.cellWidth * browser.cellHeight)
    let ghosttyCoverage = Double(ghostty.cursor.changedPixelCount)
      / Double(ghostty.cellWidth * ghostty.cellHeight)
    XCTAssertLessThanOrEqual(
      abs(browserCoverage - ghosttyCoverage),
      0.08,
      "Browser and attached pinned Ghostty cursor masks differ in coverage"
    )
    let browserCursorWidth = Double(browser.cursor.bounds.width) / Double(browser.cellWidth)
    let ghosttyCursorWidth = Double(ghostty.cursor.bounds.width) / Double(ghostty.cellWidth)
    let browserCursorHeight = Double(browser.cursor.bounds.height) / Double(browser.cellHeight)
    let ghosttyCursorHeight = Double(ghostty.cursor.bounds.height) / Double(ghostty.cellHeight)
    XCTAssertLessThanOrEqual(
      abs(browserCursorWidth - ghosttyCursorWidth),
      0.15,
      "Browser and attached pinned Ghostty cursor masks differ in width"
    )
    XCTAssertLessThanOrEqual(
      abs(browserCursorHeight - ghosttyCursorHeight),
      0.15,
      "Browser and attached pinned Ghostty cursor masks differ in height"
    )

    for glyph in glyphRoleSpecs {
      guard let browserGlyph = browser.glyphs[glyph.name],
            let ghosttyGlyph = ghostty.glyphs[glyph.name] else {
        XCTFail("missing glyph role \(glyph.name)")
        continue
      }
      XCTAssertEqual(
        browserGlyph.background,
        ghosttyGlyph.background,
        "\(glyph.name) resolved a different flat background"
      )
      let browserInk = Double(browserGlyph.inkPixelCount)
        / Double(browserGlyph.totalPixelCount)
      let ghosttyInk = Double(ghosttyGlyph.inkPixelCount)
        / Double(ghosttyGlyph.totalPixelCount)
      XCTAssertLessThanOrEqual(
        abs(browserInk - ghosttyInk),
        0.12,
        "\(glyph.name) antialias-independent ink masks differ in coverage"
      )
      let contrastTolerance = max(
        10,
        0.20 * max(
          browserGlyph.meanInkDistanceFromBackground,
          ghosttyGlyph.meanInkDistanceFromBackground
        )
      )
      XCTAssertLessThanOrEqual(
        abs(
          browserGlyph.meanInkDistanceFromBackground
            - ghosttyGlyph.meanInkDistanceFromBackground
        ),
        contrastTolerance,
        "\(glyph.name) glyph color contrast differs beyond antialias tolerance"
      )
      let browserWidth = Double(browserGlyph.inkBounds.width)
        / Double(browser.cellWidth * glyph.width)
      let ghosttyWidth = Double(ghosttyGlyph.inkBounds.width)
        / Double(ghostty.cellWidth * glyph.width)
      let browserHeight = Double(browserGlyph.inkBounds.height)
        / Double(browser.cellHeight)
      let ghosttyHeight = Double(ghosttyGlyph.inkBounds.height)
        / Double(ghostty.cellHeight)
      XCTAssertLessThanOrEqual(
        abs(browserWidth - ghosttyWidth),
        0.15,
        "\(glyph.name) ink masks differ in width"
      )
      XCTAssertLessThanOrEqual(
        abs(browserHeight - ghosttyHeight),
        0.15,
        "\(glyph.name) ink masks differ in height"
      )
    }
  }

  private func assertOverrideResetTransition(
    overrides: ColorRoleCapture,
    reset: ColorRoleCapture,
    name: String
  ) {
    XCTAssertTrue(
      hasOverrideResetTransition(overrides: overrides, reset: reset),
      "\(name) reset capture did not advance past its override pixels"
    )
    for role in ["default-fg", "default-bg", "ansi-normal-1", "ansi-dim-1"] {
      XCTAssertNotEqual(
        overrides.colors[role],
        reset.colors[role],
        "\(name) did not reset \(role) after OSC 104/110/111"
      )
    }
    for role in ["registration", "truecolor-bg", "truecolor-fg"] {
      XCTAssertEqual(
        overrides.colors[role],
        reset.colors[role],
        "\(name) reset mutated explicit \(role)"
      )
    }
    XCTAssertEqual(
      overrides.cursor.color,
      overrides.colors["truecolor-fg"],
      "\(name) did not apply OSC 12 to the steady cursor"
    )
    XCTAssertNotEqual(
      overrides.cursor.color,
      reset.cursor.color,
      "\(name) did not reset OSC 12 with OSC 112"
    )
  }

  private func assertUnscaledPaletteFrame(
    _ capture: PaletteCapture,
    matches baseline: PaletteCapture,
    name: String
  ) {
    XCTAssertEqual(
      capture.grid.originX,
      baseline.grid.originX,
      "\(name) moved the retained grid horizontally instead of top-left anchoring it"
    )
    XCTAssertEqual(
      capture.grid.originY,
      baseline.grid.originY,
      "\(name) moved the retained grid vertically instead of top-left anchoring it"
    )
    XCTAssertEqual(
      capture.grid.swatchWidth,
      baseline.grid.swatchWidth,
      "\(name) stretched terminal cell pixels horizontally"
    )
    XCTAssertEqual(
      capture.grid.swatchHeight,
      baseline.grid.swatchHeight,
      "\(name) stretched terminal cell pixels vertically"
    )
    assertExactPaletteParity(browser: capture.grid, ghostty: baseline.grid)
  }

  private func assertRetainedFrameGutters(
    _ capture: PaletteCapture,
    baseline: PaletteCapture,
    inspectExactTerminal: Bool,
    name: String
  ) throws {
    let rightGutterWidth = capture.pixelWidth - baseline.pixelWidth
    let bottomGutterHeight = capture.pixelHeight - baseline.pixelHeight
    XCTAssertGreaterThanOrEqual(
      rightGutterWidth,
      0,
      "\(name) unexpectedly narrowed the terminal screenshot"
    )
    XCTAssertGreaterThanOrEqual(
      bottomGutterHeight,
      0,
      "\(name) unexpectedly shortened the terminal screenshot"
    )
    guard inspectExactTerminal else { return }

    let expectedBackground = baseline.bitmap.dominantColor()
    if rightGutterWidth > 0 {
      let mismatches = try capture.bitmap.nonMatchingPixelCount(
        color: expectedBackground,
        in: PalettePixelRect(
          x: baseline.pixelWidth,
          y: 0,
          width: rightGutterWidth,
          height: baseline.pixelHeight
        )
      )
      XCTAssertEqual(
        mismatches,
        0,
        "\(name) painted \(mismatches) non-background pixels into the "
          + "right retained-frame gutter (likely scale-first presentation)"
      )
    }
    if bottomGutterHeight > 0 {
      let mismatches = try capture.bitmap.nonMatchingPixelCount(
        color: expectedBackground,
        in: PalettePixelRect(
          x: 0,
          y: baseline.pixelHeight,
          width: capture.pixelWidth,
          height: bottomGutterHeight
        )
      )
      XCTAssertEqual(
        mismatches,
        0,
        "\(name) painted \(mismatches) non-background pixels into the "
          + "bottom retained-frame gutter (likely scale-first presentation)"
      )
    }
  }

  func testWheelDrivenCounterVisuallyAdvancesAfterEveryBurst() throws {
    let terminal = launchBrowser(testName: "wheel-counter")
    guard let session = activeSession else {
      XCTFail("wheel counter test has no owned cmux-tui session")
      return
    }
    let probe = "\(repositoryRoot)/scripts/terminal-parity-probe.py"
    let pidPath = session.root.appendingPathComponent("wheel-counter-probe.pid")
    let statePath = session.root.appendingPathComponent("wheel-counter-state.json")
    let shellBitmap = try PaletteBitmap(screenshot: terminal.screenshot())
    runTerminalCommand(
      "python3 \(shellQuote(probe)) --wheel-counter "
        + "--pid-file \(shellQuote(pidPath.path)) "
        + "--state-file \(shellQuote(statePath.path))",
      in: terminal
    )

    var probePid: Int?
    var initialState: TerminalProbeState?
    XCTAssertTrue(
      waitUntil(timeout: 8) {
        probePid = self.validatedPid(at: pidPath)
        initialState = self.terminalProbeState(at: statePath)
        return probePid != nil
          && initialState?.page == "wheel-counter"
          && initialState?.wheelEvents == 0
      },
      "wheel counter probe did not publish its owned pid and initial state: "
        + lastSessionQueryError
    )
    guard let ownedProbePid = probePid, var priorState = initialState else { return }
    XCTAssertEqual(priorState.wheelMarkerIndex, 196)

    // NativeViewHost is intentionally flattened in Chromium's macOS AX tree,
    // so `terminal` can be the whole Browser window. Restrict the exact-pixel
    // oracle to the upper terminal canvas containing the probe marker. This
    // excludes both Browser chrome and unrelated macOS notification overlays
    // while retaining most of the 64-by-12-cell marker at every backing scale.
    let markerRegion = PalettePixelRect(
      x: shellBitmap.width / 6,
      y: shellBitmap.height / 12,
      width: shellBitmap.width * 2 / 3,
      height: shellBitmap.height / 2
    )
    let pixelCount = markerRegion.width * markerRegion.height
    let significantPixelChange = max(512, pixelCount / 200)
    guard let initialFrame = try stableChangedTerminalFrame(
      in: terminal,
      differentFrom: shellBitmap,
      comparisonRect: markerRegion,
      minimumChangedPixels: significantPixelChange,
      timeout: 5
    ) else {
      XCTFail("wheel counter probe did not paint a stable initial marker")
      return
    }
    var precedingBitmap = initialFrame.bitmap
    saveScreenshot(
      initialFrame.screenshot,
      name: "cmux wheel counter baseline",
      path: "/tmp/cmux-xcui-wheel-counter-baseline.png"
    )

    let burstCount = 6
    let gesturesPerBurst = 4
    let overallStart = Date()
    var totalRenderLatency: TimeInterval = 0
    var trace: [String] = []
    for burst in 0..<burstCount {
      let priorWheelEvents = priorState.wheelEvents
      let burstStart = Date()
      for _ in 0..<gesturesPerBurst {
        terminal.scroll(
          byDeltaX: 0,
          deltaY: burst.isMultiple(of: 2) ? -120 : 120
        )
      }
      let injectionLatency = Date().timeIntervalSince(burstStart)
      let renderStart = Date()

      guard let acknowledged = waitForTerminalProbeState(
        at: statePath,
        after: priorState.generation,
        timeout: 4,
        matching: { $0.wheelEvents > priorWheelEvents }
      ) else {
        XCTFail(
          "wheel burst \(burst + 1) did not produce a causally observed "
            + "wheel event"
        )
        return
      }
      guard let settledState = settledTerminalProbeState(
        at: statePath,
        startingFrom: acknowledged
      ) else {
        XCTFail("wheel burst \(burst + 1) never reached a quiet probe generation")
        return
      }
      XCTAssertGreaterThan(settledState.wheelEvents, priorWheelEvents)
      let expectedMarker = 16 + ((180 + settledState.wheelEvents * 73) % 216)
      XCTAssertEqual(
        settledState.wheelMarkerIndex,
        expectedMarker,
        "wheel marker was not derived from its published input generation"
      )
      XCTAssertNotEqual(
        settledState.wheelMarkerIndex,
        priorState.wheelMarkerIndex,
        "wheel burst wrapped to the prior marker and lost its visual oracle"
      )

      guard let frame = try stableChangedTerminalFrame(
        in: terminal,
        differentFrom: precedingBitmap,
        comparisonRect: markerRegion,
        minimumChangedPixels: significantPixelChange
      ) else {
        XCTFail(
          "wheel burst \(burst + 1) reached counter "
            + "\(settledState.wheelEvents) without a stable changed frame"
        )
        return
      }
      let renderLatency = Date().timeIntervalSince(renderStart)
      let wallLatency = Date().timeIntervalSince(burstStart)
      totalRenderLatency += renderLatency
      trace.append(
        String(
          format:
            "burst=%d wheel_events=%d->%d marker=%d changed_pixels=%d "
              + "injection_seconds=%.3f render_seconds=%.3f wall_seconds=%.3f",
          burst + 1,
          priorWheelEvents,
          settledState.wheelEvents,
          settledState.wheelMarkerIndex,
          frame.changed,
          injectionLatency,
          renderLatency,
          wallLatency
        )
      )
      XCTAssertLessThan(
        renderLatency,
        5,
        "wheel burst \(burst + 1) took \(renderLatency)s after input injection "
          + "to reach a stable frame"
      )
      if burst == burstCount - 1 {
        saveScreenshot(
          frame.screenshot,
          name: "cmux wheel counter final burst",
          path: "/tmp/cmux-xcui-wheel-counter-final.png"
        )
      }
      precedingBitmap = frame.bitmap
      priorState = settledState
    }

    let elapsed = Date().timeIntervalSince(overallStart)
    app.typeKey("q", modifierFlags: [])
    let quitObserved = waitUntil(timeout: 3) {
      !FileManager.default.fileExists(atPath: pidPath.path)
        && !self.sessionOwnsProcess(ownedProbePid)
    }
    let timing = XCTAttachment(
      string: trace.joined(separator: "\n")
        + String(
          format: "\nwall_seconds=%.3f total_render_seconds=%.3f\n",
          elapsed,
          totalRenderLatency
        )
    )
    timing.name = "cmux-xcui-wheel-counter-timing.txt"
    timing.lifetime = .keepAlways
    add(timing)
    XCTAssertTrue(quitObserved, "wheel counter probe did not receive q promptly")
    XCTAssertLessThan(
      totalRenderLatency,
      20,
      "six wheel bursts took \(totalRenderLatency)s of post-injection render time"
    )
  }

  // Keep htop as a representative timer-driven TUI workload. The adjacent
  // wheel-counter test supplies the deterministic, input-causal frame oracle.
  func testFastHtopWheelBurstRemainsResponsive() throws {
    let terminal = launchBrowser(testName: "render-burst")
    let htopCommand = "/opt/homebrew/bin/htop -d 1"
    let shellBitmap = try PaletteBitmap(screenshot: terminal.screenshot())
    runTerminalCommand(htopCommand, in: terminal)
    var htopProcesses = Set<Int>()
    XCTAssertTrue(
      waitUntil(timeout: 8) {
        htopProcesses = self.sessionOwnedProcessIdentifiers(command: htopCommand)
        return !htopProcesses.isEmpty
      },
      "htop did not enter its alternate-screen update loop: \(lastSessionQueryError)"
    )

    // Process existence alone can race htop's first painted alternate-screen
    // frame. Establish a visual baseline before timing the wheel burst.
    var previousScreenshot: XCUIScreenshot?
    var previousBitmap: PaletteBitmap?
    XCTAssertTrue(
      waitUntil(timeout: 5, pollEvery: 0.1) {
        let screenshot = terminal.screenshot()
        guard let bitmap = try? PaletteBitmap(screenshot: screenshot),
              let changed = try? bitmap.differentPixelCount(from: shellBitmap),
              changed >= max(128, bitmap.width * bitmap.height / 100) else {
          return false
        }
        previousScreenshot = screenshot
        previousBitmap = bitmap
        return true
      },
      "htop process started without painting its alternate-screen UI"
    )
    guard previousScreenshot != nil, var precedingBitmap = previousBitmap else {
      return
    }

    let start = Date()
    var progression: [
      (
        gesture: Int,
        elapsed: TimeInterval,
        injection: TimeInterval,
        capture: TimeInterval,
        changed: Int
      )
    ] = []
    for group in 0..<8 {
      let injectionStart = Date()
      for offset in 0..<3 {
        let index = group * 3 + offset
        terminal.scroll(
          byDeltaX: 0,
          deltaY: index.isMultiple(of: 2) ? -120 : 120
        )
      }
      let injectionLatency = Date().timeIntervalSince(injectionStart)
      let captureStart = Date()
      let screenshot = terminal.screenshot()
      let bitmap = try PaletteBitmap(screenshot: screenshot)
      let changed = try bitmap.differentPixelCount(from: precedingBitmap)
      let captureLatency = Date().timeIntervalSince(captureStart)
      let gestureCount = (group + 1) * 3
      progression.append(
        (
          gesture: gestureCount,
          elapsed: Date().timeIntervalSince(start),
          injection: injectionLatency,
          capture: captureLatency,
          changed: changed
        )
      )
      precedingBitmap = bitmap
      if [3, 12, 21].contains(gestureCount) {
        saveScreenshot(
          screenshot,
          name: "cmux htop visual progression after \(gestureCount) wheel gestures",
          path: "/tmp/cmux-xcui-htop-\(gestureCount).png"
        )
      }
    }
    let elapsed = Date().timeIntervalSince(start)

    let pixelCount = precedingBitmap.width * precedingBitmap.height
    let significantPixelChange = max(128, pixelCount / 1_000)
    let intermediateProgress = progression.filter {
      $0.gesture < 24 && $0.changed >= significantPixelChange
    }
    let maximumCaptureLatency = progression.map(\.capture).max() ?? 0

    // A key sent behind the wheel burst must not sit behind seconds of stale
    // renderer work. This catches both an overfilled semantic lane and a UI
    // thread monopolized by per-frame layer transactions.
    let quitStart = Date()
    app.typeKey("q", modifierFlags: [])
    let quitObserved = waitUntil(timeout: 3) {
      self.sessionOwnedProcessIdentifiers(command: htopCommand)
        .isDisjoint(with: htopProcesses)
    }
    let quitLatency = Date().timeIntervalSince(quitStart)
    let progressionTrace = progression.map {
      String(
        format:
          "gesture=%d elapsed_seconds=%.3f injection_seconds=%.3f "
            + "capture_seconds=%.3f changed_pixels=%d",
        $0.gesture,
        $0.elapsed,
        $0.injection,
        $0.capture,
        $0.changed
      )
    }.joined(separator: "\n")
    let timing = XCTAttachment(
      string: progressionTrace + "\n" + String(
        format: "wheel_elapsed_seconds=%.3f\nquit_latency_seconds=%.3f\n",
        elapsed,
        quitLatency
      )
    )
    timing.name = "cmux-xcui-htop-timing.txt"
    timing.lifetime = .keepAlways
    add(timing)
    XCTAssertTrue(
      quitObserved,
      "htop did not receive q within three seconds after the wheel burst"
    )
    XCTAssertGreaterThanOrEqual(
      intermediateProgress.count,
      4,
      "htop produced only \(intermediateProgress.count) materially changed "
        + "intermediate frames during the wheel burst (threshold "
        + "\(significantPixelChange) pixels)"
    )
    XCTAssertTrue(
      intermediateProgress.contains { $0.gesture <= 6 },
      "htop did not visually progress near the beginning of the wheel burst"
    )
    XCTAssertTrue(
      intermediateProgress.contains { $0.gesture >= 18 },
      "htop stopped visually progressing before the wheel burst completed"
    )
    XCTAssertLessThanOrEqual(
      maximumCaptureLatency,
      3,
      "htop frame capture took \(maximumCaptureLatency)s after an injected trio"
    )
  }

  func testLiveWindowResizeRetainsUnscaledTerminalFrameAndRecoversGrid() throws {
    let terminal = launchBrowser(testName: "window-resize")
    // Current builds expose the hosted NSView as a named AX node, which lets
    // this test inspect the newly exposed background gutters exactly. Older
    // comparison bundles flatten it into the Browser window; those still run
    // the blank/stretch palette and final-grid assertions below.
    let hasExactTerminalNode = app.textFields["Terminal"].firstMatch.exists
    guard let session = activeSession else {
      XCTFail("window resize test has no owned cmux-tui session")
      return
    }
    let probe = "\(repositoryRoot)/scripts/terminal-parity-probe.py"
    let pidPath = session.root.appendingPathComponent("window-resize-probe.pid")
    let statePath = session.root.appendingPathComponent("window-resize-state.json")
    let tracePath = session.root.appendingPathComponent("terminal-resize-trace.log")
    let initialGridPath = session.root.appendingPathComponent("initial-grid.txt")
    let finalGridPath = session.root.appendingPathComponent("final-grid.txt")
    runTerminalCommand(
      "stty size > \(shellQuote(initialGridPath.path)); "
        + "python3 \(shellQuote(probe)) --palette "
        + "--pid-file \(shellQuote(pidPath.path)) "
        + "--state-file \(shellQuote(statePath.path))",
      in: terminal
    )

    var probePid: Int?
    var initialGrid: TerminalGridSize?
    var initialProbeState: TerminalProbeState?
    XCTAssertTrue(
      waitUntil(timeout: 8) {
        probePid = self.validatedPid(at: pidPath)
        initialGrid = self.terminalGridSize(at: initialGridPath)
        initialProbeState = self.terminalProbeState(at: statePath)
        return probePid != nil && initialGrid != nil && initialProbeState != nil
      },
      "resize probe did not publish its pid, initial grid, and render state: "
        + lastSessionQueryError
    )
    XCTAssertEqual(
      initialProbeState?.grid,
      initialGrid,
      "shell and parity probe disagreed about the initial PTY grid"
    )

    let window = app.windows.firstMatch
    XCTAssertTrue(window.exists, "cmux did not expose its Browser window")
    let initialWindowFrame = window.frame
    let initialTerminalFrame = terminal.frame
    XCTAssertGreaterThan(initialTerminalFrame.width, 500, "terminal is too narrow for resize QA")
    XCTAssertGreaterThan(initialTerminalFrame.height, 360, "terminal is too short for resize QA")
    let initialCapture = try capturePalette(in: terminal, name: "resize baseline")
    saveFrame(initialWindowFrame, path: "/tmp/cmux-xcui-resize-initial-window.txt")
    saveFrame(initialTerminalFrame, path: "/tmp/cmux-xcui-resize-initial-terminal.txt")
    saveScreenshot(
      initialCapture.screenshot,
      name: "cmux terminal before live resize",
      path: "/tmp/cmux-xcui-resize-initial.png"
    )

    let horizontalScale = CGFloat(initialCapture.pixelWidth) / initialTerminalFrame.width
    let verticalScale = CGFloat(initialCapture.pixelHeight) / initialTerminalFrame.height
    let paletteRight = initialCapture.grid.originX
      + 16 * initialCapture.grid.swatchWidth
    let paletteBottom = initialCapture.grid.originY
      + 16 * initialCapture.grid.swatchHeight
    let safeWidthReduction = CGFloat(
      max(initialCapture.pixelWidth - paletteRight - 24, 0)
    ) / horizontalScale
    let safeHeightReduction = CGFloat(
      max(initialCapture.pixelHeight - paletteBottom - 24, 0)
    ) / verticalScale
    let majorWidthReduction = min(180, floor(safeWidthReduction * 0.75))
    let majorHeightReduction = min(120, floor(safeHeightReduction * 0.75))
    XCTAssertGreaterThanOrEqual(
      majorWidthReduction,
      48,
      "terminal has insufficient horizontal room around the parity grid"
    )
    XCTAssertGreaterThanOrEqual(
      majorHeightReduction,
      36,
      "terminal has insufficient vertical room around the parity grid"
    )

    // Derive a sub-cell movement from the registered terminal pixels instead
    // of assuming a fixed point delta. Then probe the PTY state on both sides:
    // depending on its current fractional remainder, even a one-pixel grow
    // can cross a row or column boundary. At most one boundary per dimension
    // can occur over each quarter-cell step, so advancing the baseline finds
    // a verified same-grid pair without guessing the layout remainder.
    guard let initialState = initialProbeState else { return }
    let registeredGeometry = try terminalGeometry(registeredBy: initialCapture)
    let cellWidthPoints = CGFloat(registeredGeometry.width) / horizontalScale
    let cellHeightPoints = CGFloat(registeredGeometry.height) / verticalScale
    let microGrowth = CGSize(
      width: max(
        1,
        floor(CGFloat(registeredGeometry.width) * 0.25) / horizontalScale
      ),
      height: max(
        1,
        floor(CGFloat(registeredGeometry.height) * 0.25) / verticalScale
      )
    )
    XCTAssertLessThan(microGrowth.width, cellWidthPoints)
    XCTAssertLessThan(microGrowth.height, cellHeightPoints)
    XCTAssertLessThanOrEqual(microGrowth.width * 4, cellWidthPoints)
    XCTAssertLessThanOrEqual(microGrowth.height * 4, cellHeightPoints)

    // Keep the current width as the floor: two-pane Browser layouts can
    // legitimately launch at their horizontal minimum. Reduce height to
    // establish vertical room, then search upward in both dimensions.
    let microBaseSize = CGSize(
      width: initialWindowFrame.width,
      height: initialWindowFrame.height
        - min(majorHeightReduction / 4, max(2 * cellHeightPoints, 8))
    )
    let microBaseTraceFence = terminalResizeTraceFence(at: tracePath)
    _ = dragWindowBottomRight(window, to: microBaseSize, name: "micro baseline")
    guard var searchBaseState = terminalProbeStateAfterResize(
      at: statePath,
      startingFrom: initialState,
      tracePath: tracePath,
      after: microBaseTraceFence
    ) else {
      XCTFail(
        "probe did not publish the micro-baseline PTY grid: "
          + lastResizeFenceError
      )
      return
    }
    var searchBaseSize = window.frame.size
    let searchBaseCapture = try capturePalette(
      in: terminal,
      name: "micro-baseline-settled"
    )
    assertUnscaledPaletteFrame(
      searchBaseCapture,
      matches: initialCapture,
      name: "settled micro baseline"
    )

    var verifiedBaseSize: CGSize?
    var verifiedTargetSize: CGSize?
    var verifiedBaseState: TerminalProbeState?
    var verifiedBaseCapture: PaletteCapture?
    var verifiedHorizontalResize = false
    for attempt in 0..<4 {
      let candidateTarget = CGSize(
        width: searchBaseSize.width + microGrowth.width,
        height: searchBaseSize.height + microGrowth.height
      )
      let candidateTraceFence = terminalResizeTraceFence(at: tracePath)
      let observed = dragWindowBottomRight(
        window,
        to: candidateTarget,
        name: "same-grid search \(attempt + 1)",
        // A two-pane Browser can have an effective minimum width equal to the
        // current display's maximum usable width. Keep exercising the corner
        // and right-edge hit regions, but permit that real AppKit constraint;
        // the vertical edge still drives a live resize and all retained-frame
        // invariants remain observable.
        allowConstrainedWidth: true
      )
      guard let candidateState = terminalProbeStateAfterResize(
        at: statePath,
        startingFrom: searchBaseState,
        tracePath: tracePath,
        after: candidateTraceFence
      ) else {
        XCTFail(
          "probe did not publish same-grid search PTY state \(attempt + 1): "
            + lastResizeFenceError
        )
        return
      }
      let candidateCapture = try capturePalette(
        in: terminal,
        name: "same-grid-search-\(attempt + 1)"
      )
      assertUnscaledPaletteFrame(
        candidateCapture,
        matches: initialCapture,
        name: "same-grid search \(attempt + 1)"
      )

      if candidateState.grid == searchBaseState.grid {
        let candidateSize = observed.size
        let restoreTraceFence = terminalResizeTraceFence(at: tracePath)
        _ = dragWindowBottomRight(
          window,
          to: searchBaseSize,
          name: "restore verified same-grid baseline"
        )
        guard let restoredState = terminalProbeStateAfterResize(
          at: statePath,
          startingFrom: candidateState,
          tracePath: tracePath,
          after: restoreTraceFence
        ) else {
          XCTFail(
            "probe did not republish the restored same-grid baseline: "
              + lastResizeFenceError
          )
          return
        }
        XCTAssertEqual(
          restoredState.grid,
          searchBaseState.grid,
          "restoring the verified baseline changed its PTY grid"
        )
        let restoredSize = window.frame.size
        XCTAssertGreaterThan(
          candidateSize.height - restoredSize.height,
          0.25,
          "verified micro resize did not grow the Browser vertically"
        )
        verifiedHorizontalResize =
          candidateSize.width - restoredSize.width > 0.25
        verifiedBaseSize = restoredSize
        verifiedTargetSize = candidateSize
        verifiedBaseState = restoredState
        verifiedBaseCapture = try capturePalette(
          in: terminal,
          name: "verified-same-grid-baseline"
        )
        break
      }

      searchBaseSize = observed.size
      searchBaseState = candidateState
    }

    guard let liveBaseSize = verifiedBaseSize,
          let liveTargetSize = verifiedTargetSize,
          let liveBaseState = verifiedBaseState,
          let microBaseline = verifiedBaseCapture else {
      XCTFail(
        "could not find a same-grid quarter-cell resize from actual terminal geometry; "
          + "last grid was \(searchBaseState.grid) at \(searchBaseSize)"
      )
      return
    }
    XCTAssertEqual(window.frame.width, liveBaseSize.width, accuracy: 0.25)
    XCTAssertEqual(window.frame.height, liveBaseSize.height, accuracy: 0.25)
    XCTAssertEqual(liveBaseState.grid, searchBaseState.grid)
    assertUnscaledPaletteFrame(
      microBaseline,
      matches: initialCapture,
      name: "verified same-grid baseline"
    )

    let traceBeforeHold = terminalResizeTraceRecords(at: tracePath)
    let holdTraceFence = terminalResizeTraceFence(at: tracePath)
    let liveBaseFrame = window.frame
    let heldFrame = dragWindowBottomRightHoldingMouse(
      window,
      to: liveTargetSize,
      name: "verified same-grid live resize"
    )
    guard let liveState = terminalProbeStateAfterResize(
      at: statePath,
      startingFrom: liveBaseState,
      tracePath: tracePath,
      after: holdTraceFence
    ) else {
      XCTFail(
        "probe did not publish the held-resize destination grid: "
          + lastResizeFenceError
      )
      return
    }
    XCTAssertEqual(
      liveState.grid,
      liveBaseState.grid,
      "verified sub-cell live resize changed the PTY grid"
    )
    XCTAssertEqual(heldFrame.origin.x, liveBaseFrame.origin.x, accuracy: 2)
    XCTAssertEqual(heldFrame.maxY, liveBaseFrame.maxY, accuracy: 2)

    let traceAfterHold = terminalResizeTraceRecords(at: tracePath)
    XCTAssertGreaterThanOrEqual(
      traceAfterHold.count,
      traceBeforeHold.count,
      "terminal resize trace was replaced during the held drag"
    )
    let appendedTrace = Array(traceAfterHold.dropFirst(traceBeforeHold.count))
    let liveByView = Dictionary(
      grouping: appendedTrace.filter { $0.event == "frame_size" && $0.live },
      by: \.view
    )
    guard let selectedLive = liveByView.values.max(by: { $0.count < $1.count }),
          let tracedView = selectedLive.first?.view,
          let tracedBaseline = traceBeforeHold.last(where: {
            $0.view == tracedView && $0.event == "frame_size"
          }),
          let presentedBaseline = traceBeforeHold.last(where: {
            $0.view == tracedView && $0.event == "frame_presented"
          }) else {
      XCTFail(
        "product trace did not observe presentation and live-resize geometry for the terminal view"
      )
      return
    }
    XCTAssertTrue(tracedBaseline.contentsPresent, "baseline IOSurface was missing")
    XCTAssertTrue(presentedBaseline.contentsPresent, "presented IOSurface was missing")
    XCTAssertTrue(presentedBaseline.transformIdentity, "presentation applied a layer transform")
    if verifiedHorizontalResize {
      XCTAssertGreaterThan(
        selectedLive.map(\.boundsWidth).max() ?? 0,
        tracedBaseline.boundsWidth,
        "live-resize trace did not observe horizontal growth"
      )
    } else {
      XCTAssertGreaterThanOrEqual(
        selectedLive.map(\.boundsWidth).max() ?? 0,
        tracedBaseline.boundsWidth,
        "vertically constrained live resize unexpectedly narrowed the terminal"
      )
    }
    XCTAssertGreaterThan(
      selectedLive.map(\.boundsHeight).max() ?? 0,
      tracedBaseline.boundsHeight,
      "live-resize trace did not observe vertical growth"
    )
    let tracedOperation = appendedTrace.filter { $0.view == tracedView }
    XCTAssertFalse(
      tracedOperation.contains { $0.event == "contents_cleared" },
      "held live resize cleared the IOSurface between AppKit geometry callbacks"
    )
    let visibleTrace = tracedOperation.filter {
      $0.event == "frame_size" || $0.event == "frame_presented"
    }
    for record in visibleTrace {
      XCTAssertTrue(record.contentsPresent, "live resize temporarily cleared IOSurface contents")
      XCTAssertTrue(record.anchored, "live resize changed retained-frame gravity")
      XCTAssertTrue(record.scaleStable, "live resize changed retained IOSurface scale")
      XCTAssertTrue(record.transformIdentity, "live resize applied a layer transform")
      XCTAssertEqual(record.transformM11, 1, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM12, 0, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM21, 0, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM22, 1, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM41, 0, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM42, 0, accuracy: 0.000_001)
      XCTAssertGreaterThan(record.layerWidth, 0)
      XCTAssertGreaterThan(record.layerHeight, 0)
      XCTAssertGreaterThan(record.contentsScale, 0)
      XCTAssertEqual(record.contentsScale, record.displayedScale, accuracy: 0.000_001)
      if record.event == "frame_size" && record.live {
        XCTAssertEqual(record.displayedWidth, tracedBaseline.displayedWidth)
        XCTAssertEqual(record.displayedHeight, tracedBaseline.displayedHeight)
        XCTAssertEqual(record.columns, liveBaseState.columns)
        XCTAssertEqual(record.rows, liveBaseState.rows)
      }
    }
    let traceSummary = tracedOperation.map {
      "view=\($0.view) sequence=\($0.sequence) epoch=\($0.geometryEpoch) "
        + "event=\($0.event) live=\($0.live ? 1 : 0) "
        + "bounds=\($0.boundsWidth)x\($0.boundsHeight) "
        + "layer=\($0.layerWidth)x\($0.layerHeight) "
        + "displayed=\($0.displayedWidth)x\($0.displayedHeight) "
        + "scale=\($0.contentsScale)/\($0.displayedScale) "
        + "transform=\($0.transformM11),\($0.transformM12),"
        + "\($0.transformM21),\($0.transformM22),"
        + "\($0.transformM41),\($0.transformM42) "
        + "grid=\($0.columns)x\($0.rows) contents=\($0.contentsPresent ? 1 : 0) "
        + "anchored=\($0.anchored ? 1 : 0) scale_stable=\($0.scaleStable ? 1 : 0)"
    }.joined(separator: "\n")
    let traceAttachment = XCTAttachment(string: traceSummary + "\n")
    traceAttachment.name = "cmux-xcui-held-live-resize-trace.txt"
    traceAttachment.lifetime = .keepAlways
    add(traceAttachment)

    // XCUITest serializes screenshots behind its synthetic gesture. The
    // in-product trace above covers every AppKit live-resize geometry callback;
    // this immediate lossless capture checks the resulting retained pixels.
    let retainedCapture = try captureImmediatePalette(
      in: terminal,
      name: "same-grid-after-held-resize"
    )
    assertUnscaledPaletteFrame(
      retainedCapture,
      matches: microBaseline,
      name: "same-grid frame after held live resize"
    )
    try assertRetainedFrameGutters(
      retainedCapture,
      baseline: microBaseline,
      inspectExactTerminal: hasExactTerminalNode,
      name: "same-grid frame after held live resize"
    )
    if verifiedHorizontalResize {
      XCTAssertGreaterThan(
        retainedCapture.pixelWidth,
        microBaseline.pixelWidth,
        "held same-grid resize did not expose a right gutter"
      )
    } else {
      XCTAssertEqual(
        retainedCapture.pixelWidth,
        microBaseline.pixelWidth,
        "vertical-only live resize unexpectedly changed terminal pixel width"
      )
    }
    XCTAssertGreaterThan(
      retainedCapture.pixelHeight,
      microBaseline.pixelHeight,
      "held same-grid resize did not expose a bottom gutter"
    )
    saveScreenshot(
      retainedCapture.screenshot,
      name: "cmux terminal retained frame immediately after held resize",
      path: "/tmp/cmux-xcui-resize-retained.png"
    )

    // Alternate large shrink/grow steps without sleeping between the pointer
    // action and the lossless capture. Every capture must contain the complete
    // palette at the same physical cell size; a blank transition or stretched
    // retained frame therefore fails at the exact step that exposed it.
    let traceBeforeRapidResize = terminalResizeTraceRecords(at: tracePath)
    let reductions: [(CGFloat, CGFloat)] = [
      (0.45, 0.45),
      (1.00, 1.00),
      (0.25, 0.30),
      (0.80, 0.75),
      (0.00, 0.00),
    ]
    for (index, reduction) in reductions.enumerated() {
      let target = CGSize(
        width: verifiedHorizontalResize
          ? initialWindowFrame.width
            - majorWidthReduction * reduction.0
          : initialWindowFrame.width,
        height: initialWindowFrame.height
          - majorHeightReduction * reduction.1
      )
      let observed = dragWindowBottomRight(
        window,
        to: target,
        name: "rapid step \(index + 1)"
      )
      XCTAssertEqual(
        observed.origin.x,
        initialWindowFrame.origin.x,
        accuracy: 2,
        "edge resize moved the Browser's left edge"
      )
      XCTAssertEqual(
        observed.maxY,
        initialWindowFrame.maxY,
        accuracy: 2,
        "top-edge resize moved the Browser's opposite edge"
      )
      let capture = try captureImmediatePalette(
        in: terminal,
        name: "rapid-step-\(index + 1)"
      )
      assertUnscaledPaletteFrame(
        capture,
        matches: initialCapture,
        name: "rapid resize step \(index + 1)"
      )
      saveFrame(
        observed,
        path: "/tmp/cmux-xcui-resize-window-step-\(index + 1).txt"
      )
      if index == 1 {
        saveScreenshot(
          capture.screenshot,
          name: "cmux terminal at deepest rapid resize",
          path: "/tmp/cmux-xcui-resize-deepest.png"
        )
      }
    }

    let traceAfterRapidResize = terminalResizeTraceRecords(at: tracePath)
    XCTAssertGreaterThanOrEqual(
      traceAfterRapidResize.count,
      traceBeforeRapidResize.count,
      "terminal resize trace was replaced during rapid resize"
    )
    let rapidResizeTrace = traceAfterRapidResize
      .dropFirst(traceBeforeRapidResize.count)
      .filter { $0.view == tracedView }
    XCTAssertTrue(
      rapidResizeTrace.contains { $0.event == "frame_size" },
      "rapid resize did not reach the traced terminal view"
    )
    XCTAssertFalse(
      rapidResizeTrace.contains { $0.event == "contents_cleared" },
      "rapid resize temporarily cleared the terminal IOSurface"
    )
    for record in rapidResizeTrace
      where record.event == "frame_size" || record.event == "frame_presented" {
      XCTAssertTrue(record.contentsPresent, "rapid resize observed an empty IOSurface")
      XCTAssertTrue(record.anchored, "rapid resize changed retained-frame gravity")
      XCTAssertTrue(record.scaleStable, "rapid resize changed retained IOSurface scale")
      XCTAssertTrue(record.transformIdentity, "rapid resize applied a layer transform")
      XCTAssertEqual(record.transformM11, 1, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM12, 0, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM21, 0, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM22, 1, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM41, 0, accuracy: 0.000_001)
      XCTAssertEqual(record.transformM42, 0, accuracy: 0.000_001)
      XCTAssertGreaterThan(record.layerWidth, 0)
      XCTAssertGreaterThan(record.layerHeight, 0)
      XCTAssertGreaterThan(record.contentsScale, 0)
      XCTAssertEqual(record.contentsScale, record.displayedScale, accuracy: 0.000_001)
    }

    XCTAssertEqual(window.frame.width, initialWindowFrame.width, accuracy: 3)
    XCTAssertEqual(window.frame.height, initialWindowFrame.height, accuracy: 3)
    let recoveredCapture = try capturePalette(in: terminal, name: "resize recovered")
    assertUnscaledPaletteFrame(
      recoveredCapture,
      matches: initialCapture,
      name: "recovered frame"
    )
    saveScreenshot(
      recoveredCapture.screenshot,
      name: "cmux terminal after resize recovery",
      path: "/tmp/cmux-xcui-resize-recovered.png"
    )

    terminal.click()
    app.typeKey("q", modifierFlags: [])
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        guard let identifier = probePid else { return false }
        return !self.sessionOwnsProcess(identifier)
      },
      "resize probe did not receive q after window recovery"
    )
    runTerminalCommand(
      "stty size > \(shellQuote(finalGridPath.path))",
      in: terminal
    )
    var finalGrid: TerminalGridSize?
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        finalGrid = self.terminalGridSize(at: finalGridPath)
        return finalGrid != nil
      },
      "shell did not publish its final PTY grid"
    )
    XCTAssertEqual(
      finalGrid,
      initialGrid,
      "returning Browser to its exact initial frame did not recover the PTY grid"
    )
  }

  func testClickedTerminalAcceptsPhysicalPrintableKeys() {
    let terminal = launchBrowser(testName: "physical-typing")
    // `env` resolves the external sleep binary while keeping every physically
    // typed character to simple printable letters, digits, and spaces.
    let command = "env sleep 42421"
    var launchedSleepProcesses = Set<Int>()

    // Prove the click, rather than the preceding product shortcut, establishes
    // first responder. Cmd-L is intentionally not used: cmux owns that
    // shortcut while a native terminal is active. Moving foreground ownership
    // to Finder is observable and cannot leave the terminal first responder;
    // the one Terminal click below must both reactivate cmux and focus its
    // native terminal surface.
    let finder = XCUIApplication(bundleIdentifier: "com.apple.finder")
    finder.activate()
    XCTAssertTrue(
      finder.wait(for: .runningForeground, timeout: 3),
      "could not move foreground focus away from cmux"
    )
    XCTAssertNotEqual(app.state, .runningForeground, "cmux remained foreground before click")
    terminal.click()
    XCTAssertTrue(
      app.wait(for: .runningForeground, timeout: 3),
      "Terminal click did not reactivate cmux"
    )

    // Send one XCUITest
    // keyDown per character so this cannot pass through the committed-text
    // shortcut used by typeText(). The session-scoped process oracle starts at
    // process-info's PTY pid and follows only that process tree, so unrelated
    // or leaked system `sleep` processes can neither pass nor be killed here.
    typePhysicalKeys(command)
    app.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: [])
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        launchedSleepProcesses = self.sessionOwnedProcessIdentifiers(command: "sleep 42421")
        return !launchedSleepProcesses.isEmpty
      },
      "click-focused terminal did not execute physically typed printable keys: "
        + lastSessionQueryError
    )

    app.typeKey("c", modifierFlags: .control)
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        self.sessionOwnedProcessIdentifiers(command: "sleep 42421")
          .isDisjoint(with: launchedSleepProcesses)
      },
      "click-focused terminal did not receive Ctrl-C: \(lastSessionQueryError)"
    )
  }

  func testTerminalClickReachesTuiPtyAsSgrMousePressAndRelease() {
    let terminal = launchBrowser(testName: "mouse-events")
    let probe = "\(repositoryRoot)/scripts/terminal-parity-probe.py"
    guard let session = activeSession else {
      XCTFail("mouse test has no owned cmux-tui session")
      return
    }
    let pidPath = session.root.appendingPathComponent("mouse.pid")
    let logPath = session.root.appendingPathComponent("mouse.log")

    runTerminalCommand(
      "python3 \(shellQuote(probe)) --log \(shellQuote(logPath.path)) "
        + "--pid-file \(shellQuote(pidPath.path))",
      in: terminal
    )
    var probePid: Int?
    XCTAssertTrue(
      waitUntil(timeout: 8) {
        probePid = self.validatedPid(at: pidPath)
        return probePid != nil
      },
      "terminal mouse probe did not start as a session-owned process: "
        + lastSessionQueryError
    )

    // NativeViewHost intentionally flattens the pixel-only Ghostty NSView in
    // Chromium's macOS AX tree. Use the named node when a platform revision
    // exposes it; otherwise the main window's center remains inside terminal
    // content, to the right of the fixed rail and below the tab strip. The PTY
    // log below—not the chosen accessibility shape—is the mouse-event oracle.
    let clickTarget = self.terminal(in: app)
    XCTAssertGreaterThan(clickTarget.frame.width, 64, "terminal target is too narrow")
    XCTAssertGreaterThan(clickTarget.frame.height, 64, "terminal target is too short")
    clickTarget.coordinate(
      withNormalizedOffset: CGVector(dx: 0.65, dy: 0.55)
    ).click()
    app.typeKey("q", modifierFlags: [])
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        guard let identifier = probePid else { return false }
        return !self.sessionOwnsProcess(identifier)
      },
      "mouse probe did not receive q after the terminal click"
    )

    let events = (try? String(contentsOf: logPath, encoding: .utf8)) ?? ""
    XCTAssertTrue(
      hasOrderedLeftClickPair(events),
      "terminal click did not emit an ordered same-cell SGR left press/release: \(events)"
    )
  }

  func testPinnedGhosttyAttachedFrontendColorMouseAndPhysicalInput() throws {
    let terminal = launchBrowser(
      testName: "attached-ghostty"
    )
    guard let session = activeSession else {
      XCTFail("attached Ghostty test has no owned cmux-tui session")
      return
    }
    guard let ghostty = try ghosttyApplication(requireExactPath: true) else {
      throw XCTSkip("the exact pinned Ghostty bundle is not configured")
    }
    defer {
      if ghostty.state != .notRunning {
        ghostty.terminate()
        _ = ghostty.wait(for: .notRunning, timeout: 8)
      }
    }
    if ghostty.state != .notRunning {
      ghostty.terminate()
      XCTAssertTrue(
        ghostty.wait(for: .notRunning, timeout: 8),
        "the exact pinned Ghostty bundle did not terminate before the test"
      )
    }

    let attachCommand = "exec \(shellQuote(session.binary.path)) attach "
      + "--session \(shellQuote(session.session)) "
      + "--socket \(shellQuote(session.socket.path))"

    // Launch Ghostty into its normal login shell, then type a short command
    // which expands the full attach invocation from the launch environment.
    // Typing the complete path/session/socket tuple in one XCTest event can
    // exceed macOS's event-synthesis deadline before Ghostty ever attaches.
    // Passing the helper through Ghostty's executable-launch arguments can
    // also trigger a system approval sheet, which covers the pixels under test.
    ghostty.launchArguments = []
    ghostty.launchEnvironment["CMUX_XCUI_ATTACH_COMMAND"] = attachCommand
    ghostty.launch()
    XCTAssertTrue(
      ghostty.wait(for: .runningForeground, timeout: 12),
      "the exact pinned Ghostty bundle did not launch"
    )
    let ghosttyWindow = ghostty.windows.firstMatch
    XCTAssertTrue(
      ghosttyWindow.waitForExistence(timeout: 8),
      "the exact pinned Ghostty bundle did not create a window"
    )
    let permissionMonitor = addUIInterruptionMonitor(
      withDescription: "Ghostty accessibility permission"
    ) { alert in
      let deny = alert.buttons["Deny"]
      guard deny.exists else { return false }
      deny.click()
      return true
    }
    defer { removeUIInterruptionMonitor(permissionMonitor) }
    ghosttyWindow.click()
    ghostty.typeText("eval \"$CMUX_XCUI_ATTACH_COMMAND\"")
    ghostty.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: [])

    var attachedClients: [[String: Any]] = []
    XCTAssertTrue(
      waitUntil(timeout: 10) {
        guard let clients = self.sizeParticipatingTUIClients(in: session) else {
          return false
        }
        attachedClients = clients
        return clients.count == 1
      },
      "pinned Ghostty did not become exactly one size-participating kind=tui "
        + "client (found \(attachedClients.count)): \(lastSessionQueryError)"
    )

    let probe = "\(repositoryRoot)/scripts/terminal-parity-probe.py"
    let pidPath = session.root.appendingPathComponent("attached-ghostty-probe.pid")
    let logPath = session.root.appendingPathComponent("attached-ghostty-mouse.log")
    let statePath = session.root.appendingPathComponent("attached-ghostty-state.json")
    app.activate()
    XCTAssertTrue(
      app.wait(for: .runningForeground, timeout: 3),
      "could not reactivate Browser for the shared probe"
    )
    runTerminalCommand(
      "python3 \(shellQuote(probe)) --palette "
        + "--log \(shellQuote(logPath.path)) "
        + "--pid-file \(shellQuote(pidPath.path)) "
        + "--state-file \(shellQuote(statePath.path))",
      in: terminal
    )
    var probePid: Int?
    var initialProbeState: TerminalProbeState?
    XCTAssertTrue(
      waitUntil(timeout: 8) {
        probePid = self.validatedPid(at: pidPath)
        initialProbeState = self.terminalProbeState(at: statePath)
        return probePid != nil && initialProbeState != nil
      },
      "shared palette/mouse probe did not publish owned pid and render state: "
        + lastSessionQueryError
    )

    let browserCapture = try capturePalette(in: terminal, name: "Browser attached")
    saveScreenshot(
      browserCapture.screenshot,
      name: "cmux Browser shared 256-color palette",
      path: "/tmp/cmux-xcui-attached-browser-palette.png"
    )
    ghostty.activate()
    XCTAssertTrue(
      ghostty.wait(for: .runningForeground, timeout: 3),
      "could not activate attached pinned Ghostty"
    )
    let ghosttyCapture = try capturePalette(
      in: ghosttyWindow,
      name: "attached Ghostty"
    )
    saveScreenshot(
      ghosttyCapture.screenshot,
      name: "attached pinned Ghostty shared 256-color palette",
      path: "/tmp/cmux-xcui-attached-ghostty-palette.png"
    )
    assertExactPaletteParity(
      browser: browserCapture.grid,
      ghostty: ghosttyCapture.grid
    )

    // Switch the shared PTY to its role page. Foreground roles are rendered
    // through inverse-video blank cells, allowing exact flat-pixel comparison
    // without depending on font antialiasing. The glyph and cursor fixtures
    // use coverage/bounds masks separately.
    let beforeRolesGeneration = initialProbeState?.generation ?? 0
    ghostty.typeKey("r", modifierFlags: [])
    guard let overrideProbeState = waitForTerminalProbeState(
      at: statePath,
      after: beforeRolesGeneration,
      matching: { $0.page == "roles" && $0.overrides }
    ) else {
      XCTFail("probe did not publish its override-role render generation")
      return
    }
    app.activate()
    XCTAssertTrue(
      app.wait(for: .runningForeground, timeout: 3),
      "could not reactivate Browser for override role capture"
    )
    let browserOverrideRoles = try captureColorRoles(
      in: terminal,
      name: "Browser override",
      registeredBy: browserCapture
    )
    saveScreenshot(
      browserOverrideRoles.screenshot,
      name: "cmux Browser OSC override color roles",
      path: "/tmp/cmux-xcui-attached-browser-override-roles.png"
    )
    ghostty.activate()
    XCTAssertTrue(
      ghostty.wait(for: .runningForeground, timeout: 3),
      "could not activate attached pinned Ghostty for override role capture"
    )
    let ghosttyOverrideRoles = try captureColorRoles(
      in: ghosttyWindow,
      name: "attached Ghostty override",
      registeredBy: ghosttyCapture
    )
    saveScreenshot(
      ghosttyOverrideRoles.screenshot,
      name: "attached pinned Ghostty OSC override color roles",
      path: "/tmp/cmux-xcui-attached-ghostty-override-roles.png"
    )
    assertExactColorRoleParity(
      browser: browserOverrideRoles,
      ghostty: ghosttyOverrideRoles
    )
    assertColorRoleSemantics(
      browserOverrideRoles,
      palette: browserCapture.grid,
      name: "Browser override"
    )
    assertColorRoleSemantics(
      ghosttyOverrideRoles,
      palette: ghosttyCapture.grid,
      name: "attached Ghostty override"
    )

    // `c` emits OSC 104;1 plus OSC 110/111/112. Capture the same role page
    // again and require both frontends to restore their pinned theme roles
    // while explicit truecolors remain unchanged.
    ghostty.typeKey("c", modifierFlags: [])
    guard let resetProbeState = waitForTerminalProbeState(
      at: statePath,
      after: overrideProbeState.generation,
      matching: { $0.page == "roles" && !$0.overrides }
    ) else {
      XCTFail("probe did not publish a post-OSC-reset render generation")
      return
    }
    let resetBarrier = XCTAttachment(
      string: "override_generation=\(overrideProbeState.generation)\n"
        + "reset_generation=\(resetProbeState.generation)\n"
        + "reset_monotonic_ns=\(resetProbeState.monotonicNs)\n"
    )
    resetBarrier.name = "cmux-xcui-color-reset-generation.txt"
    resetBarrier.lifetime = .keepAlways
    add(resetBarrier)
    app.activate()
    XCTAssertTrue(
      app.wait(for: .runningForeground, timeout: 3),
      "could not reactivate Browser for reset role capture"
    )
    let browserResetRoles = try captureColorRoles(
      in: terminal,
      name: "Browser reset",
      registeredBy: browserCapture,
      resetFrom: browserOverrideRoles
    )
    saveScreenshot(
      browserResetRoles.screenshot,
      name: "cmux Browser OSC reset color roles",
      path: "/tmp/cmux-xcui-attached-browser-reset-roles.png"
    )
    ghostty.activate()
    XCTAssertTrue(
      ghostty.wait(for: .runningForeground, timeout: 3),
      "could not activate attached pinned Ghostty for reset role capture"
    )
    let ghosttyResetRoles = try captureColorRoles(
      in: ghosttyWindow,
      name: "attached Ghostty reset",
      registeredBy: ghosttyCapture,
      resetFrom: ghosttyOverrideRoles
    )
    saveScreenshot(
      ghosttyResetRoles.screenshot,
      name: "attached pinned Ghostty OSC reset color roles",
      path: "/tmp/cmux-xcui-attached-ghostty-reset-roles.png"
    )
    assertExactColorRoleParity(
      browser: browserResetRoles,
      ghostty: ghosttyResetRoles
    )
    assertOverrideResetTransition(
      overrides: browserOverrideRoles,
      reset: browserResetRoles,
      name: "Browser"
    )
    assertOverrideResetTransition(
      overrides: ghosttyOverrideRoles,
      reset: ghosttyResetRoles,
      name: "attached pinned Ghostty"
    )

    // Return to the indexed page while overrides remain reset. This both
    // validates OSC 104 parity and restores the coordinate fixture used by
    // the mouse assertion below.
    ghostty.typeKey("p", modifierFlags: [])
    app.activate()
    XCTAssertTrue(
      app.wait(for: .runningForeground, timeout: 3),
      "could not reactivate Browser for reset palette capture"
    )
    let browserResetPalette = try capturePalette(
      in: terminal,
      name: "Browser reset"
    )
    ghostty.activate()
    XCTAssertTrue(
      ghostty.wait(for: .runningForeground, timeout: 3),
      "could not activate attached pinned Ghostty for reset palette capture"
    )
    let ghosttyResetPalette = try capturePalette(
      in: ghosttyWindow,
      name: "attached Ghostty reset"
    )
    assertExactPaletteParity(
      browser: browserResetPalette.grid,
      ghostty: ghosttyResetPalette.grid
    )
    XCTAssertNotEqual(
      browserCapture.grid.colors[1],
      browserResetPalette.grid.colors[1],
      "Browser did not restore palette index 1 after OSC 104;1"
    )
    XCTAssertNotEqual(
      ghosttyCapture.grid.colors[1],
      ghosttyResetPalette.grid.colors[1],
      "attached pinned Ghostty did not restore palette index 1 after OSC 104;1"
    )
    assertColorRoleSemantics(
      browserResetRoles,
      palette: browserResetPalette.grid,
      name: "Browser reset"
    )
    assertColorRoleSemantics(
      ghosttyResetRoles,
      palette: ghosttyResetPalette.grid,
      name: "attached Ghostty reset"
    )

    // Click the center of palette index 0x22. Registration comes from the
    // rendered palette itself, so this cannot accidentally hit TUI chrome or a
    // canonical-size gutter. The probe's known layout maps it to PTY cell 12,6.
    let paletteIndex = 0x22
    let paletteCoordinate = try paletteCellCoordinate(
      index: paletteIndex,
      capture: ghosttyResetPalette,
      in: ghosttyWindow
    )
    paletteCoordinate.click()
    ghostty.typeKey("q", modifierFlags: [])
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        guard let identifier = probePid else { return false }
        return !self.sessionOwnsProcess(identifier)
      },
      "attached TUI did not deliver q to the shared probe"
    )
    let events = (try? String(contentsOf: logPath, encoding: .utf8)) ?? ""
    XCTAssertTrue(
      hasOrderedLeftClickPair(events, expectedX: 12, expectedY: 6),
      "palette-derived attached-TUI click did not emit the expected ordered "
        + "same-cell SGR left press/release: \(events)"
    )

    // Reuse a proven interior surface coordinate after the probe returns to
    // its shell. One keyDown is sent for each printable character; no
    // committed-text shortcut participates in this assertion.
    paletteCoordinate.click()
    let command = "env sleep 42422"
    var launchedSleepProcesses = Set<Int>()
    typePhysicalKeys(command, in: ghostty)
    ghostty.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: [])
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        launchedSleepProcesses = self.sessionOwnedProcessIdentifiers(
          command: "sleep 42422"
        )
        return !launchedSleepProcesses.isEmpty
      },
      "attached TUI did not execute physically typed printable keys: "
        + lastSessionQueryError
    )
    ghostty.typeKey("c", modifierFlags: .control)
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        self.sessionOwnedProcessIdentifiers(command: "sleep 42422")
          .isDisjoint(with: launchedSleepProcesses)
      },
      "attached TUI did not deliver Ctrl-C: \(lastSessionQueryError)"
    )

    // The frontend must disappear before XCTest tears down Browser and the
    // owned daemon. This avoids racing the TUI's last size lease with topology
    // closure and proves the exact Ghostty process detached cleanly.
    ghostty.terminate()
    XCTAssertTrue(
      ghostty.wait(for: .notRunning, timeout: 8),
      "attached pinned Ghostty did not terminate before session cleanup"
    )
    XCTAssertTrue(
      waitUntil(timeout: 3) {
        self.sizeParticipatingTUIClients(in: session)?.isEmpty == true
      },
      "attached TUI client survived exact Ghostty termination: "
        + lastSessionQueryError
    )
  }

  func testCaptureLosslessBrowserAndGhosttyPalettes() throws {
    let terminal = launchBrowser(testName: "color-parity")
    let probe = "\(repositoryRoot)/scripts/terminal-parity-probe.py"
    runTerminalCommand("python3 \(shellQuote(probe)) --palette", in: terminal)
    let browserCapture = try capturePalette(in: terminal, name: "Browser")
    saveFrame(terminal.frame, path: "/tmp/cmux-xcui-browser-terminal-frame.txt")
    saveScreenshot(
      browserCapture.screenshot,
      name: "cmux Browser 256-color palette",
      path: "/tmp/cmux-xcui-browser-palette.png"
    )

    if let ghostty = try ghosttyApplication(requireExactPath: false) {
      defer {
        if ghostty.state != .notRunning {
          ghostty.terminate()
          _ = ghostty.wait(for: .notRunning, timeout: 8)
        }
      }
      if ghostty.state != .notRunning {
        ghostty.terminate()
        _ = ghostty.wait(for: .notRunning, timeout: 8)
      }
      // Type into Ghostty's normal login shell. macOS prompts for explicit
      // approval when a GUI app is launched with an arbitrary executable via
      // `-e`, which would cover the terminal and invalidate the color sample.
      ghostty.launchArguments = []
      ghostty.launch()
      XCTAssertTrue(
        ghostty.wait(for: .runningForeground, timeout: 12),
        "pinned Ghostty reference did not launch"
      )
      XCTAssertTrue(
        ghostty.windows.firstMatch.waitForExistence(timeout: 8),
        "pinned Ghostty reference did not create a window"
      )
      let permissionMonitor = addUIInterruptionMonitor(
        withDescription: "Ghostty accessibility permission"
      ) { alert in
        let deny = alert.buttons["Deny"]
        guard deny.exists else { return false }
        deny.click()
        return true
      }
      defer { removeUIInterruptionMonitor(permissionMonitor) }
      ghostty.windows.firstMatch.click()
      ghostty.typeText("python3 \(shellQuote(probe)) --palette")
      ghostty.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: [])
      // A target-side UI action gives XCUITest's interruption monitor a chance
      // to dismiss the system-owned TCC prompt before the lossless capture.
      ghostty.windows.firstMatch.click()
      let ghosttyCapture = try capturePalette(
        in: ghostty.windows.firstMatch,
        name: "Ghostty"
      )
      saveFrame(
        ghostty.windows.firstMatch.frame,
        path: "/tmp/cmux-xcui-ghostty-window-frame.txt"
      )
      saveScreenshot(
        ghosttyCapture.screenshot,
        name: "pinned Ghostty 256-color palette",
        path: "/tmp/cmux-xcui-ghostty-palette.png"
      )
      let accessibility = XCTAttachment(string: ghostty.debugDescription)
      accessibility.name = "cmux-xcui-ghostty-accessibility.txt"
      accessibility.lifetime = .keepAlways
      add(accessibility)
      assertExactPaletteParity(
        browser: browserCapture.grid,
        ghostty: ghosttyCapture.grid
      )
      ghostty.typeKey("q", modifierFlags: [])
      ghostty.terminate()
    }

    app.activate()
    terminal.click()
    app.typeKey("q", modifierFlags: [])
  }
}
