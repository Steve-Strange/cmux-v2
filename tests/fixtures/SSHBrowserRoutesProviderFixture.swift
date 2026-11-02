import CmuxCloud
import CmuxCloudBannerCore
import Foundation

@MainActor final class CloudTunnelCoordinator {
    let backend = 0
    func stateUpdates() async -> AsyncStream<CloudTunnelState> { AsyncStream { $0.finish() } }
}
struct FixtureMachine: Equatable { let rawValue = "ssh:fixture" }
struct FixtureResource { let machine = FixtureMachine() }
struct FixtureProjection {
    let resource = FixtureResource()
    let workspaceID: UUID
    let panelID: UUID
}
struct FixtureEnvironment {
    @MainActor func workspace(_ id: UUID) -> Workspace? { Workspace.liveWorkspace(id: id) }
}
struct FixtureRenameService { let environment = FixtureEnvironment() }
@MainActor final class FixtureCatalog {
    var projections: [FixtureProjection] = []
    let cloudWorkspaceRenameService = FixtureRenameService()
}
@MainActor final class BrowserPanel {
    let id = UUID()
    let cloudAccess = FixtureBrowserAccess()
}
@MainActor final class FixtureBrowserAccess { var model: CloudPortAccessModel? }
@MainActor final class Workspace {
    let id = UUID()
    var usesSSHTui = true
    var remoteConnectionResumeEnabled = true
    let isRetiredFromOwningTabManager = false
    var remoteConfiguration: FixtureSSHConfiguration? = FixtureSSHConfiguration()
    var panels: [UUID: BrowserPanel] = [:]
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
protocol RemoteTuiLinkManaging {}
@MainActor final class SSHTuiLinkManager: RemoteTuiLinkManaging {
    let transportIntent = SSHTuiTransportIntent()
    func disconnect(ifPausedAt revision: UInt64) async {}
}
@MainActor final class CmuxTuiSurfaceProvider {
    let links: any RemoteTuiLinkManaging
    let machine = FixtureMachine()
    let machineID = "ssh:fixture"
    let catalog = FixtureCatalog()
    let portAccessStore = CloudPortAccessStore()
    var notificationSync: Bool? = true
    var manualMirrorSessions: [UUID: FixtureSession] = [:]
    var restoredAttachTasks: [UUID: Task<Void, Never>] = [:]
    var materializedPanels: Set<UUID> = []
    let attachmentRetry = FixtureRetry()
    init(links: SSHTuiLinkManager) { self.links = links }
    func installNotificationSync() { notificationSync = true }
    func stopTransportResources() { notificationSync = nil }
    func publishLinkStatus(_ state: FixtureLinkState, error: String?) {}
}
