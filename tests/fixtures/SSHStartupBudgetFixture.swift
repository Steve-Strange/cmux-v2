import Foundation

// Only route/settings and the control-service boundary are substituted. The
// connection's argument builder, preflight and carrier lifecycle are production.
public struct WorkspaceRemoteConfiguration: Sendable {
    public let destination = "fixture.invalid"
    public let port: Int? = nil
    public let identityFile: String? = nil
    public let sshOptions: [String] = []
    public let agentSocketPath: String? = nil
    public let agentSocketPathOverrideIsSet = false
    public let routeSensitiveIdentifier: String? = nil
    public let routeSensitivity = FixtureRouteSensitivity.ordinary
    public let restoredSSHSession: FixtureRestoredSession? = nil
    public let terminalProfile = FixtureTerminalProfile()
    public let configuredRemoteCommand: String? = nil
    public func sshProcessEnvironment(inheriting environment: [String: String]) -> [String: String] { environment }
}
public enum FixtureRouteSensitivity { case ordinary, unidentified
    public var markerOptions: [String] { [] }
}
public struct FixtureRestoredSession { public let sshSessionOwner: String? = nil }
public struct FixtureTerminalProfile {
    public let tmuxSessionName: String? = nil
    public let remoteCommandArguments: [String] = []
}
public struct SSHAgentSocketResolver {
    public init(environment: [String: String]) {}
    public func optionKey(_ option: String) -> String? { option.split(separator: "=").first.map { $0.lowercased() } }
    public func optionValue(named: String, in options: [String]) -> String? { nil }
}
public struct SSHConnectionSharingOptions {
    public static let unidentifiedRouteSensitiveMarkerOption = "fixture"
    public func cmuxOwnedControlPath(in options: [String]) -> String? { nil }
    public func mergingDefaults(into options: [String], routeSensitiveOptions: [String], routeIdentifier: String) -> [String] { options }
}
public struct AgentIntegrationSettingsStore {
    public init(defaults: UserDefaults) {}
    public let claudeCodeHooksEnabled = false
    public let codexHooksEnabled = false
}
public struct RemoteTmuxCommandBuilder {
    public init(arguments: [String]) {}
    public var remoteCommandArguments: [String] { [] }
}
public struct CommandResult: Sendable {
    public let stderr: String?
    public let exitStatus: Int32?
    public let timedOut: Bool
    public let executionError: String?
}
public protocol CommandRunning: Sendable {
    func run(directory: String, executable: String, arguments: [String], timeout: TimeInterval?) async -> CommandResult
}
public struct CommandRunner: CommandRunning {
    public func run(directory: String, executable: String, arguments: [String], timeout: TimeInterval?) async -> CommandResult {
        fatalError("the fixture must inject its authentication runner")
    }
}
private actor AuthenticationFailureCommands: CommandRunning {
    private(set) var timeout: TimeInterval?
    private(set) var arguments: [String] = []
    func run(directory: String, executable: String, arguments: [String], timeout: TimeInterval?) -> CommandResult {
        self.timeout = timeout
        self.arguments = arguments
        return CommandResult(stderr: "Permission denied (publickey).", exitStatus: 255,
                             timedOut: false, executionError: nil)
    }
}

@main
struct SSHStartupBudgetFixture {
    struct Failure: Error, CustomStringConvertible { let description: String }
    static func require(_ condition: Bool, _ message: String) throws {
        if !condition { throw Failure(description: message) }
    }

    static func main() async {
        do {
            let connection = SSHTuiConnection(configuration: WorkspaceRemoteConfiguration(), environment: [:])
            let commands = AuthenticationFailureCommands()
            do {
                try await SSHTuiPreflight(connection: connection, commands: commands).run()
                throw Failure(description: "authentication failure was hidden by carrier startup")
            } catch SSHTuiPreflightError.sshFailed(let detail) {
                try require(detail.contains("Permission denied"), "OpenSSH authentication detail was lost")
            }
            try require(await commands.timeout == 30, "bootstrap budget extended authentication preflight")
            try require(await commands.arguments.contains("ConnectTimeout=15"), "OpenSSH's short route check was lost")
            try require(!connection.arguments(stateDirectory: "/tmp/state", deviceName: "fixture")
                .contains("--connect-timeout-seconds"), "argument callers without a native deadline gained a startup limit")
            try await exercise(connection: connection, timeout: .seconds(180), expectedSeconds: 179)
            try await exercise(connection: connection, timeout: .milliseconds(42_750), expectedSeconds: 41)
            print("PASS native startup budgets reach the child; authentication remains bounded separately")
        } catch {
            print("FAIL \(error)")
            exit(1)
        }
    }

    static func exercise(connection: SSHTuiConnection, timeout: Duration, expectedSeconds: Int) async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("cmux-ssh-budget-\(UUID())")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let client = root.appendingPathComponent("client")
        let argv = root.appendingPathComponent("argv")
        try """
        #!/usr/bin/env python3
        import json,signal,sys
        with open('\(argv.path)', 'w') as output: json.dump(sys.argv[1:], output)
        print(json.dumps({'event':'connection-snapshot','local_socket':'/tmp/budget-fixture.sock',
                          'connection':{'state':'connected'}}), flush=True)
        signal.pause()
        """.write(to: client, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: client.path)
        let link = CloudMachineLink(machineID: "fixture", clientURL: client, paths: CloudTuiClientPaths(home: root))
        do {
            _ = try await link.connect(route: "ssh://fixture.invalid", session: "cmux", carrier: true,
                                       timeout: timeout, ssh: connection)
            let arguments = try JSONDecoder().decode([String].self, from: Data(contentsOf: argv))
            guard let flagIndex = arguments.firstIndex(of: "--connect-timeout-seconds"),
                  arguments.indices.contains(flagIndex + 1) else {
                throw Failure(description: "native startup budget never reached the SSH carrier child")
            }
            try require(arguments[flagIndex + 1] == String(expectedSeconds),
                        "carrier startup budget did not reserve the native deadline's cleanup second: \(arguments)")
            try require(!arguments.contains("--reconnect-attempts") && !arguments.contains("--reconnect-attempt-timeout-ms"),
                        "startup budget capped the carrier's later reconnect lifetime")
            await link.disconnect()
        } catch {
            await link.disconnect()
            throw error
        }
    }
}
