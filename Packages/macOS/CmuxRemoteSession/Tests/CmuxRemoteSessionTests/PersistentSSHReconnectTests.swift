import CmuxCore
import CmuxFoundation
import CmuxRemoteDaemon
import CmuxRemoteWorkspace
import Foundation
import Testing
@testable import CmuxRemoteSession

@MainActor
@Suite("Persistent SSH automatic recovery")
struct PersistentSSHReconnectTests {
    @Test("A persistent SSH workspace keeps its retry after repeated unreachable probes")
    func persistentWorkspaceKeepsRetryingOffline() {
        let coordinator = makeCoordinator()
        defer { coordinator.stop(); coordinator.queue.sync {} }

        let schedules = coordinator.queue.sync {
            (0..<12).map { _ in
                let schedule = coordinator.scheduleReconnectLocked(baseDelay: 4)
                coordinator.handleReachabilityProbeOutcomeLocked(
                    .unreachable(reason: "No route to host"),
                    generation: coordinator.reachabilityProbeGeneration
                )
                return schedule
            }
        }

        #expect(coordinator.queue.sync { !coordinator.reconnectSuspended })
        #expect(coordinator.queue.sync { coordinator.reconnectToken != nil })
        #expect(schedules.map(\.delay) == [4, 8, 16, 32, 60, 60, 60, 60, 60, 60, 60, 60])
    }

    @Test("A persistent workspace does not exhaust its bootstrap budget while the network is down")
    func networkFailureDoesNotExhaustBootstrapBudget() {
        let coordinator = makeCoordinator(
            runner: PersistentSSHFailureProcessRunner(detail: "ssh: connect to host example.test port 22: Network is unreachable")
        )
        defer { coordinator.stop(); coordinator.queue.sync {} }

        coordinator.queue.sync {
            for _ in 0..<12 { coordinator.beginConnectionAttemptLocked() }
        }

        #expect(coordinator.queue.sync { !coordinator.reconnectSuspended })
        #expect(coordinator.queue.sync { coordinator.reconnectToken != nil })
        #expect(coordinator.queue.sync { coordinator.bootstrapFailureTotal == 0 })
    }

    @Test("A wake or network signal keeps an authentication failure parked")
    func authenticationRequiresManualReconnect() {
        let coordinator = makeCoordinator()
        defer { coordinator.stop(); coordinator.queue.sync {} }
        coordinator.queue.sync {
            coordinator.parkSessionLocked(cause: .sshAuthenticationFailed, daemonState: .error, detail: "Permission denied (publickey).")
            coordinator.prepareForSystemSleepLocked()
        }

        coordinator.resetReconnectPolicyAndReconnect(reason: "network became available")
        coordinator.queue.sync {}

        #expect(coordinator.queue.sync { coordinator.parkedState?.cause == .sshAuthenticationFailed })
        #expect(coordinator.queue.sync { coordinator.reconnectToken == nil })
        #expect(coordinator.queue.sync { !coordinator.isSystemSleeping })
    }

    @Test("A bootstrap authentication failure stays paused when the network returns")
    func bootstrapAuthenticationRequiresManualReconnect() {
        let coordinator = makeCoordinator(
            runner: PersistentSSHFailureProcessRunner(detail: "user@example.test: Permission denied (publickey).")
        )
        defer { coordinator.stop(); coordinator.queue.sync {} }
        coordinator.queue.sync { coordinator.beginConnectionAttemptLocked() }
        #expect(coordinator.queue.sync { coordinator.reconnectSuspended })

        coordinator.resetReconnectPolicyAndReconnect(reason: "system woke")
        coordinator.queue.sync {}

        #expect(coordinator.queue.sync { coordinator.reconnectSuspended })
        #expect(coordinator.queue.sync { coordinator.reconnectToken == nil })
    }

    @Test("Stopping a workspace prevents later network signals from restarting it")
    func stoppedWorkspaceDoesNotReconnect() {
        let coordinator = makeCoordinator()
        coordinator.stop()
        coordinator.queue.sync {}

        coordinator.resetReconnectPolicyAndReconnect(reason: "network became available")
        coordinator.queue.sync {}

        #expect(coordinator.queue.sync { coordinator.isStopping })
        #expect(coordinator.queue.sync { coordinator.reconnectToken == nil })
        #expect(coordinator.queue.sync { coordinator.connectionAttemptTask == nil })
    }

    @Test("System sleep cancels an attempt still waiting for its host broker")
    func sleepCancelsQueuedConnectionAttempt() {
        let coordinator = makeCoordinator()
        defer { coordinator.stop(); coordinator.queue.sync {} }

        coordinator.queue.sync {
            coordinator.requestConnectionAttemptLocked()
            coordinator.prepareForSystemSleepLocked()
        }

        #expect(coordinator.queue.sync { coordinator.isSystemSleeping })
        #expect(coordinator.queue.sync { coordinator.connectionAttemptTask == nil })
        #expect(coordinator.queue.sync { coordinator.connectionAttemptToken == nil })
    }

    @Test("A network outage pauses the readiness deadline until the host is reachable")
    func offlineTimeDoesNotExhaustReadinessDeadline() {
        let coordinator = makeCoordinator()
        defer { coordinator.stop(); coordinator.queue.sync {} }

        coordinator.queue.sync {
            coordinator.proxyConnectionDesired = true
            coordinator.daemonReady = true
            coordinator.armReadinessDeadlineLocked()
            coordinator.scheduleReconnectLocked(baseDelay: 4)
            coordinator.handleReachabilityProbeOutcomeLocked(
                .unreachable(reason: "No route to host"), generation: coordinator.reachabilityProbeGeneration
            )
        }
        #expect(coordinator.queue.sync { coordinator.readinessDeadlineToken == nil })
        #expect(coordinator.queue.sync { !coordinator.reconnectSuspended })

        coordinator.queue.sync {
            coordinator.handleReachabilityProbeOutcomeLocked(
                .reachable, generation: coordinator.reachabilityProbeGeneration
            )
        }
        #expect(coordinator.queue.sync { coordinator.readinessDeadlineToken != nil })
    }

    @Test("Recognized SSH network failures keep persistent recovery armed", arguments: [
        "Network is unreachable", "Connection timed out", "ssh timed out after 20s",
        "Connection refused", "Connection reset by peer", "client_loop: send disconnect: Broken pipe",
        "Can't assign requested address", "Could not resolve hostname host: nodename nor servname provided"
    ])
    func recognizedNetworkFailuresKeepRetrying(detail: String) {
        let policy = RemoteBootstrapRetryPolicy()
        let fingerprint = RemoteBootstrapRetryPolicy.fingerprint(for: NSError(
            domain: "cmux.remote.daemon", code: 11, userInfo: [NSLocalizedDescriptionKey: detail]
        ))
        let evaluation = policy.evaluate(
            fingerprint: fingerprint, previousFingerprint: fingerprint,
            previousConsecutiveFailures: 12, previousTotalFailures: 12,
            keepsRecoveringOffline: true
        )
        #expect(evaluation.decision == .retry)
        #expect(evaluation.totalFailures == 12)
    }

    @Test("Authentication and corrupt installs still pause a persistent workspace", arguments: [
        "Permission denied (publickey).", "Host key verification failed.", "daemon checksum mismatch",
        "daemon checksum mismatch after connection reset"
    ])
    func nonNetworkFailuresStayBounded(detail: String) {
        let policy = RemoteBootstrapRetryPolicy()
        let fingerprint = RemoteBootstrapRetryPolicy.fingerprint(for: NSError(
            domain: "cmux.remote.daemon", code: 11, userInfo: [NSLocalizedDescriptionKey: detail]
        ))
        let evaluation = policy.evaluate(
            fingerprint: fingerprint, previousFingerprint: fingerprint,
            previousConsecutiveFailures: 12, previousTotalFailures: 12,
            keepsRecoveringOffline: true
        )
        #expect(evaluation.decision == .suspend)
    }

    private func makeCoordinator(
        runner: any RemoteSessionProcessRunning = IntentionalCleanupUnusedProcessRunner()
    ) -> RemoteSessionCoordinator {
        let effectiveRunner = ResolvedControlPathProcessRunner(base: runner)
        let broker = NativeSSHConnectionBroker(
            sharingOptions: SSHConnectionSharingOptions(),
            clock: RecordingImmediateClock(),
            jitterMilliseconds: { 200 },
            cleanupLauncher: { _ in },
            inheritedMasterReapRunner: effectiveRunner,
            controlMasterOwnershipRegistry: PermissiveNativeSSHControlMasterOwnershipRegistry()
        )
        return RemoteSessionCoordinator(
            host: IntentionalCleanupTestHost(),
            configuration: broker.retainWorkspace(WorkspaceRemoteConfiguration(
                destination: "user@example.test", port: nil, identityFile: nil, sshOptions: [],
                localProxyPort: nil, relayPort: nil, relayID: nil, relayToken: nil,
                localSocketPath: nil, terminalStartupCommand: nil,
                preserveAfterTerminalExit: true, persistentDaemonSlot: "persistent-test"
            )),
            proxyBroker: SSHOverrideUnusedRemoteProxyBroker(),
            connectionBroker: broker,
            manifestRepository: RemoteDaemonManifestRepository(homeDirectory: FileManager.default.temporaryDirectory),
            processRunner: effectiveRunner,
            reachabilityProbe: IntentionalCleanupNoopReachabilityProbe(),
            relayCommandRewriter: IntentionalCleanupRelayCommandRewriter(),
            buildInfo: IntentionalCleanupBuildInfo(),
            daemonStrings: RemoteDaemonStrings(
                missingPersistentPTYCapability: "", missingRequiredFunctionality: "",
                cloudNotificationClearWorkspaceInvalid: "", cloudNotificationClearWorkspaceDenied: "",
                cloudNotificationClearSurfaceInvalid: ""
            ),
            strings: RemoteSessionStrings(
                connectedVMNoProxyFormat: "%@", suspendedDetailFormat: "%@",
                reverseRelayUnavailableRetrying: "", reverseRelayPortUnavailableRetrying: "",
                controlMasterOwnershipUnavailable: "test control master unavailable"
            ),
            clock: ManualBrokerClock()
        )
    }
}
