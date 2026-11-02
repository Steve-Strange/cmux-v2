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
        print("PASS pause removes readiness; resume replaces listener and preserves service URL")
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
