import Foundation

/// Shares synchronous UI intent with the SSH carrier's asynchronous owner.
@MainActor
final class SSHTuiTransportIntent {
    nonisolated init() {}

    private(set) var isPaused = false
    private(set) var revision: UInt64 = 0
    private var explicitUses: Set<UUID> = []

    /// A new Open owns the carrier before its workspace has been created.
    func beginExplicitUse() -> UUID {
        let token = UUID()
        explicitUses.insert(token)
        resume()
        return token
    }

    func endExplicitUse(_ token: UUID) { explicitUses.remove(token) }

    func resume() {
        guard isPaused else { return }
        revision &+= 1
        isPaused = false
    }

    /// Returns the teardown generation, unless a new Open still owns the link.
    func pause(force: Bool = false) -> UInt64? {
        guard force || explicitUses.isEmpty else { return nil }
        if !isPaused {
            revision &+= 1
            isPaused = true
        }
        return revision
    }

    func connectionRevision() throws -> UInt64 {
        try Task.checkCancellation()
        guard !isPaused else { throw CancellationError() }
        return revision
    }

    func isPaused(at expected: UInt64) -> Bool { isPaused && revision == expected }
}
