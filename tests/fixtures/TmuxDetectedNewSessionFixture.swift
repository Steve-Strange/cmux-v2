import Foundation

struct AgentLaunchCommandSnapshot: Codable {
    let launcher: String?
    let executablePath: String?
    let arguments: [String]
    let workingDirectory: String?
    let environment: [String: String]?
    let capturedAt: TimeInterval
    let source: String
}

struct SurfaceResumeBindingSnapshot: Codable {
    let name: String
    let kind: String
    let command: String
    let cwd: String?
    let checkpointId: String?
    let source: String
    let environment: [String: String]?
    let launchCommand: AgentLaunchCommandSnapshot
    let autoResume: Bool
    let updatedAt: TimeInterval
}

struct CmuxTopProcessInfo: Sendable {
    let pid: Int
    let name: String
    let path: String?
    let ttyDevice: Int64?
    let cmuxWorkspaceID: UUID?
    let cmuxSurfaceID: UUID?
    let isTerminalForegroundProcessGroup: Bool
}

struct CmuxTopProcessArguments: Sendable {
    let arguments: [String]
    let environment: [String: String]
}

struct CmuxTopProcessSnapshot {
    let processesByPID: [Int: CmuxTopProcessInfo]
    static func processArgumentsAndEnvironment(for _: Int) -> CmuxTopProcessArguments? { nil }
}

struct SurfaceResumeBindingIndex {
    struct PanelKey: Hashable {
        let workspaceId: UUID
        let panelId: UUID
    }
}

enum TerminalSSHSessionDetector {
    static func resumeBinding(processName: String, processPath: String?, arguments: [String],
                              environment: [String: String], capturedAt: TimeInterval) -> SurfaceResumeBindingSnapshot? {
        nil
    }
}

@main
struct TmuxDetectedNewSessionFixture {
    static func main() throws {
        let executable = CommandLine.arguments[1]
        let socket = CommandLine.arguments[2]
        let pid = Int(CommandLine.arguments[3])!
        let workspace = UUID()
        let panel = UUID()
        let process = CmuxTopProcessInfo(pid: pid, name: "tmux", path: executable,
            ttyDevice: 1234, cmuxWorkspaceID: workspace, cmuxSurfaceID: panel,
            isTerminalForegroundProcessGroup: true)
        let snapshot = CmuxTopProcessSnapshot(processesByPID: [pid: process])
        let result = SurfaceResumeBindingIndex.processDetectedTmuxBindings(
            fileManager: .default, processSnapshot: snapshot, capturedAt: 1_777_777_777,
            processArgumentsProvider: { _ in
                CmuxTopProcessArguments(arguments: [executable, "-S", socket, "new", "-s", "cmux"],
                                        environment: ["PWD": "/tmp"])
            })
        let binding = result[SurfaceResumeBindingIndex.PanelKey(workspaceId: workspace, panelId: panel)]?.binding
        print(String(decoding: try JSONEncoder().encode(binding), as: UTF8.self))
    }
}
