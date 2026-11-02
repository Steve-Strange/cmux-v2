import CmuxCloud
import Foundation

struct SurfaceMachineID: Hashable, Sendable { let rawValue: String }
struct SurfaceResourceID: Sendable {
    let machine: SurfaceMachineID
    let key = "term_fixture"
    var rawValue: String { machine.rawValue + "/terminal/" + key }
}
struct SurfaceRemoteWorkspace: Sendable { let id = "ws_fixture"; let name = "Fixture" }
struct SurfaceRemoteView: Sendable { let workspace = SurfaceRemoteWorkspace() }
struct SurfaceResource: Sendable {
    let id: SurfaceResourceID
    let remoteWorkspace: SurfaceRemoteWorkspace? = nil
}
struct SurfaceProjection: Sendable { let workspaceID: UUID; let panelID = UUID() }
struct SurfaceResourcePlacement: Sendable {}
struct SurfaceResourceGroup: Sendable {}
struct SurfaceProjectionLayout: Sendable {}
enum SurfaceDestination: Sendable {
    enum Placement: Sendable { case tab }
    case workspace(id: UUID, placement: Placement)
    var workspaceID: UUID { switch self { case .workspace(let id, _): id } }
}
enum SurfaceCatalogError: Error { case noProvider(SurfaceMachineID) }
protocol CloudTerminalNavigationCatalog {}
struct CloudTreeNodeBuilder {
    static func localWorkspaceShowing(remoteWorkspaceID: String,
        placements: [SurfaceResourcePlacement], snapshot: Int) -> UUID? { nil }
}
struct CloudWorkspaceLayoutTranslator {
    static func fetch(machine: SurfaceMachineID, workspaceID: String,
        catalog: SurfaceCatalog) async -> SurfaceProjectionLayout? { nil }
}
struct CloudTerminalSourcePlacement {
    let machine: SurfaceMachineID
    let remoteWorkspaceID: String?
    func remoteView(of resource: SurfaceResource) throws -> SurfaceRemoteView? { nil }
}
struct FixtureNewWorkspaceHost { static let appOptimistic = FixtureNewWorkspaceHost() }
@MainActor final class FixtureProjectionCoordinator {
    func beginLocalMutation(on machine: SurfaceMachineID) -> UUID { UUID() }
    func endLocalMutation(_ token: UUID, on machine: SurfaceMachineID, catalog: SurfaceCatalog) {}
}
@MainActor final class SurfaceCatalog {
    static let shared = SurfaceCatalog()
    var fixtureProvider: CmuxTuiSurfaceProvider?
    let snapshot = 0
    let cloudWorkspaceProjectionCoordinator = FixtureProjectionCoordinator()
    func provider(for machine: SurfaceMachineID) -> CmuxTuiSurfaceProvider? { fixtureProvider }
    // The carrier is real. The catalog host supplies the pane and graph, so the
    // actual UI/socket adapters must convey deliberate intent before transport.
    func project(_ id: SurfaceResourceID, into destination: SurfaceDestination, focus: Bool,
        reuseExisting: Bool, reuseInWorkspace: UUID? = nil, remoteView: SurfaceRemoteView? = nil,
        explicitOpen: Bool = false) async throws -> (projection: SurfaceProjection, reused: Bool) {
        let provider = fixtureProvider!
        let use = explicitOpen ? provider.beginExplicitSSHTransportUse() : nil
        defer { if let use { provider.endExplicitSSHTransportUse(use) } }
        _ = try await (provider.links as! SSHTuiLinkManager).connected(machineID: id.machine.rawValue)
        provider.catalog.projections.append(FixtureProjection(workspaceID: destination.workspaceID))
        return (SurfaceProjection(workspaceID: destination.workspaceID), false)
    }
    func projectGroupAsNewLocalWorkspace(_ group: SurfaceResourceGroup, title: String,
        focus: Bool, host: FixtureNewWorkspaceHost, layout: SurfaceProjectionLayout?) async throws
        -> (workspaceID: UUID, projections: [SurfaceProjection]) { (UUID(), []) }
    func bindCloudWorkspace(localWorkspaceID: UUID, machine: SurfaceMachineID,
        remoteWorkspaceID: String, generatedTitle: String) {}
}
struct TerminalController {
    static func surfaceProvider(for machine: SurfaceMachineID, catalog: SurfaceCatalog) async throws
        -> CmuxTuiSurfaceProvider? { await catalog.provider(for: machine) }
}
extension CmuxTuiSurfaceProvider {
    func createTerminal(command: [String]?, cwd: String?, name: String?, remoteWorkspaceID: String?) async throws -> SurfaceResource {
        guard !isSSHTransportPaused else { throw CancellationError() }
        _ = try await (links as! SSHTuiLinkManager).connected(machineID: machineID)
        return SurfaceResource(id: SurfaceResourceID(machine: SurfaceMachineID(rawValue: machineID)))
    }
}
struct SSHExplicitSurfaceIntentFixture {
    @MainActor static func run() async throws {
        var failures: [String] = []
        for operation in ["UI open", "UI view open", "socket create", "socket create without projection"] {
            let probe = CloudCarrierProbe()
            let links = SSHTuiLinkManager(connection: SSHTuiConnection(), clientURL: URL(fileURLWithPath: "/unused"),
                paths: CloudTuiClientPaths(probe: probe), isEnabled: { true })
            let provider = CmuxTuiSurfaceProvider(links: links)
            let catalog = SurfaceCatalog.shared
            catalog.fixtureProvider = provider
            let local = Workspace()
            local.usesSSHTui = false
            local.remoteConnectionResumeEnabled = false
            AppDelegate.shared?.workspaces = [local]
            await links.disconnect()
            let machine = SurfaceMachineID(rawValue: "ssh:fixture")
            do {
                if operation.hasPrefix("UI") {
                    _ = try await catalog.projectTerminal(SurfaceResourceID(machine: machine), in: local.id,
                        view: operation == "UI view open" ? SurfaceRemoteView() : nil)
                } else {
                    _ = try await TerminalController.surfaceNewTerminal(machine: machine, command: nil, cwd: nil,
                        name: nil, remoteWorkspaceID: nil,
                        destination: operation == "socket create" ? .workspace(id: local.id, placement: .tab) : nil,
                        focus: false)
                }
            } catch { failures.append(operation + ": " + String(describing: error)) }
            let starts = await probe.starts
            if starts != 1 { failures.append(operation + ": carrier starts=" + String(starts)) }
            await links.disconnect()
            catalog.fixtureProvider = nil
        }
        AppDelegate.shared?.workspaces = []
        if !failures.isEmpty {
            print("FAIL explicit local SSH operations: " + failures.joined(separator: "; "))
            exit(1)
        }
        print("PASS deliberate UI terminal open and socket create after managed disconnect")
    }
}
