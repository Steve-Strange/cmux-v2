import CmuxCloud
import Foundation

@MainActor final class CloudTerminalPaneReservation {
    let workspaceID: UUID
    let panelID = UUID()
    let machine = SurfaceMachineID(rawValue: "ssh:fixture")
    let inputRelay = true
    let sourcePlacement = FixtureSourcePlacement()
    let creationReceipt = FixtureCreationReceipt()
    var retry: (() -> Void)?
    var cancel: (() -> Void)?
    init(workspaceID: UUID) { self.workspaceID = workspaceID }
}
struct FixtureSourcePlacement {
    func remoteView(of resource: SurfaceResource) throws -> SurfaceRemoteView? { nil }
    func validate(created: SurfaceResource) throws {}
}
struct FixtureCreationReceipt {
    func beginAttempt() {}
    func finish(_ result: Result<SurfaceResource, Error>) {}
}
struct CloudTerminalCreationCoordinator {
    typealias Create = @MainActor () async throws -> SurfaceResource
    typealias Project = @MainActor (SurfaceResource) async throws -> (projection: SurfaceProjection, reused: Bool)
}
enum CloudDiagnosticFailure: Error {
    case cancelled, placement
    static func classify(_ error: Error) -> Self { error is CancellationError ? .cancelled : .placement }
}
struct CloudOperationContext {
    enum Phase { case materialize }
    @MainActor static func phase<Value>(_ phase: Phase, _ body: @MainActor () async throws -> Value) async rethrows -> Value {
        try await body()
    }
}
@MainActor final class FixtureCreationStore {
    var operation: Task<Bool, Never>?
    func retry(requestID: UUID) {}
    func cancel(requestID: UUID) { operation?.cancel() }
    func run(machine: SurfaceMachineID, requestID: UUID,
        create: @escaping CloudTerminalCreationCoordinator.Create,
        project: @escaping CloudTerminalCreationCoordinator.Project,
        onStart: @escaping @MainActor () -> Void, onFinish: @escaping @MainActor () -> Void,
        inlineFailure: @escaping @MainActor (Error) -> Void,
        discardProjection: @escaping @MainActor (SurfaceProjection) -> Void, operations: Bool?) {
        onStart()
        operation = Task {
            defer { onFinish() }
            do { _ = try await project(create()); return true }
            catch { inlineFailure(error); return false }
        }
    }
}
actor FixtureCreationGate {
    private var held: CheckedContinuation<Void, Never>?
    private var observer: CheckedContinuation<Void, Never>?
    func wait() async {
        await withCheckedContinuation { held = $0; observer?.resume(); observer = nil }
    }
    func waitUntilHeld() async {
        guard held == nil else { return }
        await withCheckedContinuation { observer = $0 }
    }
    func release() { held?.resume(); held = nil }
}
struct SSHOptimisticCreationIntentFixture {
    @MainActor static func run() async throws {
        let probe = CloudCarrierProbe()
        let links = SSHTuiLinkManager(connection: SSHTuiConnection(), clientURL: URL(fileURLWithPath: "/unused"),
            paths: CloudTuiClientPaths(probe: probe), isEnabled: { true })
        let provider = CmuxTuiSurfaceProvider(links: links)
        let catalog = SurfaceCatalog.shared
        catalog.fixtureProvider = provider
        catalog.projections = []
        let local = Workspace()
        local.usesSSHTui = false
        local.remoteConnectionResumeEnabled = false
        AppDelegate.shared?.workspaces = [local]
        await links.disconnect()
        let reservation = CloudTerminalPaneReservation(workspaceID: local.id)
        local.cloudPendingCreations[reservation.panelID] = reservation
        let gate = FixtureCreationGate()
        local.runOptimisticCloudTerminalCreation(reservation: reservation, requestID: UUID(),
            destination: .workspace(id: local.id, placement: .tab), create: {
                await gate.wait()
                return try await provider.createTerminal(command: nil, cwd: nil, name: nil, remoteWorkspaceID: nil)
            }, onStart: {}, onFinish: {})
        await gate.waitUntilHeld()
        let stayedActive = !provider.isSSHTransportPaused
        await gate.release()
        let created = await local.cloudPaneCreationFailureStore.operation!.value
        let starts = await probe.starts
        await links.disconnect()
        catalog.fixtureProvider = nil
        AppDelegate.shared?.workspaces = []
        guard stayedActive, created, starts == 1 else {
            print("FAIL first local SSH pane: stayed active=\(stayedActive), created=\(created), carrier starts=\(starts)")
            exit(1)
        }
        print("PASS first local SSH pane owns transport throughout pending creation")
    }
}
