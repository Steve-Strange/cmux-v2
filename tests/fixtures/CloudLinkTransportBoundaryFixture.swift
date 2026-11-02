import Foundation

// Control-service doubles keep the test independent of authentication and
// libghostty. The actual headless Process and pipe readers remain in the link.
public enum SurfaceLinkState: String, Sendable { case connecting, connected, unavailable, error }

public struct CloudVMCursor: Equatable, Sendable {
    public let generation: String
    public let revision: UInt64
    public init?(wire: [String: Any]) {
        guard let generation = wire["generation"] as? String,
              let revision = CloudWireNumber.unsigned(wire["revision"]) else { return nil }
        self.generation = generation
        self.revision = revision
    }
}

public struct CmuxTuiSnapshotParser {
    public static func localSocket(fromLinkLine line: String) -> String? {
        guard let data = line.data(using: .utf8),
              let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              value["event"] as? String == "connection-snapshot" else { return nil }
        return value["local_socket"] as? String
    }
}

public struct CloudTuiClientPaths: Sendable {
    public let stateDir: URL
    public init(home: URL) { stateDir = home }
    public func ensureStateDir() throws {}
    public static func deviceName() -> String { "fixture" }
}

public struct SSHTuiConnection: Sendable {
    public var sshProcessEnvironment: [String: String] { ProcessInfo.processInfo.environment }
    public func arguments(stateDirectory: String, deviceName: String, upgrade: Bool) -> [String] { [] }
}

public struct CloudTuiCommandLine {
    public static func linkArguments(route: String, deviceName: String, stateDir: String,
        carrier: Bool, wireguardHubSocket: String?, session: String?, sshArguments: [String]) -> [String] { [] }
}

public struct CloudTuiRequest: Sendable {
    public let operation: String
    public init(_ operation: String) { self.operation = operation }
}

struct CloudWireNumber {
    static func unsigned(_ value: Any?) -> UInt64? {
        if let value = value as? String { return UInt64(value) }
        return (value as? NSNumber)?.uint64Value
    }
}

struct CloudOperationContext {
    enum Phase { case process }
    static func phase<Value>(_ phase: Phase, operation: () async throws -> Value) async rethrows -> Value {
        try await operation()
    }
}

actor CloudLinkControlFixture {
    static let shared = CloudLinkControlFixture()
    private var eventContinuations: [AsyncStream<Data>.Continuation] = []
    private(set) var starts = 0
    private(set) var closes = 0
    private var startWaiters: [(count: Int, value: CloudLinkFirstValue<Bool>)] = []
    func started(_ continuation: AsyncStream<Data>.Continuation) {
        starts += 1
        eventContinuations.append(continuation)
        for waiter in startWaiters where starts >= waiter.count { waiter.value.resolve(true) }
    }
    func waitForStarts(_ count: Int) async {
        guard starts < count else { return }
        let value = CloudLinkFirstValue<Bool>()
        startWaiters.append((count, value))
        _ = await value.result
    }
    func closed() { closes += 1 }
    func emitSnapshot(revision: UInt64) {
        let item: [String: Any] = [
            "kind": "snapshot", "cursor": ["generation": "fixture", "revision": String(revision)],
            "snapshot": ["workspaces": [], "screens": [], "panes": [], "tabs": [], "terminals": []]
        ]
        let data = try! JSONSerialization.data(withJSONObject: item)
        for continuation in eventContinuations { continuation.yield(data) }
    }
}

public actor CloudTuiPersistentResourceConnection {
    public private(set) var isClosed = false
    private let eventsPair = AsyncStream<Data>.makeStream()
    public init(socketPath: String, clock: any Clock<Duration>) {}
    public func start() {}
    public func close() async {
        isClosed = true
        eventsPair.continuation.finish()
        await CloudLinkControlFixture.shared.closed()
    }
    public func request(_ request: CloudTuiRequest, timeout: Duration) throws -> Data {
        if request.operation == "timeout" { throw CloudMachineLink.LinkError.requestTimedOut }
        return Data("{}".utf8)
    }
    public func sendUntracked(_ request: CloudTuiRequest) {}
    public func events(cursor: CloudVMCursor?) async -> (id: String, stream: AsyncStream<Data>) {
        await CloudLinkControlFixture.shared.started(eventsPair.continuation)
        return ("fixture-events", eventsPair.stream)
    }
    public func cancelStream(_ id: String) {}
}
