import CmuxFoundation
import Foundation

/// A cached SSH process observation names its host without claiming a managed remote PTY.
struct PlainSSHConnectionPresentation: Equatable {
    let host: WorkspaceHostLabel
    let isRestoring: Bool

    var statusText: String {
        if isRestoring {
            return String(localized: "remote.status.connecting", defaultValue: "Connecting")
        }
        return String(localized: "terminal.connection.sshOnly", defaultValue: "SSH only")
    }

    var origin: String {
        let transport = String(localized: "menu.help.ssh", defaultValue: "SSH")
        return [transport, host.label, statusText].joined(separator: " · ")
    }

    var helpText: String {
        String(localized: "terminal.connection.sshOnly.help", defaultValue: "This terminal uses ordinary SSH. Restoring reconnects SSH only; remote tmux sessions and terminals are not managed.")
    }
}
