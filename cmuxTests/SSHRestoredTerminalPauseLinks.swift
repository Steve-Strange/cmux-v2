import CmuxCloud
import Foundation

#if canImport(cmux_DEV)
@testable import cmux_DEV
#elseif canImport(cmux)
@testable import cmux
#endif

/// Holds the carrier await so a test can disconnect before a pane gets a link.
actor SSHRestoredTerminalPauseLinks: RemoteTuiLinkManaging {
    nonisolated let operations: CloudOperationRecorder? = nil
    nonisolated let started = CloudLinkFirstValue<Bool>()
    nonisolated let release = CloudLinkFirstValue<CloudMachineLink.Connected>()
    private(set) var linkCount = 0

    func connected(machineID: String) async throws -> CloudMachineLink.Connected {
        started.resolve(true)
        guard let connected = await release.result else { throw CancellationError() }
        return connected
    }

    func link(machineID: String) async -> CloudMachineLink? { linkCount += 1; return nil }
    func status(machineID: String) async -> CloudMachineLinkManager.LinkStatus? { nil }
    func privateAddresses(for machineID: String) async -> [String] { [] }
    func setPrivateAddresses(_ addresses: [String], for machineID: String) async {}
    func browserProxy(machineID: String, remoteNetwork: Bool) async throws -> CloudBrowserProxyEndpoint {
        throw URLError(.cannotConnectToHost)
    }
}
