import CmuxFoundation
import Foundation

extension Workspace {
    /// Reads the autosave scanner's cached binding; rendering never inspects processes.
    func plainSSHConnectionPresentation(panelId: UUID) -> PlainSSHConnectionPresentation? {
        guard panels[panelId]?.panelType == .terminal,
              let binding = surfaceResumeBindingsByPanelId[panelId],
              binding.isPlainSSHProcessDetectedBinding,
              binding.launchFlavor == .local,
              let arguments = binding.launchCommand?.arguments
                ?? SurfaceResumeCommandCanonicalizer.tokens(from: binding.command),
              let session = TerminalSSHSessionDetector.parseSSHCommandLine(arguments),
              let host = WorkspaceHostLabel.ssh(destination: session.destination, port: session.port) else { return nil }
        return PlainSSHConnectionPresentation(
            host: host,
            isRestoring: pendingPlainSSHRestorePanelIds.contains(panelId)
        )
    }

    /// The selected terminal owns the workspace's displayed host in mixed layouts.
    var plainSSHWorkspaceConnectionPresentation: PlainSSHConnectionPresentation? {
        guard remoteConfiguration == nil, cloudVMID == nil,
              let focusedPanelId else { return nil }
        return plainSSHConnectionPresentation(panelId: focusedPanelId)
    }

    var plainSSHConnectionPresentations: [UUID: PlainSSHConnectionPresentation] {
        panels.keys.reduce(into: [:]) { result, panelId in
            result[panelId] = plainSSHConnectionPresentation(panelId: panelId)
        }
    }

    /// A local launch directory must never masquerade as a remote SSH directory.
    func plainSSHDirectory(panelId: UUID) -> String? {
        remoteDirectoryReportPanelIds.contains(panelId) ? panelDirectories[panelId] : nil
    }

    func refreshPlainSSHConnectionPresentation(previous: [UUID: PlainSSHConnectionPresentation]) {
        guard previous != plainSSHConnectionPresentations else { return }
        owningTabManager?.workspaceHostLabelDidChange(self)
        postRemoteConnectionPresentationDidChange()
    }

    func refreshPlainSSHFocusPresentation() {
        guard remoteConfiguration == nil, cloudVMID == nil,
              surfaceResumeBindingsByPanelId.values.contains(where: \.isPlainSSHProcessDetectedBinding) else { return }
        // Bonsplit can update its selected panel before the shared focus callback.
        // Refresh from the cached bindings even when the previous panel ID already
        // reflects the new selection or both panels report the same directory.
        owningTabManager?.workspaceHostLabelDidChange(self)
        postRemoteConnectionPresentationDidChange()
    }

    var sshTmuxSessionName: String? {
        guard let configuration = remoteConfiguration else { return nil }
        if let session = configuration.terminalProfile.tmuxSessionName { return session }
        guard let command = configuration.configuredRemoteCommand,
              SurfaceResumeCommandCanonicalizer.isShellExpansionSafeCommand(command),
              let arguments = SurfaceResumeCommandCanonicalizer.tokens(from: command) else { return nil }
        return ConfiguredTmuxSessionPresentation(arguments: arguments)?.sessionName
    }
}
