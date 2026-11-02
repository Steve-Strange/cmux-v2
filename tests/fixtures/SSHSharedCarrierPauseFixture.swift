import CmuxCloud
import Foundation

protocol RemoteTuiLinkManaging: Sendable {}
struct SSHTuiPreflight {
    let connection: SSHTuiConnection
    func run() async throws {}
}

@main
struct SSHSharedCarrierPauseFixture {
    @MainActor
    static func main() async throws {
        let probe = CloudCarrierProbe()
        let links = SSHTuiLinkManager(connection: SSHTuiConnection(),
            clientURL: URL(fileURLWithPath: "/unused"), paths: CloudTuiClientPaths(probe: probe),
            isEnabled: { true })
        _ = try await links.connected(machineID: "ssh:fixture")
        await links.disconnect()
        var implicitRestarted = false
        do {
            _ = try await links.connected(machineID: "ssh:fixture")
            implicitRestarted = true
        } catch is CancellationError {
            // Intentional disconnect must stay disconnected until explicit resume.
        }
        let starts = await probe.starts
        let stops = await probe.stops
        await links.disconnect()
        print("carrier starts=\(starts), stops=\(stops), restarted after disconnect=\(implicitRestarted)")
        if implicitRestarted || starts != 1 || stops != 1 { exit(1) }
        try await sharedConsumerAndResumeCases()
        try await resumeDuringTeardown()
    }

    @MainActor
    private static func sharedConsumerAndResumeCases() async throws {
        let probe = CloudCarrierProbe()
        let links = SSHTuiLinkManager(connection: SSHTuiConnection(),
            clientURL: URL(fileURLWithPath: "/unused"), paths: CloudTuiClientPaths(probe: probe),
            isEnabled: { true })
        let provider = CmuxTuiSurfaceProvider(links: links)
        let session = FixtureSession()
        provider.manualMirrorSessions[UUID()] = session
        precondition(provider.pauseSSHTransportIfUnused() == nil, "another pane owns the carrier")
        session.allowsAutomaticReconnect = false

        let sibling = Workspace()
        AppDelegate.shared?.workspaces = [sibling]
        precondition(provider.pauseSSHTransportIfUnused() == nil, "an attaching workspace has no projection yet")
        sibling.remoteConnectionResumeEnabled = false
        let local = Workspace()
        local.usesSSHTui = false
        local.remoteConnectionResumeEnabled = false
        AppDelegate.shared?.workspaces = [sibling, local]
        provider.catalog.projections = [FixtureProjection(workspaceID: local.id)]
        precondition(provider.pauseSSHTransportIfUnused() == nil, "an independent local projection owns the carrier")
        provider.catalog.projections = []

        let opening = provider.beginExplicitSSHTransportUse()!
        precondition(provider.pauseSSHTransportIfUnused() == nil, "preflight owns the carrier before workspace creation")
        provider.endExplicitSSHTransportUse(opening)
        precondition(provider.isSSHTransportPaused, "the last owner leaving pauses synchronously")
        precondition(provider.notificationSync == nil, "pause retires notification retries")
        do {
            _ = try await links.connected(machineID: "ssh:fixture")
            fatalError("a query restarted the carrier")
        } catch is CancellationError {}

        // The old pause is still queued. Explicit resume must survive its teardown.
        let oldRevision = links.transportIntent.revision
        provider.resumeSSHTransport()
        _ = try await links.connected(machineID: "ssh:fixture")
        await links.disconnect(ifPausedAt: oldRevision)
        let resumedLink = await links.link(machineID: "ssh:fixture")
        let resumedStarts = await probe.starts
        let resumedStops = await probe.stops
        precondition(resumedLink != nil, "stale teardown killed a new carrier")
        precondition(provider.notificationSync != nil, "resume reinstalls notification ownership")
        precondition(resumedStarts == 1, "explicit resume starts exactly one carrier")
        precondition(resumedStops == 0, "stale pause must not stop the new carrier")
        await links.disconnect()
        AppDelegate.shared?.workspaces = []
        print("PASS sibling/local/open owners, paused query, stale teardown and explicit resume")
    }

    @MainActor
    private static func resumeDuringTeardown() async throws {
        let probe = CloudCarrierProbe()
        let links = SSHTuiLinkManager(connection: SSHTuiConnection(),
            clientURL: URL(fileURLWithPath: "/unused"), paths: CloudTuiClientPaths(probe: probe),
            isEnabled: { true })
        _ = try await links.connected(machineID: "ssh:fixture")
        await probe.holdNextStop()
        let revision = links.transportIntent.pause(force: true)!
        let teardown = Task { await links.disconnect(ifPausedAt: revision) }
        // Old transport cleanup is already suspended when a new Open arrives.
        await probe.waitUntilStopHeld()
        links.transportIntent.resume()
        _ = try await links.connected(machineID: "ssh:fixture")
        await probe.releaseStop()
        await teardown.value
        let replacement = await links.link(machineID: "ssh:fixture")
        let starts = await probe.starts
        let stops = await probe.stops
        precondition(replacement != nil, "finishing old teardown lost the replacement")
        precondition(starts == 2 && stops == 1, "old teardown stopped the resumed carrier")
        await links.disconnect()
        print("PASS explicit resume while old carrier teardown is suspended")
    }
}
