import CmuxCore
import Foundation
import Testing

#if canImport(cmux_DEV)
@testable import cmux_DEV
#elseif canImport(cmux)
@testable import cmux
#endif

@MainActor
@Suite(.serialized) struct RemoteWorkspaceReconnectIntentTests {
    private func configuration() throws -> WorkspaceRemoteConfiguration {
        var snapshot = SessionRemoteWorkspaceSnapshot(
            transport: .ssh,
            destination: "example.invalid",
            preserveAfterTerminalExit: true
        )
        snapshot.sshSessionOwner = "cmux-tui"
        return try #require(snapshot.workspaceConfiguration(environment: [:], isLiveAgent: { _ in false }))
    }

    @Test func deliberateDisconnectSurvivesRestoreWithoutStartingAnAttempt() throws {
        let manager = TabManager(autoWelcomeIfNeeded: false)
        let workspace = try #require(manager.selectedWorkspace)
        #expect(workspace.configureRemoteConnection(try configuration(), autoConnect: false))
        workspace.disconnectRemoteConnection()
        var snapshot = manager.sessionSnapshot(includeScrollback: false)
        #expect(snapshot.workspaces[0].remote?.autoReconnect == false)
        for index in snapshot.workspaces[0].panels.indices {
            snapshot.workspaces[0].panels[index].terminal?.isRemoteTerminal = true
        }

        let restoredManager = TabManager(autoWelcomeIfNeeded: false)
        restoredManager.restoreSessionSnapshot(snapshot)
        let restored = try #require(restoredManager.selectedWorkspace)
        #expect(!restored.remoteConnectionResumeEnabled)
        #expect(restored.remoteConnectionState == .disconnected)
        #expect(restored.sshTuiConnectionAttemptID == nil)
        #expect(restored.remoteConfiguration?.destination == "example.invalid")
        #expect(restored.panels.count == workspace.panels.count)
        #expect(restored.sessionSnapshot(includeScrollback: false).remote?.autoReconnect == false)
    }

    @Test func disconnectIntentChangesAutosaveEvenWhenTransportWasAlreadyDisconnected() throws {
        let manager = TabManager(autoWelcomeIfNeeded: false)
        let workspace = try #require(manager.selectedWorkspace)
        #expect(workspace.configureRemoteConnection(try configuration(), autoConnect: false))
        let fingerprint = manager.sessionAutosaveFingerprint()
        workspace.disconnectRemoteConnection()
        #expect(manager.sessionAutosaveFingerprint() != fingerprint)
    }

    @Test func explicitConfigurationClearsSavedDisconnectIntent() throws {
        let manager = TabManager(autoWelcomeIfNeeded: false)
        let workspace = try #require(manager.selectedWorkspace)
        var saved = try #require(try configuration().sessionSnapshot())
        saved.autoReconnect = false
        let restoredConfiguration = try #require(saved.workspaceConfiguration(environment: [:], isLiveAgent: { _ in false }))
        #expect(workspace.configureRemoteConnection(restoredConfiguration, autoConnect: false))
        #expect(workspace.remoteSessionSnapshot()?.autoReconnect != false)
    }

    @Test func localWorkspaceRestoreDoesNotPauseSSHProjections() throws {
        let manager = TabManager(autoWelcomeIfNeeded: false)
        let snapshot = manager.sessionSnapshot(includeScrollback: false)
        #expect(snapshot.workspaces.first?.remote == nil)
        let restoredManager = TabManager(autoWelcomeIfNeeded: false)
        restoredManager.restoreSessionSnapshot(snapshot)
        let restored = try #require(restoredManager.selectedWorkspace)
        #expect(restored.remoteConfiguration == nil)
        #expect(restored.remoteConnectionResumeEnabled)
    }

    @Test func rejectedTargetedReconnectKeepsDisconnectIntent() throws {
        let manager = TabManager(autoWelcomeIfNeeded: false)
        let workspace = try #require(manager.selectedWorkspace)
        #expect(workspace.configureRemoteConnection(try configuration(), autoConnect: false))
        workspace.disconnectRemoteConnection()
        #expect(!workspace.reconnectRemoteConnection(surfaceId: UUID()))
        #expect(workspace.remoteSessionSnapshot()?.autoReconnect == false)
        #expect(workspace.sshTuiConnectionAttemptID == nil)
    }
}
