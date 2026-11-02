import CmuxCloud
import Foundation

// The production extension runs against a value-only host. Network dials are
// observable counters; the retry scheduler and refresh coordinator are real.
struct CloudMachineLink {
    struct Connected { let socketPath: String }
}

enum FixtureResolution {
    case retryable(String)
}

enum FixturePhase {
    case idle, disconnected
}

@MainActor
final class FixtureSession {
    let terminalID = "term_fixture"
    var allowsAutomaticReconnect = true
    let phase = FixturePhase.disconnected
    func updateRemoteSurfaceID(_ id: UInt64) {}
    func markSurfaceResolutionUnavailable(reason: FixtureInterruption) {}
    func reconnect(socketPath: String) {}
}

enum FixtureInterruption {
    case unresolved(String)
}

enum CloudAttachmentReconcileDecision {
    case rebind(UInt64), exited, fence(FixtureInterruption), keep
    static func decide(phase: FixturePhase, resolution: FixtureResolution) -> Self { .keep }
}

struct FixtureLog {
    func retry(machineID: String, failures: Int, delay: Duration) {}
    func resolution(machineID: String, terminalID: String, attempt: Int, outcome: FixtureResolution) {}
}

@MainActor
final class CmuxTuiSurfaceProvider {
    struct Machine { let isSSH: Bool }
    let machine: Machine
    let machineID = "fixture"
    let attachmentLog = FixtureLog()
    let attachmentRetry: CloudTerminalAttachmentRetryScheduler
    let refreshCoordinator = CloudProviderRefreshCoordinator()
    var manualMirrorSessions: [UUID: FixtureSession] = [:]
    var manualMirrorSurfaceIDsSocketPath: String?
    var currentLifecycleGeneration: UInt64 = 0
    var refreshCount = 0
    var registered = true

    init(isSSH: Bool, clock: CloudReadManualClock) {
        machine = Machine(isSSH: isSSH)
        attachmentRetry = CloudTerminalAttachmentRetryScheduler(clock: clock)
    }

    func isCurrentLifecycleGeneration(_ generation: UInt64) -> Bool {
        generation == currentLifecycleGeneration
    }
    func isRegisteredInCatalog() -> Bool { registered }
    func isCurrentRefresh(lifecycle: UInt64, refresh: UInt64) -> Bool { true }
    func closePanes(forExitedTerminals ids: Set<String>) {}
    func resolveManualMirrorSessions(
        _ sessions: [FixtureSession], socketPath: String, link: CloudMachineLink
    ) async -> [String: FixtureResolution] { [:] }

    func scheduleRefresh() {
        Task { @MainActor in
            await Task.yield()
            _ = await refreshCoordinator.refresh(force: false) { force in
                await self.performRefresh(force: force)
            }
        }
    }

    func performRefresh(force: Bool) async -> Bool {
        refreshCount += 1
        return true
    }
}

@main
struct SSHAttachmentReconnectIntentFixture {
    @MainActor
    static func main() async {
        let activeRetries = await retriesAfterTimer(isSSH: true, pause: false, remove: false)
        let pausedRetries = await retriesAfterTimer(isSSH: true, pause: true, remove: false)
        let closedRetries = await retriesAfterTimer(isSSH: true, pause: false, remove: true)
        let cloudRetries = await retriesAfterTimer(isSSH: false, pause: true, remove: false)
        let resumedRetries = await resumeAfterPausedTimer()
        let results = [
            ("active SSH retries", activeRetries, 1),
            ("paused SSH stays disconnected", pausedRetries, 0),
            ("closed SSH pane stays closed", closedRetries, 0),
            ("Cloud retry behavior stays intact", cloudRetries, 1),
            ("explicit SSH resume retries", resumedRetries, 1),
        ]
        var failed = false
        for (name, actual, expected) in results {
            print("\(actual == expected ? "PASS" : "FAIL") \(name): \(actual), expected \(expected)")
            failed = failed || actual != expected
        }
        if failed { exit(1) }
    }

    @MainActor
    private static func retriesAfterTimer(isSSH: Bool, pause: Bool, remove: Bool) async -> Int {
        let clock = CloudReadManualClock()
        let provider = CmuxTuiSurfaceProvider(isSSH: isSSH, clock: clock)
        let session = FixtureSession()
        provider.manualMirrorSessions[UUID()] = session
        provider.scheduleAttachmentRetry()
        await settle { clock.pendingSleeperCount == 1 }
        session.allowsAutomaticReconnect = !pause
        if remove { provider.manualMirrorSessions.removeAll() }
        clock.advance(by: .seconds(1))
        await settle { !provider.attachmentRetry.isPending }
        // Flush the refresh owner's actor tasks after the timer fired.
        for _ in 0..<100 { await Task.yield() }
        return provider.refreshCount
    }

    @MainActor
    private static func resumeAfterPausedTimer() async -> Int {
        let clock = CloudReadManualClock()
        let provider = CmuxTuiSurfaceProvider(isSSH: true, clock: clock)
        let session = FixtureSession()
        provider.manualMirrorSessions[UUID()] = session
        provider.scheduleAttachmentRetry()
        await settle { clock.pendingSleeperCount == 1 }
        session.allowsAutomaticReconnect = false
        clock.advance(by: .seconds(1))
        await settle { !provider.attachmentRetry.isPending }
        for _ in 0..<100 { await Task.yield() }
        session.allowsAutomaticReconnect = true
        provider.scheduleAttachmentRetry()
        await settle { clock.pendingSleeperCount == 1 }
        clock.advance(by: .seconds(2))
        await settle { !provider.attachmentRetry.isPending }
        for _ in 0..<100 { await Task.yield() }
        return provider.refreshCount
    }

    @MainActor
    private static func settle(until condition: @MainActor () -> Bool) async {
        for _ in 0..<10_000 {
            if condition() { return }
            await Task.yield()
        }
        fatalError("retry actor tasks did not settle")
    }
}
