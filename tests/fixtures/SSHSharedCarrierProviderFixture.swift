import CmuxCloud
import Foundation

struct FixtureMachine: Equatable { let rawValue = "ssh:fixture" }
struct FixtureResource { let machine = FixtureMachine() }
struct FixtureProjection { let resource = FixtureResource(); let workspaceID: UUID }
struct FixtureEnvironment {
    @MainActor func workspace(_ id: UUID) -> Workspace? { Workspace.liveWorkspace(id: id) }
}
struct FixtureRenameService { let environment = FixtureEnvironment() }
@MainActor final class FixtureCatalog {
    var projections: [FixtureProjection] = []
    let cloudWorkspaceRenameService = FixtureRenameService()
}
@MainActor final class Workspace {
    let id = UUID()
    var usesSSHTui = true
    var remoteConnectionResumeEnabled = true
    let isRetiredFromOwningTabManager = false
    var remoteConfiguration: FixtureSSHConfiguration? = FixtureSSHConfiguration()
    static func liveWorkspace(id: UUID) -> Workspace? {
        AppDelegate.shared?.workspaces.first { $0.id == id }
    }
}
@MainActor final class AppDelegate {
    static let shared: AppDelegate? = AppDelegate()
    var workspaces: [Workspace] = []
    func surfaceCatalogWorkspaces() -> [Workspace] { workspaces }
}
struct ManagedRemoteConnectionsPolicy { static let isEnabled = true }
@MainActor final class FixtureSession { var allowsAutomaticReconnect = true }
struct FixtureRetry { func cancel() {} }
enum FixtureLinkState { case unavailable }
@MainActor final class CmuxTuiSurfaceProvider {
    let links: any RemoteTuiLinkManaging
    let machine = FixtureMachine()
    let machineID = "ssh:fixture"
    let catalog = FixtureCatalog()
    var notificationSync: Bool? = true
    var manualMirrorSessions: [UUID: FixtureSession] = [:]
    var restoredAttachTasks: [UUID: Task<Void, Never>] = [:]
    var materializedPanels: Set<UUID> = []
    let attachmentRetry = FixtureRetry()
    private(set) var stopCount = 0
    init(links: SSHTuiLinkManager) { self.links = links }
    func installNotificationSync() { notificationSync = true }
    func stopTransportResources() { stopCount += 1; notificationSync = nil }
    func publishLinkStatus(_ state: FixtureLinkState, error: String?) {}
}
