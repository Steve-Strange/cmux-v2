import Foundation

public struct CloudOperationRecorder: Sendable {}
public struct CloudBrowserProxyEndpoint: Sendable {}
public actor CloudCarrierProbe {
    public init() {}
    public private(set) var starts = 0
    public private(set) var stops = 0
    private var holdStop = false
    private var heldStop: CheckedContinuation<Void, Never>?
    private var stopObserver: CheckedContinuation<Void, Never>?
    public func started() { starts += 1 }
    public func holdNextStop() { holdStop = true }
    public func waitUntilStopHeld() async {
        guard heldStop == nil else { return }
        await withCheckedContinuation { stopObserver = $0 }
    }
    public func releaseStop() { heldStop?.resume(); heldStop = nil }
    public func stopped() async {
        stops += 1
        guard holdStop else { return }
        holdStop = false
        await withCheckedContinuation { continuation in
            heldStop = continuation
            stopObserver?.resume()
            stopObserver = nil
        }
    }
}
public struct CloudTuiClientPaths: Sendable {
    public let stateDir = URL(fileURLWithPath: "/unused")
    public let probe: CloudCarrierProbe
    public init(probe: CloudCarrierProbe) { self.probe = probe }
    public static func deviceName() -> String { "fixture" }
}
public struct FixtureSSHConfiguration: Sendable {
    public let destination = "fixture"
    public let sshOptions: [String] = []
    public let agentSocketPath: String? = nil
    public let agentSocketPathOverrideIsSet = false
    public init() {}
}
public struct SSHTuiConnection: Sendable {
    public let configuration = FixtureSSHConfiguration()
    public let id = "ssh:fixture"
    public let session = "cmux"
    public var agentHookProviders: [String] = []
    public var sshProcessEnvironment: [String: String] { [:] }
    public init() {}
    public init(configuration: FixtureSSHConfiguration) {}
    public func browserArguments(stateDirectory: String) -> [String] { [] }
    public func arguments(stateDirectory: String, deviceName: String) -> [String] { [] }
}
public actor CloudMachineLink {
    public enum LinkError: Error { case timedOut, failureMessage(String) }
    public struct Connected: Sendable {
        public let socketPath = "/unused"
        public let session = "cmux"
        public init() {}
    }
    public enum State: Sendable { case connected, connecting, unavailable }
    public private(set) var connected: Connected?
    public private(set) var state = State.unavailable
    public var lastError: String? { nil }
    public var isConnected: Bool { connected != nil && state == .connected }
    public var isSocketReady: Bool { connected != nil }
    public func beginRecovery() { state = .connecting }
    private let probe: CloudCarrierProbe
    public init(machineID: String, clientURL: URL, paths: CloudTuiClientPaths) { probe = paths.probe }
    public func connect(route: String, session: String, carrier: Bool, timeout: Duration,
                        ssh: SSHTuiConnection, sshUpgrade: Bool) async throws -> Connected {
        try Task.checkCancellation()
        await probe.started()
        let result = Connected()
        connected = result
        state = .connected
        return result
    }
    public func disconnect() async {
        if connected != nil { await probe.stopped() }
        connected = nil
        state = .unavailable
    }
}
public struct CloudMachineLinkManager {
    public struct LinkStatus: Sendable {
        public let state: CloudMachineLink.State
        public let error: String?
        public init(state: CloudMachineLink.State, error: String?) { self.state = state; self.error = error }
    }
}
public actor CloudBrowserProxyProcess {
    public var readyEndpoint: CloudBrowserProxyEndpoint? { nil }
    public init(addresses: [String]) {}
    public func start(client: URL, arguments: [String], environment: [String: String],
                      releaseHub: @escaping @Sendable () async -> Void) async throws -> CloudBrowserProxyEndpoint {
        CloudBrowserProxyEndpoint()
    }
    public func stop() {}
    public static func sanitizedEnvironment(_ environment: [String: String]) -> [String: String] { environment }
}
public struct CloudLinkPipe {
    public static func lines(from handle: FileHandle) -> AsyncStream<String> { AsyncStream { $0.finish() } }
}
public final class CloudLinkFirstValue<Value: Sendable>: Sendable {
    public init() {}
    public func resolve(_ value: Value?) {}
    public var result: Value? { get async { nil } }
}
