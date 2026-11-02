import CmuxCloud
import CmuxSurfaceCatalogModel
import Foundation

extension SurfaceCatalog {
    typealias ExplicitSSHTransportUses = [(provider: CmuxTuiSurfaceProvider, token: UUID)]

    /// A deliberate open/create owns its carrier until its projection can own it.
    func beginExplicitSSHTransportUse(on machines: Set<SurfaceMachineID>, in workspaceID: UUID? = nil) throws -> ExplicitSSHTransportUses {
        let sshMachines = machines.filter(\.isSSH)
        guard !sshMachines.isEmpty else { return [] }
        try Task.checkCancellation()
        return sshMachines.compactMap { machine in
            guard let provider = provider(for: machine) as? CmuxTuiSurfaceProvider else { return nil }
            if let workspaceID,
               let workspace = cloudWorkspaceRenameService.environment.workspace(workspaceID)
                ?? Workspace.liveWorkspace(id: workspaceID),
               workspace.usesSSHTui, let configuration = workspace.remoteConfiguration,
               SSHTuiConnection(configuration: configuration).id == machine.rawValue {
                workspace.remoteConnectionResumeEnabled = true
            }
            guard let token = provider.beginExplicitSSHTransportUse() else { return nil }
            return (provider, token)
        }
    }

    func endExplicitSSHTransportUse(_ uses: ExplicitSSHTransportUses) {
        for use in uses { use.provider.endExplicitSSHTransportUse(use.token) }
    }
}
