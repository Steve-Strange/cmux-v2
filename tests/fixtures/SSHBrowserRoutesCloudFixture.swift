import Foundation

public struct CloudBrowserProxyEndpoint: Sendable, Equatable {}
public struct CloudMachineLink {
    public static func errorText(_ error: Error) -> String { String(describing: error) }
}
public struct IPNetworkPrefix {
    public static func routeHost(_ raw: String) -> String? { URL(string: raw)?.host }
    public static func isPrivateAddress(_ host: String) -> Bool { false }
}
public struct CloudTunnelStatus {
    public init(backend: Int, state: some Sendable, isPinned: Bool) {}
    public var privateRouteBlocker: String? { nil }
}
public struct FixtureSSHConfiguration: Sendable { public init() {} }
public struct SSHTuiConnection: Sendable {
    public init(configuration: FixtureSSHConfiguration) {}
    public let id = "ssh:fixture"
}
