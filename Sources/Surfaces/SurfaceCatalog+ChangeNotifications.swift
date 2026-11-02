import CmuxSurfaceCatalogModel
import Foundation

@MainActor
extension SurfaceCatalog {
    func notifyChange(for machine: SurfaceMachineID? = nil) {
        if let machine { pendingChangedMachines.insert(machine) } else { pendingGlobalChange = true }
        guard !changeNotificationPending else { return }
        changeNotificationPending = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.changeNotificationPending = false
            let machines = self.pendingChangedMachines
            self.pendingChangedMachines.removeAll()
            let global = self.pendingGlobalChange
            self.pendingGlobalChange = false
            for workspace in self.cloudWorkspaceRenameService.environment.workspaces() {
                guard let machine = workspace.sshPresentationMachine,
                      global || machines.isEmpty || machines.contains(machine) else { continue }
                workspace.postRemoteConnectionPresentationDidChange()
            }
            NotificationCenter.default.post(
                name: Self.didChangeNotification,
                object: self,
                userInfo: global || machines.isEmpty ? nil : ["machines": machines.map(\.rawValue)]
            )
        }
    }
}
