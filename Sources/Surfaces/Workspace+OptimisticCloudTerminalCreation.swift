import CmuxCloud
import CmuxSurfaceCatalogModel
import Foundation

@MainActor
extension Workspace {
    /// The shared coordinator run behind every optimistic route: one request id,
    /// one remote create, projection adopting the reserved pane, and pane-local
    /// failure and retry. `onStart`/`onFinish` bracket the projection-suppression
    /// scope the caller chose.
    func runOptimisticCloudTerminalCreation(
        reservation: CloudTerminalPaneReservation,
        requestID: UUID,
        destination: SurfaceDestination,
        create: @escaping CloudTerminalCreationCoordinator.Create,
        onStart: @escaping @MainActor () -> Void,
        onFinish: @escaping @MainActor () -> Void
    ) {
        let catalog = SurfaceCatalog.shared
        let store = cloudPaneCreationFailureStore
        let project: CloudTerminalCreationCoordinator.Project = { [weak self, reservation] created in
            guard let self, !self.isRetiredFromOwningTabManager,
                  self.cloudPendingCreations[reservation.panelID] === reservation else {
                onFinish()
                throw CancellationError()
            }
            defer { onFinish() }
            // The stable terminal id is available before surface attachment.
            // Route reservation input into that PTY immediately so remote shell
            // startup owns echo and line discipline just like a local shell.
            if let cloudProvider = catalog.provider(for: created.machine) as? CmuxTuiSurfaceProvider {
                _ = try await cloudProvider.bindOptimisticTerminalInput(
                    reservation.inputRelay,
                    terminalID: created.id.key
                )
            }
            let remoteView = try reservation.sourcePlacement.remoteView(of: created)
            // The device layout can mirror the terminal here before its receipt
            // binds this reservation: a layout event that beats the create
            // response, or a retry after the first receipt was lost. A second
            // pane for the same terminal would leave that layout unable to
            // apply, so the reserved pane gives way to the mirrored one.
            if let existing = catalog.projections.first(where: {
                $0.workspaceID == self.id && $0.resource == created.id
                    && (remoteView == nil || $0.remoteTabID == remoteView?.tabID)
                    && self.panels[$0.panelID] != nil
            }) {
                self.completeReservedCloudTerminalPane(reservation, adoptedPanelID: existing.panelID)
                return (existing, true)
            }
            #if DEBUG
            cmuxDebugLog("cloud.split.project terminal=\(created.id.key)")
            #endif
            // Focus was granted when the pane appeared; adoption must not steal it
            // back from wherever the user has typed since.
            let result = try await CloudOperationContext.phase(.materialize) {
                try await catalog.project(
                    created.id,
                    into: destination,
                    focus: false,
                    reuseExisting: false,
                    remoteView: remoteView,
                    adopting: reservation
                )
            }
            self.completeReservedCloudTerminalPane(reservation, adoptedPanelID: result.projection.panelID)
            #if DEBUG
            cmuxDebugLog("cloud.split.projected terminal=\(created.id.key)")
            #endif
            return result
        }
        reservation.retry = { [weak store] in store?.retry(requestID: requestID) }
        reservation.cancel = { [weak store] in store?.cancel(requestID: requestID) }
        store.run(
            machine: reservation.machine,
            requestID: requestID,
            create: {
                do {
                    let created = try await create()
                    try Task.checkCancellation()
                    try reservation.sourcePlacement.validate(created: created)
                    reservation.creationReceipt.finish(.success(created))
                    return created
                } catch {
                    let failure: Error = CloudDiagnosticFailure.classify(error) == .cancelled ? CloudDiagnosticFailure.placement : error
                    reservation.creationReceipt.finish(.failure(failure))
                    throw error
                }
            },
            project: project,
            onStart: { [weak self, reservation] in
                reservation.creationReceipt.beginAttempt()
                onStart()
                if let self, !self.isRetiredFromOwningTabManager,
                   self.cloudPendingCreations[reservation.panelID] === reservation,
                   let use = try? catalog.beginExplicitSSHTransportUse(on: [reservation.machine], in: self.id) {
                    catalog.endExplicitSSHTransportUse(use)
                }
                self?.restartReservedCloudTerminalPane(reservation)
                (catalog.provider(for: reservation.machine) as? CmuxTuiSurfaceProvider)?
                    .beginAttachHandshake(for: reservation)
            },
            onFinish: onFinish,
            inlineFailure: { [weak self, reservation] error in
                self?.failReservedCloudTerminalPane(reservation, error: error)
            },
            discardProjection: { projection in
                catalog.endProjections(panelID: projection.panelID, reason: .replaced)
            },
            operations: AppDelegate.shared?.cloudOperations
        )
    }

}
