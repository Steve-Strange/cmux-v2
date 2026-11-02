import CmuxCloud
import CmuxSurfaceCatalogModel
import Foundation
import CmuxCore

extension Workspace {
    /// CLI status is derived from the same carrier and graph as native panes.
    func tuiSSHStatusPayload() -> [String: Any] {
        guard let configuration = remoteConfiguration else { return [:] }
        let connection = SSHTuiConnection(configuration: configuration)
        let machine = SurfaceMachineID(rawValue: connection.id)
        let catalog = SurfaceCatalog.shared
        let info = catalog.machineInfo(for: machine)
        let presentation = sshConnectionPresentation(catalog: catalog)
        let displayState = presentation?.state ?? remoteConnectionState
        let connected = displayState == .connected
        let state = displayState.rawValue
        let resources = catalog.authoritativeSnapshot.resources(on: machine)
        return [
            "enabled": true, "backend": "cmux-tui", "transport": "ssh", "terminal_transport": "ssh",
            "machine": machine.rawValue, "state": state, "connected": connected,
            "destination": configuration.destination, "port": configuration.port ?? NSNull(),
            "auto_reconnect": remoteConnectionResumeEnabled,
            "has_identity_file": configuration.identityFile != nil, "has_ssh_options": !configuration.sshOptions.isEmpty,
            "terminal_profile": configuration.terminalProfile.kind.rawValue,
            "terminal_tmux_session": configuration.terminalProfile.tmuxSessionName ?? NSNull(),
            "remote_workspace_id": cloudVMBinding?.remoteWorkspaceID ?? NSNull(),
            "active_terminal_sessions": remoteConnectionResumeEnabled && info?.linkState == .connected
                ? presentation?.attachments.filter { $0.phase == .attached }.count ?? 0 : 0,
            "daemon": ["state": connected ? "ready" : state, "name": "cmux-tui"],
            "detail": remoteConnectionDisplayDetail(catalog: catalog) ?? NSNull(),
            "detected_ports": resources.compactMap(\.port), "forwarded_ports": [Int](), "conflicted_ports": [Int](),
            "proxy": ["state": "on_demand", "transport": "cmux-tui"],
        ]
    }
}
