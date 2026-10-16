import AppKit

private enum WorkspacePresentation: Int, CaseIterable {
    case occluded = 1
    case offscreen
    case transparent
    case orderedOut
    case minimized

    var slug: String {
        switch self {
        case .occluded: return "occluded"
        case .offscreen: return "offscreen"
        case .transparent: return "transparent"
        case .orderedOut: return "ordered-out"
        case .minimized: return "minimized"
        }
    }

    var explanation: String {
        switch self {
        case .occluded:
            return "Ordered behind the visible shell at exactly the same bounds"
        case .offscreen:
            return "Ordered, but positioned beyond the right edge of every screen"
        case .transparent:
            return "Ordered onscreen with window alphaValue = 0"
        case .orderedOut:
            return "Created but removed from the window server with orderOut"
        case .minimized:
            return "Ordered and then miniaturized"
        }
    }
}

private final class WorkspaceWindow {
    let presentation: WorkspacePresentation
    let window: NSWindow
    let input: NSTextField

    init(presentation: WorkspacePresentation,
         frame: NSRect,
         target: AnyObject,
         action: Selector) {
        self.presentation = presentation
        self.window = NSWindow(
            contentRect: frame,
            styleMask: [.titled, .closable, .resizable],
            backing: .buffered,
            defer: false)
        self.input = NSTextField(string: "initial-\(presentation.slug)")

        window.title = "AX Workspace — \(presentation.slug)"
        window.isReleasedWhenClosed = false
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        window.setAccessibilityIdentifier("workspace-window-\(presentation.slug)")

        let root = NSView(frame: NSRect(origin: .zero, size: frame.size))
        root.autoresizingMask = [.width, .height]

        let heading = NSTextField(labelWithString: "Workspace \(presentation.slug)")
        heading.font = .boldSystemFont(ofSize: 18)
        heading.frame = NSRect(x: 24, y: 160, width: 410, height: 26)
        heading.setAccessibilityIdentifier("workspace-heading-\(presentation.slug)")
        root.addSubview(heading)

        let detail = NSTextField(wrappingLabelWithString: presentation.explanation)
        detail.frame = NSRect(x: 24, y: 112, width: 410, height: 42)
        detail.setAccessibilityIdentifier("workspace-detail-\(presentation.slug)")
        root.addSubview(detail)

        input.frame = NSRect(x: 24, y: 68, width: 250, height: 28)
        input.setAccessibilityIdentifier("workspace-input-\(presentation.slug)")
        input.setAccessibilityLabel("\(presentation.slug) workspace input")
        root.addSubview(input)

        let button = NSButton(title: "Operate \(presentation.slug) workspace",
                              target: target,
                              action: action)
        button.bezelStyle = .rounded
        button.tag = presentation.rawValue
        button.frame = NSRect(x: 24, y: 22, width: 250, height: 32)
        button.setAccessibilityIdentifier("workspace-operate-\(presentation.slug)")
        button.setAccessibilityLabel("Operate \(presentation.slug) workspace")
        root.addSubview(button)

        window.contentView = root
    }
}

private final class AppDelegate: NSObject, NSApplicationDelegate {
    private var shellWindow: NSWindow!
    private var statusLabel: NSTextField!
    private var workspaces: [WorkspacePresentation: WorkspaceWindow] = [:]

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)

        let visibleFrame = NSScreen.main?.visibleFrame ??
            NSRect(x: 0, y: 0, width: 1440, height: 900)
        let shellSize = NSSize(width: 640, height: 460)
        let shellFrame = NSRect(
            x: visibleFrame.midX - shellSize.width / 2,
            y: visibleFrame.midY - shellSize.height / 2,
            width: shellSize.width,
            height: shellSize.height)

        shellWindow = NSWindow(
            contentRect: shellFrame,
            styleMask: [.titled, .closable, .resizable, .miniaturizable],
            backing: .buffered,
            defer: false)
        shellWindow.title = "cmux AX Window Lab — visible shell"
        shellWindow.isReleasedWhenClosed = false
        shellWindow.setAccessibilityIdentifier("visible-shell-window")

        let root = NSView(frame: NSRect(origin: .zero, size: shellSize))
        root.autoresizingMask = [.width, .height]

        let title = NSTextField(labelWithString: "One visible shell, five native workspace windows")
        title.font = .boldSystemFont(ofSize: 20)
        title.frame = NSRect(x: 28, y: 386, width: 580, height: 30)
        root.addSubview(title)

        let detail = NSTextField(wrappingLabelWithString:
            "Use Sky to inspect and operate controls in the workspace windows. " +
            "Any successful background action is reported below.")
        detail.frame = NSRect(x: 28, y: 330, width: 580, height: 48)
        root.addSubview(detail)

        statusLabel = NSTextField(wrappingLabelWithString: "No workspace action received yet")
        statusLabel.font = .monospacedSystemFont(ofSize: 15, weight: .medium)
        statusLabel.frame = NSRect(x: 28, y: 248, width: 580, height: 62)
        statusLabel.setAccessibilityIdentifier("experiment-status")
        statusLabel.setAccessibilityLabel("Experiment status")
        root.addSubview(statusLabel)

        let matrix = WorkspacePresentation.allCases.map {
            "\($0.slug): \($0.explanation)"
        }.joined(separator: "\n")
        let modes = NSTextField(wrappingLabelWithString: matrix)
        modes.frame = NSRect(x: 28, y: 28, width: 580, height: 200)
        modes.font = .systemFont(ofSize: 13)
        root.addSubview(modes)

        shellWindow.contentView = root

        for presentation in WorkspacePresentation.allCases {
            let workspace = WorkspaceWindow(
                presentation: presentation,
                frame: NSRect(origin: shellFrame.origin,
                              size: NSSize(width: 460, height: 230)),
                target: self,
                action: #selector(operateWorkspace(_:)))
            workspaces[presentation] = workspace
        }

        // Establish each hiding strategy before the visible shell is ordered.
        workspaces[.offscreen]?.window.setFrameOrigin(NSPoint(
            x: NSScreen.screens.map(\.frame.maxX).max()! + 500,
            y: visibleFrame.midY))
        workspaces[.offscreen]?.window.orderFront(nil)

        workspaces[.transparent]?.window.alphaValue = 0
        workspaces[.transparent]?.window.orderFront(nil)

        workspaces[.orderedOut]?.window.orderOut(nil)

        workspaces[.minimized]?.window.orderFront(nil)
        workspaces[.minimized]?.window.miniaturize(nil)

        workspaces[.occluded]?.window.setFrame(shellFrame, display: false)
        workspaces[.occluded]?.window.orderFront(nil)

        shellWindow.makeKeyAndOrderFront(nil)
        if let occluded = workspaces[.occluded]?.window {
            occluded.order(.below, relativeTo: shellWindow.windowNumber)
        }
        NSApp.activate(ignoringOtherApps: true)
        writeLog("READY")
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    @objc private func operateWorkspace(_ sender: NSButton) {
        guard let presentation = WorkspacePresentation(rawValue: sender.tag),
              let workspace = workspaces[presentation] else {
            return
        }
        let result = "OPERATED \(presentation.slug) input=\(workspace.input.stringValue)"
        statusLabel.stringValue = result
        shellWindow.makeKeyAndOrderFront(nil)
        writeLog(result)
    }

    private func writeLog(_ message: String) {
        let line = "\(ISO8601DateFormatter().string(from: Date())) \(message)\n"
        let url = URL(fileURLWithPath: "/tmp/cmux-ax-window-lab.log")
        if let handle = try? FileHandle(forWritingTo: url) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: Data(line.utf8))
        } else {
            try? Data(line.utf8).write(to: url)
        }
    }
}

@main
private struct AXWindowLab {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }
}
