import AppKit
import CoreGraphics
import XCTest

// Deterministic, app-scoped keyboard E2E for cmux-browser. Drives keys through
// XCUITest (which manages app activation/focus via the automation framework —
// reliable where raw CGEvent was flaky) and verifies via CDP (HTTP) + process
// checks. tools/xcuitest-run.sh launches the configured app/profile and passes
// its exact PID, bundle identifier, and remote-debugging port to this bundle.
final class CmuxKeyboardUITests: XCTestCase {
  private lazy var app = XCUIApplication(bundleIdentifier: bundleIdentifier)
  private let bundleIdentifier =
    ProcessInfo.processInfo.environment["CMUX_XCUI_BUNDLE_ID"] ?? "com.cmux.app"
  private let port =
    Int(ProcessInfo.processInfo.environment["CMUX_PORT"] ?? "9300") ?? 9300

  override func setUp() { continueAfterFailure = true }

  // --- helpers ---------------------------------------------------------------
  func cdpJson() -> [[String: Any]] {
    guard let url = URL(string: "http://127.0.0.1:\(port)/json") else { return [] }
    let sem = DispatchSemaphore(value: 0)
    var out: [[String: Any]] = []
    URLSession.shared.dataTask(with: url) { data, _, _ in
      if let d = data, let a = try? JSONSerialization.jsonObject(with: d) as? [[String: Any]] { out = a }
      sem.signal()
    }.resume()
    _ = sem.wait(timeout: .now() + 5)
    return out
  }
  func pageUrls() -> [String] { cdpJson().filter { ($0["type"] as? String) == "page" }.compactMap { $0["url"] as? String } }
  func devtoolsCount() -> Int { cdpJson().filter { ($0["url"] as? String)?.hasPrefix("devtools://") ?? false }.count }
  func cdpAlive() -> Bool { !cdpJson().isEmpty }
  @discardableResult func shell(_ cmd: String) -> String {
    let p = Process(); p.launchPath = "/bin/bash"; p.arguments = ["-c", cmd]
    let pipe = Pipe(); p.standardOutput = pipe
    try? p.run(); p.waitUntilExit()
    return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
  }
  func pause(_ s: Double) { Thread.sleep(forTimeInterval: s) }
  func typePhysicalKeys(_ text: String) {
    for character in text {
      let key = character == " "
        ? XCUIKeyboardKey.space.rawValue
        : String(character)
      app.typeKey(key, modifierFlags: [])
    }
  }

  private func runnerAppPID() -> pid_t? {
    if
      let value = ProcessInfo.processInfo.environment["CMUX_XCUI_APP_PID"],
      let pid = pid_t(value),
      pid > 0
    {
      return pid
    }

    guard
      let runToken = try? String(
        contentsOfFile: "/tmp/cmux-xcui-run-token",
        encoding: .utf8
      ).trimmingCharacters(in: .whitespacesAndNewlines),
      !runToken.isEmpty,
      let selector = try? String(
        contentsOfFile: "/tmp/cmux-xcui-app-pid",
        encoding: .utf8
      ).trimmingCharacters(in: .whitespacesAndNewlines),
      selector.hasPrefix("\(runToken):"),
      let pid = pid_t(String(selector.dropFirst(runToken.count + 1))),
      pid > 0
    else {
      return nil
    }
    return pid
  }

  private func activateAndVerifyApp() throws {
    guard let expectedPID = runnerAppPID() else {
      throw XCTSkip(
        "Keyboard tests require the standard runner's owned app PID; "
          + "external-app mode does not launch or own that process"
      )
    }
    app.activate()
    XCTAssertTrue(app.wait(for: .runningForeground, timeout: 8), "cmux not foreground")
    XCTAssertTrue(
      waitUntil {
        NSWorkspace.shared.frontmostApplication?.processIdentifier == expectedPID
      },
      "XCUIApplication activated a process other than expected PID \(expectedPID)"
    )
  }

  private func railRows() -> [XCUIElement] {
    // Chromium maps Views' kTab role to AXRadioButton on macOS. CmuxRailRow is
    // the only kTab role in cmux's custom browser chrome, so this deliberately
    // exercises the shipped accessibility surface rather than a test-only ID.
    return app.radioButtons.allElementsBoundByIndex
      .filter { $0.exists && !$0.frame.isEmpty }
      .sorted {
        if abs($0.frame.minY - $1.frame.minY) > 0.5 {
          return $0.frame.minY < $1.frame.minY
        }
        return $0.frame.minX < $1.frame.minX
      }
  }

  private func waitUntil(
    timeout: TimeInterval = 5,
    _ condition: () -> Bool
  ) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    repeat {
      if condition() {
        return true
      }
      RunLoop.current.run(until: Date().addingTimeInterval(0.05))
    } while Date() < deadline
    return condition()
  }

  private func ensureRailRowCount(_ requiredCount: Int) {
    var count = railRows().count
    while count < requiredCount {
      let newWorkspace = app.buttons["New workspace"].firstMatch
      XCTAssertTrue(
        newWorkspace.waitForExistence(timeout: 3),
        "New workspace button is not exposed through accessibility"
      )
      guard newWorkspace.exists else {
        return
      }

      newWorkspace.click()
      let previousCount = count
      let added = waitUntil { self.railRows().count > previousCount }
      XCTAssertTrue(
        added,
        "New workspace click did not add an accessible CmuxRailRow tab"
      )
      guard added else {
        return
      }
      count = railRows().count
    }
  }

  private func postMouseClick(
    on element: XCUIElement,
    button: CGMouseButton = .left,
    modifiers: CGEventFlags = []
  ) {
    XCTAssertTrue(element.exists, "Cannot click a missing rail row")
    let frame = element.frame
    XCTAssertFalse(frame.isEmpty, "Cannot click an empty rail-row frame")
    guard element.exists, !frame.isEmpty else {
      return
    }
    guard let appPID = runnerAppPID() else {
      XCTFail("CMUX_XCUI_APP_PID must identify the app launched by the runner")
      return
    }

    let point = CGPoint(x: frame.midX, y: frame.midY)
    let downType: CGEventType = button == .right ? .rightMouseDown : .leftMouseDown
    let upType: CGEventType = button == .right ? .rightMouseUp : .leftMouseUp
    guard
      let source = CGEventSource(stateID: .hidSystemState),
      let down = CGEvent(
        mouseEventSource: source,
        mouseType: downType,
        mouseCursorPosition: point,
        mouseButton: button
      ),
      let up = CGEvent(
        mouseEventSource: source,
        mouseType: upType,
        mouseCursorPosition: point,
        mouseButton: button
      )
    else {
      XCTFail("Unable to create modifier-bearing mouse events")
      return
    }

    down.flags = modifiers
    up.flags = modifiers
    down.setIntegerValueField(.mouseEventClickState, value: 1)
    up.setIntegerValueField(.mouseEventClickState, value: 1)
    down.postToPid(appPID)
    pause(0.04)
    up.postToPid(appPID)
  }

  private func postKeyboardEvent(
    to appPID: pid_t,
    keyCode: CGKeyCode,
    keyDown: Bool,
    modifiers: CGEventFlags,
    isRepeat: Bool = false
  ) {
    guard
      let source = CGEventSource(stateID: .hidSystemState),
      let event = CGEvent(
        keyboardEventSource: source,
        virtualKey: keyCode,
        keyDown: keyDown
      )
    else {
      XCTFail("Unable to create keyboard event")
      return
    }
    event.flags = modifiers
    if isRepeat {
      event.setIntegerValueField(.keyboardEventAutorepeat, value: 1)
    }
    event.postToPid(appPID)
  }

  private func clickRailRow(
    _ index: Int,
    modifiers: CGEventFlags = [],
    button: CGMouseButton = .left
  ) {
    let rows = railRows()
    XCTAssertGreaterThan(rows.count, index, "Missing rail row \(index)")
    guard rows.indices.contains(index) else {
      return
    }
    postMouseClick(on: rows[index], button: button, modifiers: modifiers)
  }

  private func selectedRailRowIndices() -> Set<Int> {
    let rows = railRows()
    return Set(rows.indices.filter { rows[$0].isSelected })
  }

  private func assertSelectedRailRows(
    _ expected: Set<Int>,
    file: StaticString = #filePath,
    line: UInt = #line
  ) {
    XCTAssertTrue(
      waitUntil { self.selectedRailRowIndices() == expected },
      "Expected selected rail rows \(expected.sorted()); got "
        + "\(selectedRailRowIndices().sorted())",
      file: file,
      line: line
    )
  }

  // --- tests -----------------------------------------------------------------
  func testSidebarModifierClickSelection() throws {
    try activateAndVerifyApp()
    ensureRailRowCount(4)
    XCTAssertGreaterThanOrEqual(
      railRows().count,
      4,
      "Need four accessible CmuxRailRow tab elements (AXRadioButton on macOS)"
    )
    guard railRows().count >= 4 else {
      return
    }

    // Command-click toggles one row without clearing the existing selection.
    clickRailRow(0)
    assertSelectedRailRows([0])
    clickRailRow(2, modifiers: .maskCommand)
    assertSelectedRailRows([0, 2])
    clickRailRow(2, modifiers: .maskCommand)
    assertSelectedRailRows([0])

    // Shift-click replaces the selection with the contiguous anchor range.
    clickRailRow(2, modifiers: .maskShift)
    assertSelectedRailRows([0, 1, 2])

    // Command+Shift adds the anchor range to an existing discontiguous row.
    // Command-clicking row 3 makes it the anchor while preserving row 0;
    // Command+Shift-clicking row 1 then adds rows 1...3 without dropping row 0.
    clickRailRow(0)
    assertSelectedRailRows([0])
    clickRailRow(3, modifiers: .maskCommand)
    assertSelectedRailRows([0, 3])
    clickRailRow(1, modifiers: [.maskCommand, .maskShift])
    assertSelectedRailRows([0, 1, 2, 3])

    // Opening a context menu for a selected row must not collapse the set.
    clickRailRow(2, button: .right)
    XCTAssertTrue(
      app.menuItems["New Workspace Below"].firstMatch.waitForExistence(timeout: 3),
      "Right-click did not open the workspace context menu"
    )
    assertSelectedRailRows([0, 1, 2, 3])
    app.typeKey(XCUIKeyboardKey.escape.rawValue, modifierFlags: [])
  }

  func testKeyboardAndDevTools() throws {
    try activateAndVerifyApp()
    pause(1.0)
    XCTAssertTrue(cdpAlive(), "cmux CDP not reachable on :\(port)")

    // 1. Omnibox: Cmd-L focuses it, type a URL, Enter navigates. Clean
    //    end-to-end keyboard check that doesn't depend on web-content focus.
    app.typeKey("l", modifierFlags: .command); pause(0.5)
    app.typeText("example.com"); pause(0.3)
    app.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: []); pause(2.5)
    XCTAssertTrue(pageUrls().contains { $0.contains("example.com") },
                  "omnibox did not navigate; pages=\(pageUrls())")

    // 2. Terminal: focus terminal column, run a foreground job, Ctrl-C kills it.
    app.typeKey(XCUIKeyboardKey.rightArrow.rawValue, modifierFlags: [.command, .option]); pause(0.5)
    // typeText uses the committed-text injection path and can pass even when
    // printable keyDown events are dropped. Emit every character as a real
    // key so this covers the same path as hardware typing.
    typePhysicalKeys("sleep 41"); pause(0.2)
    app.typeKey(XCUIKeyboardKey.return.rawValue, modifierFlags: []); pause(0.8)
    let before = shell(#"pgrep -f "sleep 41" | head -1"#)
    app.typeKey("c", modifierFlags: .control); pause(0.9)
    let after = shell(#"pgrep -f "sleep 41" | head -1"#)
    XCTAssertTrue(!before.isEmpty && after.isEmpty,
                  "Ctrl-C did not interrupt terminal job (before=\(before) after=\(after))")
    if !after.isEmpty { shell("kill \(after)") }

    // 3. DevTools: open + close via Cmd-Opt-I; process must stay alive (no crash).
    let dt0 = devtoolsCount()
    app.typeKey("i", modifierFlags: [.command, .option]); pause(2.0)
    XCTAssertGreaterThan(devtoolsCount(), dt0, "DevTools did not open")
    app.typeKey("i", modifierFlags: [.command, .option]); pause(2.0)
    XCTAssertTrue(cdpAlive(), "cmux CRASHED after DevTools close")
  }

  func testHeldCommandWClosesRepeatedTabs() throws {
    try activateAndVerifyApp()
    XCTAssertTrue(cdpAlive(), "cmux CDP not reachable on :\(port)")

    let initialPageCount = pageUrls().count
    for _ in 0..<6 {
      app.typeKey("t", modifierFlags: .command)
      pause(0.15)
    }
    XCTAssertTrue(
      waitUntil(timeout: 8) { self.pageUrls().count >= initialPageCount + 6 },
      "Cmd-T did not create six tabs; pages=\(pageUrls())"
    )
    let expandedPageCount = pageUrls().count
    guard expandedPageCount >= initialPageCount + 6 else {
      return
    }
    guard let appPID = runnerAppPID() else {
      XCTFail("CMUX_XCUI_APP_PID must identify the app launched by the runner")
      return
    }

    // Match macOS hardware autorepeat: Command stays down, W sends one
    // ordinary keyDown followed by repeat keyDowns, and W receives only one
    // keyUp when the hold ends.
    let commandKey: CGKeyCode = 55
    let wKey: CGKeyCode = 13
    postKeyboardEvent(
      to: appPID, keyCode: commandKey, keyDown: true, modifiers: .maskCommand
    )
    postKeyboardEvent(
      to: appPID, keyCode: wKey, keyDown: true, modifiers: .maskCommand
    )
    pause(0.35)
    for _ in 1..<6 {
      postKeyboardEvent(
        to: appPID,
        keyCode: wKey,
        keyDown: true,
        modifiers: .maskCommand,
        isRepeat: true
      )
      pause(0.12)
    }
    postKeyboardEvent(
      to: appPID, keyCode: wKey, keyDown: false, modifiers: .maskCommand
    )
    postKeyboardEvent(
      to: appPID, keyCode: commandKey, keyDown: false, modifiers: []
    )

    XCTAssertTrue(
      waitUntil(timeout: 8) {
        self.pageUrls().count <= expandedPageCount - 6
      },
      "held Cmd-W did not close six tabs; before=\(expandedPageCount) "
        + "after=\(pageUrls().count)"
    )
    XCTAssertTrue(cdpAlive(), "cmux stopped responding after held Cmd-W")
  }
}
