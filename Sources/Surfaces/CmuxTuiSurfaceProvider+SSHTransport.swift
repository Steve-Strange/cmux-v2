import CmuxCloud
import CmuxSurfaceCatalogModel
import Foundation

extension CmuxTuiSurfaceProvider {
    var isSSHTransportPaused: Bool {
        (links as? SSHTuiLinkManager)?.transportIntent.isPaused ?? false
    }

    /// Explicit Open/Retry re-enables the carrier and its retired notification owner.
    func resumeSSHTransport() {
        guard let manager = links as? SSHTuiLinkManager else { return }
        manager.transportIntent.resume()
        resumeSSHBrowserRoutes()
        if notificationSync == nil { installNotificationSync() }
    }

    /// Preflight has no workspace yet, so it owns the shared carrier temporarily.
    func beginExplicitSSHTransportUse() -> UUID? {
        guard let manager = links as? SSHTuiLinkManager else { return nil }
        let token = manager.transportIntent.beginExplicitUse()
        resumeSSHTransport()
        return token
    }

    func endExplicitSSHTransportUse(_ token: UUID) {
        guard let manager = links as? SSHTuiLinkManager else { return }
        manager.transportIntent.endExplicitUse(token)
        pauseSSHTransportIfUnused()
    }

    /// Detaches only an unused local carrier; the daemon and saved graph survive.
    @discardableResult
    func pauseSSHTransportIfUnused(excluding workspaceID: UUID? = nil) -> Task<Void, Never>? {
        guard let manager = links as? SSHTuiLinkManager else { return nil }
        let forced = !ManagedRemoteConnectionsPolicy.isEnabled
        guard forced || !hasActiveSSHTransportConsumers(excluding: workspaceID),
              let revision = manager.transportIntent.pause(force: forced) else { return nil }
        pauseSSHBrowserRoutes()
        stopTransportResources()
        attachmentRetry.cancel()
        for (panelID, task) in restoredAttachTasks {
            task.cancel()
            if manualMirrorSessions[panelID] == nil { materializedPanels.remove(panelID) }
        }
        restoredAttachTasks.removeAll()
        publishLinkStatus(.unavailable, error: nil)
        return Task { await manager.disconnect(ifPausedAt: revision) }
    }

    private func hasActiveSSHTransportConsumers(excluding workspaceID: UUID?) -> Bool {
        if manualMirrorSessions.values.contains(where: \.allowsAutomaticReconnect) { return true }
        // A workspace can be connecting before its first projection exists.
        let workspaces = AppDelegate.shared?.surfaceCatalogWorkspaces() ?? []
        if workspaces.contains(where: { workspace in
            guard workspace.id != workspaceID, !workspace.isRetiredFromOwningTabManager,
                  workspace.usesSSHTui, workspace.remoteConnectionResumeEnabled,
                  let configuration = workspace.remoteConfiguration else { return false }
            return SSHTuiConnection(configuration: configuration).id == machineID
        }) { return true }
        // Independent terminals, browsers and port previews can live in a local
        // workspace, sharing this carrier with managed SSH workspaces.
        return catalog.projections.contains { projection in
            guard projection.resource.machine == machine, projection.workspaceID != workspaceID,
                  let workspace = catalog.cloudWorkspaceRenameService.environment.workspace(projection.workspaceID)
                    ?? Workspace.liveWorkspace(id: projection.workspaceID),
                  !workspace.isRetiredFromOwningTabManager else { return false }
            return !workspace.usesSSHTui || workspace.remoteConnectionResumeEnabled
        }
    }
}
