import CmuxCloud
import CmuxCore
import CmuxRemoteSession
import CmuxSurfaceCatalogModel

extension Workspace {
    var sshPresentationMachine: SurfaceMachineID? {
        guard usesSSHTui, let configuration = remoteConfiguration else { return nil }
        return SurfaceMachineID(rawValue: SSHTuiConnection(configuration: configuration).id)
    }

    /// Legacy transports retain their controller presentation; TUI SSH reads live facts.
    func remoteConnectionDisplayState(catalog: SurfaceCatalog = .shared) -> WorkspaceRemoteConnectionState {
        sshConnectionPresentation(catalog: catalog)?.state ?? remoteConnectionState
    }

    func remoteConnectionDisplayDetail(catalog: SurfaceCatalog = .shared) -> String? {
        guard usesSSHTui else { return remoteConnectionDetail }
        guard remoteConnectionResumeEnabled else { return nil }
        return sshMachineInfo(catalog: catalog)?.linkError ?? remoteConnectionDetail
    }

    func sshConnectionPresentation(catalog: SurfaceCatalog = .shared) -> SSHConnectionPresentation? {
        guard let machine = sshPresentationMachine else { return nil }
        let info = catalog.machineInfo(for: machine)
        let provider = catalog.provider(for: machine) as? CmuxTuiSurfaceProvider
        let attachments = provider?.manualMirrorSessions.compactMap { panelID, session -> SSHConnectionPresentation.Attachment? in
            guard panels[panelID] != nil else { return nil }
            return .init(phase: session.phase, allowsAutomaticReconnect: session.allowsAutomaticReconnect)
        } ?? []
        let detail = info?.linkError ?? remoteConnectionDetail ?? ""
        return SSHConnectionPresentation(
            resumeEnabled: remoteConnectionResumeEnabled,
            controllerState: remoteConnectionState,
            carrierConnected: info.map { $0.linkState == .connected },
            requiresManualAuthentication: RemoteRelayAuthenticationFailure.detect(in: detail) != nil,
            attachments: attachments
        )
    }

    private func sshMachineInfo(catalog: SurfaceCatalog) -> SurfaceMachineInfo? {
        guard let machine = sshPresentationMachine else { return nil }
        return catalog.machineInfo(for: machine)
    }
}
