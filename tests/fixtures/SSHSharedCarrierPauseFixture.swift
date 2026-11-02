import CmuxCloud
import Foundation

protocol RemoteTuiLinkManaging: Sendable {}
struct SSHTuiPreflight {
    let connection: SSHTuiConnection
    func run() async throws {}
}

@main
struct SSHSharedCarrierPauseFixture {
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
    }
}
