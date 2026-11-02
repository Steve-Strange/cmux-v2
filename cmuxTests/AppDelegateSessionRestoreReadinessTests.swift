import AppKit
import Testing

#if canImport(cmux_DEV)
@testable import cmux_DEV
#elseif canImport(cmux)
@testable import cmux
#endif

// Regression guard for https://github.com/manaflow-ai/cmux/issues/2751.
//
// `AppDelegate.didCompleteInitialSessionRestore` is the readiness signal that
// gates the v2 control pre-mint pass (`TerminalController.v2RefreshKnownRefs()`
// and its `drag_surface_to_split` twin `controlSidebarRefreshKnownRefs()`):
// those skip the pre-mint while a session restore is pending or in flight so
// they never enumerate a half-built window/tab tree (the EXC_BAD_ACCESS the
// issue reported). The signal is the conjunction of two lifecycle flags:
//
//     didAttemptStartupSessionRestore && !isApplyingSessionRestore
//
// Draft 1 of the fix gated on a flag that could be TRUE before the primary
// restore actually ran on the deferred-signing-secret path, reopening the crash
// window. This test locks the four flag combinations so the signal can only be
// TRUE when restore has truly settled. It reads two Bools, so no real window or
// tab tree is needed and the crash itself (which requires a half-built tree) is
// intentionally not reproduced here.
@Suite(.serialized)
@MainActor
struct AppDelegateSessionRestoreReadinessTests {
    @Test
    func signalIsFalseBeforeRestoreIsAttempted() {
        let previousApp = AppDelegate.shared
        let appDelegate = AppDelegate()
        defer { AppDelegate.shared = previousApp }
        appDelegate.didAttemptStartupSessionRestore = false
        appDelegate.isApplyingSessionRestore = false
        // Pre-attempt / deferred-signing-secret window: nothing has decided to
        // restore yet, so enumerating the tree must be skipped.
        #expect(appDelegate.didCompleteInitialSessionRestore == false)
    }

    @Test
    func signalIsFalseWhileRestoreIsInFlight() {
        let previousApp = AppDelegate.shared
        let appDelegate = AppDelegate()
        defer { AppDelegate.shared = previousApp }
        appDelegate.didAttemptStartupSessionRestore = true
        appDelegate.isApplyingSessionRestore = true
        // restoreSessionSnapshot is mutating a tab manager in place; the tree is
        // half-built, so the pre-mint pass must be skipped.
        #expect(appDelegate.didCompleteInitialSessionRestore == false)
    }

    @Test
    func signalIsTrueOnceRestoreHasSettled() {
        let previousApp = AppDelegate.shared
        let appDelegate = AppDelegate()
        defer { AppDelegate.shared = previousApp }
        appDelegate.didAttemptStartupSessionRestore = true
        appDelegate.isApplyingSessionRestore = false
        // completeSessionRestoreOperation has cleared the in-flight flag: the
        // tree is stable and safe to enumerate.
        #expect(appDelegate.didCompleteInitialSessionRestore == true)
    }

    @Test
    func signalIsFalseWhenApplyingWithoutAttemptFlag() {
        let previousApp = AppDelegate.shared
        let appDelegate = AppDelegate()
        defer { AppDelegate.shared = previousApp }
        appDelegate.didAttemptStartupSessionRestore = false
        appDelegate.isApplyingSessionRestore = true
        // Defensive: any restore-in-flight state must read FALSE regardless of
        // the attempt flag, so the guard can never enumerate a mutating tree.
        #expect(appDelegate.didCompleteInitialSessionRestore == false)
    }
}

@Suite(.serialized, .exclusiveAppContext)
@MainActor
struct AppDelegateOrdinaryOpenTests {
    private func withRegisteredWindow(
        _ body: (AppDelegate, NSWindow, TabManager) throws -> Void
    ) throws {
        _ = NSApplication.shared
        let previousApp = AppDelegate.shared
        let previousManager = TerminalController.shared.activeTabManagerForCallerNotification()
        let app = AppDelegate()
        app.didAttemptStartupSessionRestore = true
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 500, height: 320),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        let windowId = UUID()
        window.identifier = NSUserInterfaceItemIdentifier("cmux.main.\(windowId.uuidString)")
        let manager = TabManager(autoWelcomeIfNeeded: false)
        _ = try #require(manager.addTab())
        app.registerMainWindow(
            window,
            windowId: windowId,
            tabManager: manager,
            sidebarState: SidebarState(),
            sidebarSelectionState: SidebarSelectionState()
        )
        defer {
            for context in Array(app.mainWindowContexts.values) {
                let ownedWindow = context.window
                app.unregisterMainWindowContextForTesting(windowId: context.windowId)
                ownedWindow?.orderOut(nil)
            }
            window.orderOut(nil)
            TerminalController.shared.setActiveTabManager(previousManager)
            AppDelegate.shared = previousApp
        }
        try body(app, window, manager)
    }

    private func awaitSigningSecretReadiness() async {
        await withCheckedContinuation { continuation in
            SurfaceResumeApprovalStore.whenSigningSecretReady {
                continuation.resume()
            }
        }
    }

    @Test(arguments: [false, true])
    func repeatedReopenSelectsFirstWorkspaceWithoutAddingWindowsOrWorkspaces(hidden: Bool) throws {
        try withRegisteredWindow { app, window, manager in
            let workspaceIds = manager.tabs.map(\.id)
            let windowIds = Set(app.mainWindowContexts.values.map(\.windowId))
            window.orderFront(nil)
            if hidden {
                app.mainWindowVisibilityController.dismissWindows(windows: [window], reason: .titlebarDismiss)
            }
            for _ in 0..<2 {
                manager.selectTab(try #require(manager.tabs.last))
                #expect(app.applicationShouldHandleReopen(NSApp, hasVisibleWindows: !hidden))
                #expect(manager.selectedTabId == workspaceIds.first)
                #expect(manager.tabs.map(\.id) == workspaceIds)
                #expect(Set(app.mainWindowContexts.values.map(\.windowId)) == windowIds)
            }
        }
    }

    @Test
    func ordinaryStartupSelectsFirstWorkspaceAfterRestoringSavedSelection() async throws {
        await awaitSigningSecretReadiness()
        try withRegisteredWindow { app, window, manager in
            let snapshot = manager.sessionSnapshot(includeScrollback: false)
            manager.restoreSessionSnapshot(snapshot)
            let workspaceIds = manager.tabs.map(\.id)
            #expect(manager.selectedTabId == workspaceIds.last)
            app.didAttemptStartupSessionRestore = false
            app.registerMainWindow(
                window,
                windowId: try #require(manager.windowId),
                tabManager: manager,
                sidebarState: SidebarState(),
                sidebarSelectionState: SidebarSelectionState()
            )
            #expect(app.didCompleteInitialSessionRestore)
            #expect(manager.selectedTabId == workspaceIds.first)
            #expect(manager.tabs.map(\.id) == workspaceIds)
        }
    }

    @Test
    func explicitStartupOpenKeepsItsSelectedWorkspace() async throws {
        await awaitSigningSecretReadiness()
        try withRegisteredWindow { app, window, manager in
            let selectedId = manager.selectedTabId
            app.didAttemptStartupSessionRestore = false
            app.prepareForExplicitOpenIntentAtStartup()
            app.registerMainWindow(
                window,
                windowId: try #require(manager.windowId),
                tabManager: manager,
                sidebarState: SidebarState(),
                sidebarSelectionState: SidebarSelectionState()
            )
            #expect(manager.selectedTabId == selectedId)
        }
    }

    @Test
    func deferredStartupNavigationKeepsItsRequestedWorkspace() async throws {
        await awaitSigningSecretReadiness()
        try withRegisteredWindow { app, window, manager in
            let target = try #require(manager.tabs.last)
            app.pendingStartupNavigationURLRequests = [CmuxNavigationURLRequest(
                originalURL: try #require(URL(string: "cmux://workspace/\(target.id.uuidString)")),
                target: .workspace(target.id),
                stableFallbackWorkspaceId: nil,
                stableFallbackSurfaceId: nil
            )]
            app.didAttemptStartupSessionRestore = false
            app.registerMainWindow(
                window,
                windowId: try #require(manager.windowId),
                tabManager: manager,
                sidebarState: SidebarState(),
                sidebarSelectionState: SidebarSelectionState()
            )
            #expect(app.pendingStartupNavigationURLRequests.isEmpty)
            #expect(manager.selectedTabId == target.id)
        }
    }

    @Test
    func manualSessionRestoreKeepsSavedSelection() throws {
        try withRegisteredWindow { app, _, manager in
            let savedWindowId = UUID()
            let snapshot = AppSessionSnapshot(
                version: SessionSnapshotSchema.currentVersion,
                createdAt: 1_700_000_000,
                windows: [SessionWindowSnapshot(
                    windowId: savedWindowId,
                    frame: nil,
                    display: nil,
                    tabManager: manager.sessionSnapshot(includeScrollback: false),
                    sidebar: SessionSidebarSnapshot(isVisible: true, selection: .tabs, width: nil)
                )]
            )
            #expect(app.restorePreviousSessionSnapshot(snapshot, shouldActivate: false))
            let restored = try #require(app.tabManagerFor(windowId: savedWindowId))
            #expect(restored.tabs.count == 2)
            #expect(restored.selectedTabId == restored.tabs.last?.id)
        }
    }
}

// Regression guard for https://github.com/manaflow-ai/cmux/issues/5757.
//
// `controlResolveOnMain` gates the full-tree `v2RefreshKnownRefs()` pass on
// `needsHandleTopologyRefresh`. Repeated calls when the window/tab topology
// hasn't changed must skip rescanning the tree to prevent freezing the MainActor
// during heavy agent load.
@Suite(.serialized)
@MainActor
struct TerminalControllerControlResolveTests {
    private func makeMainWindow(id: UUID) -> NSWindow {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 500, height: 320),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.identifier = NSUserInterfaceItemIdentifier("cmux.main.\(id.uuidString)")
        return window
    }

    @Test
    func repeatedControlResolveOnMainSkipsRescanWhenTopologyUnchanged() throws {
        _ = NSApplication.shared
        let previousApp = AppDelegate.shared
        let app = AppDelegate()
        AppDelegate.shared = app
        defer { AppDelegate.shared = previousApp }

        // Session restore settled: readiness allows v2RefreshKnownRefs to proceed
        app.didAttemptStartupSessionRestore = true
        app.isApplyingSessionRestore = false

        let windowId = UUID()
        let window = makeMainWindow(id: windowId)
        let manager = TabManager()
        let previousManager = TerminalController.shared.activeTabManagerForCallerNotification()
        defer {
            TerminalController.shared.setActiveTabManager(previousManager)
            app.unregisterMainWindowContextForTesting(windowId: windowId)
            window.orderOut(nil)
        }

        app.registerMainWindow(
            window,
            windowId: windowId,
            tabManager: manager,
            sidebarState: SidebarState(),
            sidebarSelectionState: SidebarSelectionState(),
            fileExplorerState: FileExplorerState()
        )
        window.makeKeyAndOrderFront(nil)
        TerminalController.shared.setActiveTabManager(manager)

        let initialWorkspace = Workspace()
        manager.tabs = [initialWorkspace]

        // Reset topology refresh state so initial scan can claim it
        TerminalController.shared.invalidateSocketHandleTopologyRefresh()
        #expect(TerminalController.shared.controlCommandCoordinator.needsHandleTopologyRefresh)

        // First call: scans topology, mints initial workspace ref, marks refresh completed
        TerminalController.shared.controlResolveOnMain { _ in }
        #expect(!TerminalController.shared.controlCommandCoordinator.needsHandleTopologyRefresh)
        #expect(TerminalController.shared.v2ExistingHandleRef(kind: .workspace, uuid: initialWorkspace.id) != nil)

        // Add a second workspace directly to manager without posting a topology invalidation
        let secondWorkspace = Workspace()
        manager.tabs.append(secondWorkspace)

        // Second call: topology has not changed (gate closed).
        // Must skip rescan, so secondWorkspace is NOT minted.
        TerminalController.shared.controlResolveOnMain { _ in }
        #expect(!TerminalController.shared.controlCommandCoordinator.needsHandleTopologyRefresh)
        #expect(TerminalController.shared.v2ExistingHandleRef(kind: .workspace, uuid: secondWorkspace.id) == nil)

        // Invalidate topology (mirroring notification when windows/tabs change)
        NotificationCenter.default.post(name: .mainWindowContextsDidChange, object: app)
        #expect(TerminalController.shared.controlCommandCoordinator.needsHandleTopologyRefresh)

        // Third call: topology was invalidated, so it rescans and mints the second workspace
        TerminalController.shared.controlResolveOnMain { _ in }
        #expect(!TerminalController.shared.controlCommandCoordinator.needsHandleTopologyRefresh)
        #expect(TerminalController.shared.v2ExistingHandleRef(kind: .workspace, uuid: secondWorkspace.id) != nil)
    }
}

