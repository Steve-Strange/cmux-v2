import CmuxCore
import Foundation

extension WorkspaceRemoteConnectionState {
    /// Shared by workspace rows and individual terminal tabs.
    var displayLabel: String {
        switch self {
        case .connected:
            return String(localized: "remote.status.connected", defaultValue: "Connected")
        case .connecting:
            return String(localized: "remote.status.connecting", defaultValue: "Connecting")
        case .reconnecting:
            return String(localized: "remote.status.reconnecting", defaultValue: "Reconnecting")
        case .error:
            return String(localized: "remote.status.error", defaultValue: "Error")
        case .disconnected:
            return String(localized: "remote.status.disconnected", defaultValue: "Disconnected")
        case .suspended:
            return String(localized: "remote.status.suspended", defaultValue: "Unreachable")
        }
    }
}
