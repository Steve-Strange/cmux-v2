import CmuxCloud
import CmuxSurfaceCatalogModel
import Foundation

extension CmuxTuiSurfaceProvider {
    func pauseSSHBrowserRoutes() {
        portAccessStore.invalidateTransport(machineID: machineID)
    }

    /// A carrier can be shared with paused workspaces; resume only its live viewers.
    func resumeSSHBrowserRoutes() {
        let activeModels = Set(catalog.projections.compactMap { projection -> ObjectIdentifier? in
            guard projection.resource.machine == machine,
                  let workspace = catalog.cloudWorkspaceRenameService.environment.workspace(projection.workspaceID)
                    ?? Workspace.liveWorkspace(id: projection.workspaceID),
                  !workspace.isRetiredFromOwningTabManager,
                  !workspace.usesSSHTui || workspace.remoteConnectionResumeEnabled,
                  let browser = workspace.panels[projection.panelID] as? BrowserPanel,
                  let model = browser.cloudAccess.model else { return nil }
            return ObjectIdentifier(model)
        })
        portAccessStore.resumeTransport(machineID: machineID, activeModels: activeModels)
    }
}
