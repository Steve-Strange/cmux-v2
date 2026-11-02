import Foundation

/// Value-only terminal chrome. The process title stays separate so a host,
/// connection state, or directory never becomes authored terminal metadata.
struct TerminalConnectionPresentation {
    let origin: String
    let title: String
    let directory: String?
    let sessionName: String?

    var tabTitle: String {
        var fields = [origin, title]
        if let directory, !directory.isEmpty {
            let leaf = directory.split(separator: "/").last.map(String.init) ?? directory
            if leaf != title { fields.append(leaf) }
        }
        if let sessionName, !sessionName.isEmpty {
            fields.append(sessionDescription(sessionName))
        }
        return fields.joined(separator: " | ")
    }

    var detailLines: [String] {
        var lines: [String] = []
        if let sessionName, !sessionName.isEmpty { lines.append(sessionDescription(sessionName)) }
        if !title.isEmpty { lines.append(title) }
        if let directory, !directory.isEmpty { lines.append(directory) }
        return lines
    }

    private func sessionDescription(_ name: String) -> String {
        String(
            format: String(localized: "terminal.connection.tmuxSession", defaultValue: "tmux session: %@"),
            locale: .current,
            name
        )
    }
}
