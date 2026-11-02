import CmuxCloud
import Foundation

@main
struct SSHBrowserRoutesLifecycleFixture {
    @MainActor
    static func main() async throws {
        let links = SSHTuiLinkManager()
        let provider = CmuxTuiSurfaceProvider(links: links)
        let workspace = Workspace()
        let browser = BrowserPanel()
        let target = CloudPortForwardTarget(host: "127.0.0.1", port: 4000)
        var starts = 0
        let model = provider.portAccessStore.model(machineID: provider.machineID, target: target) {
            CloudPortAccessModel(target: target, coordinator: nil, wake: {}, startForward: { _ in
                starts += 1
                return starts == 1 ? 51_001 : 51_002
            }, stopForward: {}, route: .loopback)
        }
        browser.cloudAccess.model = model
        workspace.panels[browser.id] = browser
        AppDelegate.shared?.workspaces = [workspace]
        provider.catalog.projections = [.init(workspaceID: workspace.id, panelID: browser.id)]
        let pausedSibling = Workspace()
        pausedSibling.remoteConnectionResumeEnabled = false
        let siblingBrowser = BrowserPanel()
        let siblingTarget = CloudPortForwardTarget(host: "127.0.0.1", port: 4001)
        var siblingStarts = 0
        let siblingModel = provider.portAccessStore.model(machineID: provider.machineID, target: siblingTarget) {
            CloudPortAccessModel(target: siblingTarget, coordinator: nil, wake: {}, startForward: { _ in
                siblingStarts += 1
                return 51_003
            }, stopForward: {}, route: .loopback)
        }
        siblingBrowser.cloudAccess.model = siblingModel
        pausedSibling.panels[siblingBrowser.id] = siblingBrowser
        AppDelegate.shared?.workspaces.append(pausedSibling)
        provider.catalog.projections.append(.init(workspaceID: pausedSibling.id, panelID: siblingBrowser.id))
        siblingModel.connect()
        await settled(siblingModel)
        model.connect()
        await settled(model)
        let service = URL(string: "http://localhost:4000/projects/123?tab=logs#tail")!
        precondition(model.url(for: service)?.port == 51_001)

        workspace.remoteConnectionResumeEnabled = false
        let teardown = provider.pauseSSHTransportIfUnused(excluding: workspace.id)
        guard !model.isReady, model.url(for: service) == nil else {
            print("FAIL paused SSH browser still advertises its retired listener as ready")
            exit(1)
        }
        await teardown?.value
        model.retry()
        await Task.yield()
        precondition(!model.isReady && starts == 1, "automatic route retry bypassed explicit pause")

        workspace.remoteConnectionResumeEnabled = true
        provider.resumeSSHTransport()
        await settled(model)
        precondition(starts == 2, "workspace reconnect did not replace its browser forward")
        precondition(model.url(for: service)?.absoluteString == "http://127.0.0.1:51002/projects/123?tab=logs#tail",
                     "the original pane retained the retired listener URL")
        precondition(!siblingModel.isReady && siblingStarts == 1, "shared reconnect resumed a deliberately paused sibling")
        print("PASS pause removes readiness; resume replaces listener and preserves service URL")
        try await localProjectionKeepsRoute()
        await pendingStopCannotCloseResumedRoute()
    }

    @MainActor
    private static func localProjectionKeepsRoute() async throws {
        let provider = CmuxTuiSurfaceProvider(links: SSHTuiLinkManager())
        let workspace = Workspace()
        workspace.usesSSHTui = false
        workspace.remoteConnectionResumeEnabled = false
        let browser = BrowserPanel()
        let target = CloudPortForwardTarget(host: "127.0.0.1", port: 8000)
        let model = provider.portAccessStore.model(machineID: provider.machineID, target: target) {
            CloudPortAccessModel(target: target, coordinator: nil, wake: {}, startForward: { _ in 51_004 },
                stopForward: {}, route: .loopback)
        }
        browser.cloudAccess.model = model
        workspace.panels[browser.id] = browser
        AppDelegate.shared?.workspaces = [workspace]
        provider.catalog.projections = [.init(workspaceID: workspace.id, panelID: browser.id)]
        model.connect()
        await settled(model)
        precondition(provider.pauseSSHTransportIfUnused() == nil && model.isReady,
                     "an independent local projection lost its shared forward")
        print("PASS an independent local browser projection keeps its route")
    }

    @MainActor
    private static func pendingStopCannotCloseResumedRoute() async {
        let gate = Gate()
        var starts = 0
        var stops = 0
        let model = CloudPortAccessModel(target: .init(host: "127.0.0.1", port: 9000), coordinator: nil,
            wake: {}, startForward: { _ in
                starts += 1
                if starts == 1 { await gate.wait() }
                return starts == 1 ? 51_005 : 51_006
            }, stopForward: { stops += 1 }, route: .loopback)
        model.connect()
        for _ in 0..<1_000 {
            if starts == 1 { break }
            await Task.yield()
        }
        precondition(starts == 1)
        let oldStop = Task { await model.stop() }
        for _ in 0..<1_000 {
            if model.phase == .stopping { break }
            await Task.yield()
        }
        precondition(model.phase == .stopping)
        model.invalidateTransport()
        model.resumeTransport()
        gate.release()
        await oldStop.value
        await settled(model)
        precondition(starts == 2 && stops == 0 && model.localAddress == "127.0.0.1:51006",
                     "an older route stop closed its replacement")
        print("PASS old asynchronous route cleanup cannot close the resumed listener")
    }

    @MainActor
    private final class Gate {
        private var continuation: CheckedContinuation<Void, Never>?
        func wait() async { await withCheckedContinuation { continuation = $0 } }
        func release() { continuation?.resume(); continuation = nil }
    }

    @MainActor
    private static func settled(_ model: CloudPortAccessModel) async {
        for _ in 0..<1_000 {
            if model.isReady { return }
            await Task.yield()
        }
        preconditionFailure("route never became ready")
    }
}
