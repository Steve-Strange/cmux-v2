import CmuxCore
import Foundation

extension Workspace {
    /// Decorates only tab chrome; `panelTitle` and persisted names retain the
    /// shell/user title for naming, restore, and remote rename ownership.
    func tabDisplayTitle(panelId: UUID, fallback: String) -> String {
        let title = resolvedPanelTitle(panelId: panelId, fallback: fallback)
        guard panels[panelId]?.panelType == .terminal,
              remoteConfiguration?.transport == .ssh,
              !isRemoteTmuxMirror else { return title }
        let isRemote = machineOwningSurface(panelId)?.isSSH == true
            || isRemoteTerminalSurface(panelId)
            || remoteDisconnectPlaceholderPanelIds.contains(panelId)
            || endedPersistentRemotePTYAttachSurfaceIds.contains(panelId)
        let origin: String
        if isRemote {
            let state = terminalConnectionDisplayState(panelId: panelId)
            let label = state == .connected
                ? String(localized: "terminal.connection.remote", defaultValue: "Remote")
                : state.displayLabel
            origin = [label, remoteDisplayTarget].compactMap { $0 }.joined(separator: " · ")
        } else {
            origin = String(localized: "commandPalette.currentWork.local", defaultValue: "Local")
        }
        return TerminalConnectionPresentation(
            origin: origin,
            title: title,
            directory: effectivePanelDirectory(panelId: panelId),
            sessionName: isRemote ? remoteConfiguration?.terminalProfile.tmuxSessionName : nil
        ).tabTitle
    }

    /// A sibling's connected link does not establish that this pane attached.
    private func terminalConnectionDisplayState(panelId: UUID) -> WorkspaceRemoteConnectionState {
        if let mirror = tuiMirrorSession(for: panelId) {
            switch mirror.phase {
            case .attached: return .connected
            case .connecting: return remoteConnectionState == .reconnecting ? .reconnecting : .connecting
            case .idle, .stopped: return .disconnected
            case .disconnected:
                return remoteConnectionState == .connected ? .disconnected : remoteConnectionState
            }
        }
        let phase = remoteTerminalSessionStatesBySurfaceId[panelId]?.phase
        if phase == .ended { return .disconnected }
        if remoteConnectionState == .connected,
           phase == .launching || cloudPendingCreations[panelId] != nil { return .connecting }
        return remoteConnectionState
    }

    /// Reprojects tabs at connection/configuration boundaries, without writing
    /// title metadata or adding another connection-state source.
    func refreshTerminalConnectionTitles() {
        guard !isRemoteTmuxMirror else { return }
        for (panelId, panel) in panels where panel.panelType == .terminal {
            refreshTerminalConnectionTitle(panelId: panelId)
        }
    }

    func refreshTerminalConnectionTitle(panelId: UUID) {
        guard panels[panelId]?.panelType == .terminal,
              let tabId = surfaceIdFromPanelId(panelId),
              let panel = panels[panelId] else { return }
        let next = tabDisplayTitle(panelId: panelId, fallback: panelTitles[panelId] ?? panel.displayTitle)
        if bonsplitController.tab(tabId)?.title != next {
            bonsplitController.updateTab(tabId, title: next)
        }
    }
}
