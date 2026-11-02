import CmuxCloud
import CmuxCore
import CmuxSurfaceCatalogModel
import Foundation
import Testing

#if canImport(cmux_DEV)
@testable import cmux_DEV
#elseif canImport(cmux)
@testable import cmux
#endif

@MainActor
@Suite("Cloud stale inventory admission", .serialized, .exclusiveAppContext)
struct CloudStaleInventoryMaterializationTests {
    @Test("a stale graph never materializes an interactive restored terminal")
    func staleGraphIsNotInteractive() {
        #expect(CmuxTuiSurfaceProvider.shouldShowRecoverableCloudFailure(observation: .stale(reason: "link timeout")))
        #expect(!CmuxTuiSurfaceProvider.shouldShowRecoverableCloudFailure(observation: .current))
    }

    @Test("missing and stale observations fail the actual attachment and fresh state retries it", arguments: [false, true])
    func restoredAttachmentRecovers(hasStaleObservation: Bool) throws {
        _ = try #require(AppDelegate.shared)
        let manager = TabManager(autoWelcomeIfNeeded: false, createInitialWorkspace: false)
        let workspace = Workspace()
        manager.tabs = [workspace]
        manager.selectedTabId = workspace.id
        defer { workspace.teardownAllPanels(); manager.tabs = [] }
        try LiveWorkspaceFixture.withAppRegistration(of: manager) {
            let panelID = try #require(workspace.focusedPanelId)
            let catalog = SurfaceCatalog()
            let summary = VMSummary(id: "stale-\(UUID())", provider: "freestyle", status: "running", image: "fixture", createdAt: 0, base: nil)
            let provider = CmuxTuiSurfaceProvider(summary: summary,
                links: CloudMachineLinkManager(clientURL: nil, hub: nil, hostThemeColors: { nil }), catalog: catalog)
            catalog.register(provider)
            defer { provider.suspendForFeatureFlag(); catalog.unregister(machine: provider.machine) }
            let resource = SurfaceResource(id: .init(machine: provider.machine, kind: .terminal, key: "term_fixture"), title: "Terminal", lifecycle: .running)
            let state = CloudVMState(machine: provider.machine, cursor: nil, rawSnapshot: Data("{}".utf8),
                workspaces: [], screens: [], panes: [], tabs: [], terminals: [], browsers: [], agents: [])
            if hasStaleObservation {
                catalog.replaceCloudState(state, resources: [resource], info: provider.info, observation: .stale(reason: "link timeout"))
            }
            let reservation = CloudTerminalPaneReservation(workspaceID: workspace.id, panelID: panelID, machine: provider.machine)
            workspace.cloudPendingCreations[panelID] = reservation
            defer { reservation.cancel?(); workspace.cloudPendingCreations[panelID] = nil }
            provider.attachReservedTerminalPane(reservation, resource: resource, remoteTabID: nil)
            #expect(workspace.cloudMaterializationFailures[panelID] != nil)
            #expect(!provider.materializedPanels.contains(panelID))
            #expect(provider.restoredAttachTasks[panelID] == nil)
            #expect(workspace.retryReservedCloudTerminalPane(surfaceId: panelID))
            #expect(!provider.materializedPanels.contains(panelID))

            catalog.record(SurfaceProjection(resource: resource.id, workspaceID: workspace.id, panelID: panelID))
            catalog.replaceCloudState(state, resources: [resource], info: provider.info, observation: .current)
            provider.projectionsRestored()
            #expect(provider.materializedPanels.contains(panelID))
            #expect(provider.restoredAttachTasks[panelID] != nil)
        }
    }

    @Test("a paused managed SSH reservation does not start an attachment")
    func pausedSSHReservationDoesNotStart() throws {
        let app = try VaultPaneAppFixture()
        defer { app.tearDown() }
        let workspace = app.workspace
        let configuration = try sshConfiguration()
        workspace.remoteConfiguration = configuration
        workspace.remoteConnectionResumeEnabled = false
        let panelID = try #require(workspace.focusedPanelId)
        let catalog = SurfaceCatalog()
        let provider = CmuxTuiSurfaceProvider(summary: .ssh(SSHTuiConnection(configuration: configuration)),
            links: SSHRestoredTerminalPauseLinks(), catalog: catalog)
        catalog.register(provider)
        defer { provider.suspendForFeatureFlag(); catalog.unregister(machine: provider.machine) }
        let resource = SurfaceResource(id: .init(machine: provider.machine, kind: .terminal, key: "term_paused"),
            title: "Terminal", lifecycle: .running)
        let state = CloudVMState(machine: provider.machine, cursor: nil, rawSnapshot: Data("{}".utf8),
            workspaces: [], screens: [], panes: [], tabs: [], terminals: [], browsers: [], agents: [])
        catalog.replaceCloudState(state, resources: [resource], info: provider.info, observation: .current)
        let reservation = CloudTerminalPaneReservation(workspaceID: workspace.id, panelID: panelID, machine: provider.machine)
        workspace.cloudPendingCreations[panelID] = reservation
        defer { reservation.cancel?(); workspace.cloudPendingCreations[panelID] = nil }

        provider.attachReservedTerminalPane(reservation, resource: resource, remoteTabID: nil)

        #expect(provider.restoredAttachTasks[panelID] == nil)
        #expect(!provider.materializedPanels.contains(panelID))
        #expect(workspace.remoteSessionSnapshot()?.autoReconnect == false)
    }

    @Test("a pause fences managed SSH without blocking projections in a local workspace", .timeLimit(.minutes(1)), arguments: [true, false])
    func pauseDuringSSHCarrierConnection(managedWorkspace: Bool) async throws {
        let app = try VaultPaneAppFixture()
        defer { app.tearDown() }
        let workspace = app.workspace
        let configuration = try sshConfiguration()
        workspace.remoteConfiguration = managedWorkspace ? configuration : nil
        let panelID = try #require(workspace.focusedPanelId)
        let pane = try #require(workspace.paneId(forPanelId: panelID))
        let links = SSHRestoredTerminalPauseLinks()
        let catalog = SurfaceCatalog()
        let provider = CmuxTuiSurfaceProvider(summary: .ssh(SSHTuiConnection(configuration: configuration)),
            links: links, catalog: catalog)
        defer { links.release.resolve(nil); provider.suspendForFeatureFlag() }
        let resource = SurfaceResource(id: .init(machine: provider.machine, kind: .terminal, key: "term_paused"),
            title: "Terminal", lifecycle: .running)
        let attempt = Task {
            try await provider.materializeManualMirrorTerminal(resource,
                at: .tab(workspaceID: workspace.id, paneID: pane.id.uuidString, index: nil), focus: false)
        }
        _ = await links.started.result
        workspace.disconnectRemoteConnection()
        // A workspace without SSH configuration has no pause intent. Even an
        // obsolete false value must not prevent its independent SSH projection.
        if !managedWorkspace { workspace.remoteConnectionResumeEnabled = false }
        links.release.resolve(CloudMachineLink.Connected(socketPath: "/unused-paused-ssh", session: "fixture"))

        do {
            _ = try await attempt.value
            Issue.record("The fixture has no link and must not materialize a terminal")
        } catch {
            #expect((error is CancellationError) == managedWorkspace)
        }
        #expect(await links.linkCount == (managedWorkspace ? 0 : 1))
        #expect(provider.manualMirrorSessions.isEmpty)
        if managedWorkspace { #expect(workspace.remoteSessionSnapshot()?.autoReconnect == false) }
    }

    @Test("explicit Retry on an SSH reservation resumes the saved workspace intent")
    func sshReservationRetryResumesIntent() throws {
        let app = try VaultPaneAppFixture()
        defer { app.tearDown() }
        let workspace = app.workspace
        let configuration = try sshConfiguration()
        workspace.remoteConfiguration = configuration
        workspace.remoteConnectionResumeEnabled = false
        let panelID = try #require(workspace.focusedPanelId)
        let catalog = SurfaceCatalog()
        let provider = CmuxTuiSurfaceProvider(summary: .ssh(SSHTuiConnection(configuration: configuration)),
            links: SSHRestoredTerminalPauseLinks(), catalog: catalog)
        catalog.register(provider)
        defer { workspace.disconnectRemoteConnection(); provider.suspendForFeatureFlag(); catalog.unregister(machine: provider.machine) }
        let resource = SurfaceResource(id: .init(machine: provider.machine, kind: .terminal, key: "term_paused"),
            title: "Terminal", lifecycle: .running)
        let reservation = CloudTerminalPaneReservation(workspaceID: workspace.id, panelID: panelID, machine: provider.machine)
        workspace.cloudPendingCreations[panelID] = reservation
        defer { reservation.cancel?(); workspace.cloudPendingCreations[panelID] = nil }
        provider.attachReservedTerminalPane(reservation, resource: resource, remoteTabID: nil)

        #expect(workspace.retryReservedCloudTerminalPane(surfaceId: panelID))

        #expect(workspace.remoteSessionSnapshot()?.autoReconnect != false)
        #expect(workspace.sshTuiConnectionAttemptID != nil)
    }

    private func sshConfiguration() throws -> WorkspaceRemoteConfiguration {
        var snapshot = SessionRemoteWorkspaceSnapshot(transport: .ssh, destination: "example.invalid",
            preserveAfterTerminalExit: true)
        snapshot.sshSessionOwner = "cmux-tui"
        return try #require(snapshot.workspaceConfiguration(environment: [:], isLiveAgent: { _ in false }))
    }

}
