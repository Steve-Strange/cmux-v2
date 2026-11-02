import CmuxCore
import CmuxCloudTui
import Foundation

// Value-only workspace host. The production tab projection and status labels
// below run unchanged; no terminal process or live server is involved.
enum PanelType { case terminal, browser }
struct Panel { var panelType: PanelType = .terminal; var displayTitle = "Codex" }
enum Transport { case ssh }
struct Profile { var tmuxSessionName: String? = "de" }
struct Configuration { var transport: Transport = .ssh; var terminalProfile = Profile() }
enum Phase { case launching, connected, ended }
struct SessionState { let phase: Phase }
struct Machine { var isSSH: Bool }
struct MirrorSession { var phase: CloudTuiManualMirrorPhase; var allowsAutomaticReconnect = true }
struct Tab { var title = "" }
final class Controller {
    var titles: [UUID: String] = [:]
    func tab(_ id: UUID) -> Tab? { titles[id].map { Tab(title: $0) } }
    func updateTab(_ id: UUID, title: String) { titles[id] = title }
}
final class Workspace {
    var panels: [UUID: Panel] = [:]
    var remoteConfiguration: Configuration? = Configuration()
    var isRemoteTmuxMirror = false
    var remoteDisconnectPlaceholderPanelIds = Set<UUID>()
    var endedPersistentRemotePTYAttachSurfaceIds = Set<UUID>()
    var activeRemoteTerminalSurfaceIds = Set<UUID>()
    var remoteTerminalSessionStatesBySurfaceId: [UUID: SessionState] = [:]
    var remoteConnectionState: WorkspaceRemoteConnectionState = .connected
    var usesSSHTui = false
    var remoteConnectionResumeEnabled = true
    var carrierConnected: Bool? = nil
    var requiresManualAuthentication = false
    var remoteDisplayTarget: String? = "gpusp"
    var panelTitles: [UUID: String] = [:]
    var directories: [UUID: String] = [:]
    var machinesByPanelID: [UUID: Machine] = [:]
    var mirrorsByPanelID: [UUID: MirrorSession] = [:]
    var cloudPendingCreations: [UUID: Bool] = [:]
    let bonsplitController = Controller()
    func resolvedPanelTitle(panelId: UUID, fallback: String) -> String { panelTitles[panelId] ?? fallback }
    func effectivePanelDirectory(panelId: UUID) -> String? { directories[panelId] }
    func isRemoteTerminalSurface(_ panelId: UUID) -> Bool { activeRemoteTerminalSurfaceIds.contains(panelId) }
    func machineOwningSurface(_ panelId: UUID) -> Machine? { machinesByPanelID[panelId] }
    func tuiMirrorSession(for panelId: UUID) -> MirrorSession? { mirrorsByPanelID[panelId] }
    func sshConnectionPresentation() -> SSHConnectionPresentation? {
        guard usesSSHTui else { return nil }
        return SSHConnectionPresentation(resumeEnabled: remoteConnectionResumeEnabled,
            controllerState: remoteConnectionState, carrierConnected: carrierConnected,
            requiresManualAuthentication: requiresManualAuthentication,
            attachments: mirrorsByPanelID.values.map { .init(phase: $0.phase, allowsAutomaticReconnect: $0.allowsAutomaticReconnect) })
    }
    func surfaceIdFromPanelId(_ panelId: UUID) -> UUID? { panels[panelId] == nil ? nil : panelId }
}

@main
struct TerminalConnectionPresentationFixture {
    static func main() {
        let remote = UUID()
        let local = UUID()
        let browser = UUID()
        let workspace = Workspace()
        workspace.panels = [remote: Panel(), local: Panel(), browser: Panel(panelType: .browser)]
        workspace.panelTitles[remote] = "Codex"
        workspace.directories = [remote: "/home/wangziteng/decision_dsp", local: "/Users/wzt/cmux"]
        workspace.activeRemoteTerminalSurfaceIds.insert(remote)
        workspace.refreshTerminalConnectionTitles()
        var failures: [String] = []
        let connected = workspace.bonsplitController.titles[remote] ?? ""
        if !connected.contains("Remote · gpusp") || !connected.contains("decision_dsp") || !connected.contains("de") { failures.append("Remote tab omits host, directory or tmux identity: \(connected)") }
        if workspace.bonsplitController.titles[local]?.hasPrefix("Local | ") != true { failures.append("A local shell in an SSH workspace must say Local") }
        if workspace.panelTitles[remote] != "Codex" { failures.append("Display context leaked into process metadata") }
        if workspace.tabDisplayTitle(panelId: browser, fallback: "Docs") != "Docs" { failures.append("Browser title was decorated as a terminal") }

        workspace.remoteTerminalSessionStatesBySurfaceId[remote] = SessionState(phase: .launching)
        let launching = workspace.tabDisplayTitle(panelId: remote, fallback: "Codex")
        if !launching.hasPrefix("Connecting · gpusp") { failures.append("Connected sibling made an attaching pane appear connected: \(launching)") }
        workspace.remoteTerminalSessionStatesBySurfaceId[remote] = SessionState(phase: .ended)
        let ended = workspace.tabDisplayTitle(panelId: remote, fallback: "Codex")
        if !ended.hasPrefix("Disconnected · gpusp") { failures.append("Ended remote pane still appears connected") }
        workspace.remoteDisconnectPlaceholderPanelIds.insert(remote)
        workspace.activeRemoteTerminalSurfaceIds.remove(remote)
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Disconnected · gpusp") { failures.append("Disconnected placeholder was misidentified as Local") }
        workspace.remoteDisconnectPlaceholderPanelIds.remove(remote)
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Local | ") { failures.append("A returned local shell retained remote identity") }

        // Default managed SSH terminals use manual mirror projections, not
        // the legacy activeRemoteTerminalSurfaceIds registration path. The
        // ownership seam also resolves pending reservations during restore.
        workspace.remoteTerminalSessionStatesBySurfaceId.removeValue(forKey: remote)
        workspace.machinesByPanelID[remote] = Machine(isSSH: true)
        let projected = workspace.tabDisplayTitle(panelId: remote, fallback: "Codex")
        if !projected.hasPrefix("Remote · gpusp") { failures.append("An SSH manual mirror projection was labeled Local: \(projected)") }
        workspace.cloudPendingCreations[remote] = true
        let splitPending = workspace.tabDisplayTitle(panelId: remote, fallback: "Codex")
        if !splitPending.hasPrefix("Connecting · gpusp") { failures.append("A new SSH split borrowed a sibling's connected status: \(splitPending)") }
        workspace.cloudPendingCreations.removeValue(forKey: remote)
        workspace.mirrorsByPanelID[remote] = MirrorSession(phase: .disconnected)
        let detached = workspace.tabDisplayTitle(panelId: remote, fallback: "Codex")
        if !detached.hasPrefix("Disconnected · gpusp") { failures.append("A disconnected mirror borrowed its workspace's connected status: \(detached)") }
        workspace.mirrorsByPanelID[remote] = MirrorSession(phase: .connecting)
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Connecting · gpusp") { failures.append("A connecting mirror borrowed a sibling's connected status") }
        workspace.mirrorsByPanelID.removeValue(forKey: remote)
        workspace.remoteConnectionState = .connecting
        let reserved = workspace.tabDisplayTitle(panelId: remote, fallback: "Codex")
        if !reserved.hasPrefix("Connecting · gpusp") { failures.append("A reserved SSH pane was labeled Local: \(reserved)") }
        workspace.machinesByPanelID.removeValue(forKey: remote)
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Local | ") { failures.append("Removing SSH provenance left a remote label") }

        workspace.usesSSHTui = true
        workspace.remoteConnectionState = .connected
        workspace.carrierConnected = false
        workspace.machinesByPanelID[remote] = Machine(isSSH: true)
        workspace.mirrorsByPanelID[remote] = MirrorSession(phase: .disconnected)
        if workspace.sshConnectionPresentation()?.state != .reconnecting { failures.append("A dropped carrier still appears Connected in workspace status") }
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Reconnecting · gpusp") { failures.append("Network retry is missing from terminal chrome") }
        workspace.carrierConnected = true
        workspace.machinesByPanelID[local] = Machine(isSSH: true)
        workspace.mirrorsByPanelID[local] = MirrorSession(phase: .attached)
        if workspace.sshConnectionPresentation()?.state != .reconnecting { failures.append("One attached sibling hid another pane's automatic recovery") }
        if !workspace.tabDisplayTitle(panelId: local, fallback: "zsh").hasPrefix("Remote · gpusp") { failures.append("An attached pane borrowed a sibling's reconnecting status") }
        workspace.mirrorsByPanelID.removeValue(forKey: local)
        workspace.remoteConnectionResumeEnabled = false
        workspace.carrierConnected = true
        workspace.mirrorsByPanelID[remote] = MirrorSession(phase: .attached)
        if workspace.sshConnectionPresentation()?.state != .disconnected { failures.append("A shared connected carrier overrode explicit Disconnect") }
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Disconnected · gpusp") { failures.append("Paused terminal still appears connected") }
        workspace.remoteConnectionResumeEnabled = true
        workspace.carrierConnected = false
        workspace.requiresManualAuthentication = true
        workspace.mirrorsByPanelID[remote] = MirrorSession(phase: .idle, allowsAutomaticReconnect: false)
        if workspace.sshConnectionPresentation()?.state != .error { failures.append("Rejected authentication does not show a manual error") }
        if !workspace.tabDisplayTitle(panelId: remote, fallback: "Codex").hasPrefix("Error · gpusp") { failures.append("Authentication failure differs between workspace and terminal") }
        workspace.remoteConfiguration = nil
        if workspace.tabDisplayTitle(panelId: local, fallback: "zsh") != "zsh" { failures.append("Ordinary local workspaces should preserve their existing title") }
        let root = TerminalConnectionPresentation(origin: "Local", title: "zsh", directory: "/", sessionName: nil)
        if !root.tabTitle.hasSuffix(" | /") { failures.append("Root directory disappeared") }
        if !failures.isEmpty {
            FileHandle.standardError.write(Data((failures.joined(separator: "\n") + "\n").utf8))
            exit(1)
        }
        print("PASS: per-pane host, directory, tmux session, SSH projections and truthful local/connecting/disconnected states")
    }
}
