internal import Foundation

extension RemoteSessionCoordinator {
    /// Pauses reconnect-policy accounting while the local Mac is asleep.
    public func prepareForSystemSleep() {
        queue.async { [weak self] in
            self?.prepareForSystemSleepLocked()
        }
    }

    /// Re-arms automatic transport recovery after an external signal.
    ///
    /// A reconnect is scheduled when the coordinator was suspended or no longer
    /// has a ready proxy. Healthy connections are left in place; a transport
    /// failure delivered after wake will schedule through the freshly reset policy.
    /// Authentication and host-key failures still require a manual Reconnect.
    ///
    /// - Parameter reason: A diagnostic label for the re-arm signal.
    public func resetReconnectPolicyAndReconnect(reason: String) {
        queue.async { [weak self] in
            guard let self else { return }
            let shouldReconnect = self.resetReconnectPolicyLocked(reason: reason)
            guard shouldReconnect else { return }
            self.resetTransportForReconnectLocked()
            _ = self.scheduleReconnectLocked(baseDelay: 2.0)
        }
    }

    func prepareForSystemSleepLocked() {
        guard !isStopping else { return }
        isSystemSleeping = true
        cancelConnectionAttemptLocked()
        cancelReconnectRetryLocked()
        // The deadline measures time spent seeking readiness, not time asleep;
        // the post-wake reconnect arms a fresh one.
        cancelReadinessDeadlineLocked()
        reachabilityProbeGeneration &+= 1
        debugLog("remote.session.systemSleep \(debugConfigSummary())")
    }

    @discardableResult
    func resetReconnectPolicyLocked(reason: String) -> Bool {
        guard !isStopping else { return false }
        isSystemSleeping = false
        // A network transition cannot unlock a key, complete MFA, or approve a
        // changed host key. Only the explicit Reconnect path replaces this
        // coordinator and releases its authentication verdict.
        if parkedState?.cause == .sshAuthenticationFailed ||
            (parkedState?.cause == .bootstrapFailed && bootstrapFailureFingerprint?.hasSuffix(":blocked") == true) {
            return false
        }
        let expectsProxyEndpoint = !configuration.skipDaemonBootstrap ||
            configuration.daemonWebSocketEndpoint != nil
        let shouldReconnect = reconnectSuspended || reconnectRetryCount > 0 ||
            (expectsProxyEndpoint && proxyEndpoint == nil) || !daemonReady
        cancelReconnectRetryLocked()
        reconnectRetryCount = 0
        consecutiveUnreachableProbeCount = 0
        resetBootstrapFailureTrackingLocked()
        endReadinessSeekLocked()
        reachabilityProbeGeneration &+= 1
        debugLog(
            "remote.session.reconnect.rearmed reason=\(reason.debugLogSnippet(limit: 80)) " +
                "reconnect=\(shouldReconnect ? 1 : 0) \(debugConfigSummary())"
        )
        return shouldReconnect
    }

    func resetTransportForReconnectLocked(
        preservePersistentRelayMetadata: Bool = false
    ) {
        cancelTransportDependentWorkLocked()
        cancelReverseRelayRestartLocked()
        if preservePersistentRelayMetadata {
            invalidateReverseRelayAfterControlMasterReapLocked()
        } else {
            stopReverseRelayLocked()
        }
        // Wait-for-ready bridge requests belong to the persistent remote PTY,
        // not to this particular local transport lease. Leave them parked so
        // a sleep/wake or transient reconnect does not manufacture a failed
        // attach that the pane immediately has to retry.
        releaseProxyLeaseLocked()
        proxyEndpoint = nil
        daemonReady = false
        daemonBootstrapVersion = nil
        daemonRemotePath = nil
        publishProxyEndpoint(nil)
    }

    private func cancelTransportDependentWorkLocked() {
        bootstrapRemoteTTYResolved = false
        bootstrapRemoteTTYFetchInFlight = false
        suspendRemotePortScanningLocked()
    }

    func releaseProxyLeaseLocked() {
        proxyLeaseGeneration &+= 1
        proxyLease?.release()
        proxyLease = nil
    }
}
