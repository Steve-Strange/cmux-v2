import Foundation

typealias BrowserMediaActivity = String
typealias WorkspaceTaskStatus = String
typealias SidebarWorkspaceCompactStatusMenuModel = String
typealias WorkspaceChecklistItem = String
typealias SidebarCompactStatusGlyph = String
typealias SidebarWorkspaceTaskStatusSnapshot = String

// Value-only host for compiling the actual refresh policy without SwiftUI or
// the app. Opaque row details remain strings: this probe exercises which
// snapshot is presented, not how those unrelated details are formatted.
struct SidebarWorkspaceSnapshotBuilder {
    struct Snapshot: Equatable {
        var presentationKey = "settings"
        var title = "gpusp"
        var customDescription: String?
        var isPinned = false
        var isMuted = false
        var customColorHex: String?
        var hasManualCustomColor = false
        var cloudWorkspaceLabel: String?
        var remoteWorkspaceSidebarText: String? = "gpusp · tmux: de"
        var remoteConnectionStatusText = "Connected"
        var remoteStateHelpText = "Remote connected to gpusp"
        var showsRemoteReconnectAffordance = false
        var copyableSidebarSSHError: String?
        var latestConversationMessage: String?
        var metadataEntries = [String]()
        var metadataBlocks = [String]()
        var latestLog: String?
        var progress: String?
        var activeCodingAgentCount = 0
        var compactGitBranchSummaryText: String?
        var compactDirectoryCandidates = [String]()
        var compactBranchDirectoryCandidates = [String]()
        var branchDirectoryLines = [String]()
        var branchLinesContainBranch = false
        var pullRequestRows = [String]()
        var listeningPorts = [Int]()
        var finderDirectoryPath: String?
        var mediaActivity = "idle"
        var taskStatus: String?
        var todoStatusMenuModel: String?
        var hasManualTaskStatus = false
        var checklistItems = [String]()
        var checklistCompletedCount = 0
        var checklistTotalCount = 0
        var checklistFirstUncheckedText: String?
        var taskStatusInput = "idle"
        var deviceWorkspaceLabel: String?
        var compactStatusGlyph: String?
        var hostTitleSuffix: String?
    }
}

@main
struct SidebarRemoteStatusRefreshFixture {
    static func main() throws {
        let current = SidebarWorkspaceSnapshotBuilder.Snapshot(
            latestConversationMessage: "old", listeningPorts: [3000]
        )
        var next = current
        next.remoteWorkspaceSidebarText = "gpusp · tmux: de2"
        next.remoteConnectionStatusText = "Disconnected"
        next.remoteStateHelpText = "Remote disconnected from gpusp: Broken pipe"
        next.showsRemoteReconnectAffordance = true
        next.copyableSidebarSSHError = "gpusp: Broken pipe"
        next.latestConversationMessage = "new"
        next.listeningPorts = [3000, 4000]

        let policy = SidebarWorkspaceSnapshotRefreshPolicy()
        let decision = policy.decision(current: current, next: next, force: false, contextMenuVisible: true)
        let shown = decision.workspaceSnapshotStorage
        var failures: [String] = []
        if shown?.remoteConnectionStatusText != "Disconnected" { failures.append("Disconnected SSH still appears Connected while its menu is open") }
        if shown?.remoteWorkspaceSidebarText != next.remoteWorkspaceSidebarText { failures.append("SSH identity is stale") }
        if shown?.remoteStateHelpText != next.remoteStateHelpText { failures.append("SSH diagnostic help is stale") }
        if shown?.showsRemoteReconnectAffordance != true { failures.append("Reconnect action remains hidden") }
        if shown?.copyableSidebarSSHError != next.copyableSidebarSSHError { failures.append("Copy SSH error is stale") }
        if shown?.latestConversationMessage != "old" || shown?.listeningPorts != [3000] { failures.append("Noisy telemetry must remain deferred") }
        if decision.pendingWorkspaceSnapshot != next { failures.append("Deferred details were lost") }

        let closed = policy.decision(current: shown, next: next, force: false, contextMenuVisible: false)
        if closed.workspaceSnapshotStorage != next || closed.pendingWorkspaceSnapshot != nil { failures.append("Closing the menu must flush the full snapshot") }
        if !failures.isEmpty {
            FileHandle.standardError.write(Data((failures.joined(separator: "\n") + "\n").utf8))
            exit(1)
        }
        print("PASS: SSH status, identity, diagnostic and reconnect update during menu tracking; telemetry stays deferred")
    }
}
